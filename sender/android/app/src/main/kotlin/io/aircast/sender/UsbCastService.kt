package io.aircast.sender

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.DisplayMetrics
import android.util.Log
import android.view.Surface
import android.view.WindowManager
import java.io.OutputStream
import kotlin.concurrent.thread

/**
 * USB cast path: VirtualDisplay -> MediaCodec (Surface input, zero copy) ->
 * Annex-B H.264 -> an abstract-namespace local socket the desktop reaches with
 * `adb forward tcp:<port> localabstract:<name>`.
 *
 * Deliberately not WebRTC: adb forwarding carries TCP only, and over a cable
 * there is nothing for congestion control to adapt to.
 */
class UsbCastService : Service() {

    private var projection: MediaProjection? = null
    private var codec: MediaCodec? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var serverSocket: LocalServerSocket? = null
    private var wakeLock: PowerManager.WakeLock? = null

    @Volatile
    private var running = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopCasting()
                stopSelf()
            }

            ACTION_START -> start(intent)

            // Nothing but the notification, for the WebRTC path. flutter_webrtc
            // calls getMediaProjection() itself and starts no service of its
            // own, and since targetSdk 29 the platform answers that with a
            // SecurityException thrown on the main thread from native code —
            // which kills the process before any Dart catch can see it. This
            // action exists so that call has somewhere to stand.
            ACTION_HOLD -> {
                startForegroundWithNotification("Casting this screen")
                keepScreenOn()
                // Only now is getMediaProjection() legal. The plugin waits for
                // this before answering Dart, because startForegroundService()
                // returns before onStartCommand has run, and a getDisplayMedia
                // that races it dies with the same SecurityException this action
                // exists to prevent.
                onForeground?.invoke()
                onForeground = null
            }
        }
        return START_NOT_STICKY
    }

    private fun start(intent: Intent) {
        startForegroundWithNotification()
        keepScreenOn()

        val manager = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val data = intent.getParcelableExtra<Intent>(EXTRA_RESULT_DATA) ?: return stopSelf()
        val projection =
            manager.getMediaProjection(intent.getIntExtra(EXTRA_RESULT_CODE, 0), data)
                ?: return stopSelf()
        this.projection = projection

        // Mandatory on API 34+, and the only place user revocation and Android
        // 15 QPR1 stop-on-screen-lock surface.
        projection.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                stopCasting()
                stopSelf()
            }
        }, null)

        val metrics = displayMetrics()
        // Encoder dimensions must be even; odd screen widths exist.
        val width = metrics.widthPixels and 1.inv()
        val height = metrics.heightPixels and 1.inv()
        val bitrate = intent.getIntExtra(EXTRA_BITRATE, 6_000_000)
        val socketName = intent.getStringExtra(EXTRA_SOCKET_NAME) ?: "aircast"

        val format =
            MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(
                    MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface,
                )
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, FRAME_RATE)
                // One second between keyframes: the receiver can join mid-stream
                // without waiting, at a few percent of bitrate.
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                setInteger(
                    MediaFormat.KEY_BITRATE_MODE,
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR,
                )
                if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
            }
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val surface: Surface = codec.createInputSurface()
        codec.start()
        this.codec = codec

        virtualDisplay = projection.createVirtualDisplay(
            "aircast-usb",
            width,
            height,
            metrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            surface,
            null,
            null,
        )

        running = true
        thread(name = "aircast-usb") { serve(socketName, codec) }
    }

    /** Accepts one desktop at a time; a new connection replaces the old one. */
    private fun serve(socketName: String, codec: MediaCodec) {
        try {
            val server = LocalServerSocket(socketName)
            serverSocket = server
            server.use {
                while (running) {
                    it.accept().use { client -> pump(client, codec) }
                }
            }
        } catch (e: Exception) {
            if (running) Log.w(TAG, "usb socket closed", e)
        } finally {
            serverSocket = null
        }
    }

    private fun pump(client: LocalSocket, codec: MediaCodec) {
        val out: OutputStream = client.outputStream
        val info = MediaCodec.BufferInfo()
        // MediaCodec warns that holding buffers stalls the codec, so every
        // buffer is written and released in the same iteration.
        while (running) {
            val index = codec.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT_US)
            if (index < 0) continue
            try {
                if (info.size > 0) {
                    val buffer = codec.getOutputBuffer(index)
                    if (buffer != null) {
                        buffer.position(info.offset)
                        buffer.limit(info.offset + info.size)
                        val bytes = ByteArray(info.size)
                        buffer.get(bytes)
                        out.write(bytes)
                        out.flush()
                    }
                }
            } catch (e: Exception) {
                Log.i(TAG, "desktop disconnected", e)
                codec.releaseOutputBuffer(index, false)
                return
            }
            codec.releaseOutputBuffer(index, false)
            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return
        }
    }

    private fun startForegroundWithNotification(text: String = "Casting this screen over USB") {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Screen cast", NotificationManager.IMPORTANCE_LOW)
        )
        val notification: Notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("aircast")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    /**
     * Android stops a MediaProjection the moment the screen locks, and a tablet
     * left alone locks in under a minute. The first fix was FLAG_KEEP_SCREEN_ON
     * on the app's own window, which holds only while that window is visible —
     * and the user leaves it at once, because what they want mirrored is some
     * other app. The screen then timed out behind our back, the projection
     * stopped, and the desktop froze on the last frame with no error anywhere.
     *
     * A screen wake lock is the one thing a service can hold that keeps the
     * screen on regardless of which window is in front. Deprecated since API 17
     * in favour of the window flag, for the ordinary case where the window flag
     * is enough; it is still honoured, and this is the case it is not. DIM, not
     * BRIGHT: capture reads the framebuffer, not the backlight, so a dimmed
     * screen mirrors just as well and costs less battery.
     */
    @Suppress("DEPRECATION")
    private fun keepScreenOn() {
        if (wakeLock != null) return
        val power = getSystemService(PowerManager::class.java)
        wakeLock = power.newWakeLock(
            PowerManager.SCREEN_DIM_WAKE_LOCK or PowerManager.ON_AFTER_RELEASE,
            "aircast:cast",
        ).also { it.acquire() }
    }

    @Suppress("DEPRECATION")
    private fun displayMetrics(): DisplayMetrics {
        val metrics = DisplayMetrics()
        val window = getSystemService(WINDOW_SERVICE) as WindowManager
        window.defaultDisplay.getRealMetrics(metrics)
        return metrics
    }

    private fun stopCasting() {
        running = false
        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null
        runCatching { serverSocket?.close() }
        virtualDisplay?.release()
        codec?.runCatching { stop() }
        codec?.release()
        projection?.stop()
        virtualDisplay = null
        codec = null
        projection = null
    }

    override fun onDestroy() {
        stopCasting()
        super.onDestroy()
    }

    companion object {
        const val ACTION_START = "io.aircast.sender.USB_START"
        const val ACTION_STOP = "io.aircast.sender.USB_STOP"
        const val ACTION_HOLD = "io.aircast.sender.HOLD_FOREGROUND"

        /** Set by UsbCastPlugin before it starts ACTION_HOLD; fired once startForeground has run. */
        @Volatile
        var onForeground: (() -> Unit)? = null
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
        const val EXTRA_SOCKET_NAME = "socketName"
        const val EXTRA_BITRATE = "bitrate"

        private const val TAG = "aircast"
        private const val CHANNEL_ID = "aircast-cast"
        private const val NOTIFICATION_ID = 1
        private const val FRAME_RATE = 30
        private const val DEQUEUE_TIMEOUT_US = 10_000L
    }
}
