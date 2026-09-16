package io.aircast.sender

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.IBinder
import android.os.PowerManager
import android.util.DisplayMetrics
import android.util.Log
import android.view.Display
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

    /** Swapped by the serve thread on rotation and read there; see [rotate]. */
    @Volatile
    private var codec: MediaCodec? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var displayListener: DisplayManager.DisplayListener? = null
    private var bitrate = 6_000_000

    /**
     * What the running encoder was configured for. onDisplayChanged fires for
     * brightness and refresh rate too, so the size is what decides whether a
     * change is a rotation.
     */
    private var encodedWidth = 0
    private var encodedHeight = 0

    /** Set on the main thread by the display listener, acted on in [pump]. */
    @Volatile
    private var rotationPending = false
    private var wakeLock: PowerManager.WakeLock? = null

    // Written on the serve thread and read on the main thread in stopCasting.
    // Without volatile there is no edge between the two, and the main thread is
    // entitled to go on seeing the null it started with and skip the close.
    @Volatile
    private var serverSocket: LocalServerSocket? = null

    /** Kept so stopCasting can reach the name the serve thread is parked on. */
    @Volatile
    private var socketName = "aircast"

    /**
     * The SPS and PPS, kept from the codec-config buffer MediaCodec emits once
     * at the head of the stream. Filled on the serve thread by whichever
     * desktop drains that buffer and read there by every desktop after it;
     * cleared on the main thread in stopCasting, and that crossing is what
     * makes it volatile.
     */
    @Volatile
    private var codecConfig: ByteArray? = null

    @Volatile
    private var running = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopCasting()
                stopSelf()
                // The notification's Stop button lands here too, and on the
                // WebRTC path stopCasting() above ends nothing the user can
                // see: that capture belongs to flutter_webrtc, over in the Dart
                // process, and this service holds only the notification and the
                // wake lock. A button that took the notification away and left
                // the screen still being mirrored would be worse than no button
                // at all.
                //
                // It settles in one round whichever direction it starts from.
                // Dart's own stop arrives here and then hears its own echo,
                // which finds nothing: _stop in main.dart clears its fields
                // before its first await. The button runs the other way round —
                // Dart tears the session down and then calls stop() back down
                // here (main.dart's usb branch, and session.dart's Android
                // branch on the WebRTC path), which builds this service once
                // more only to stop it again, and it is that second echo which
                // finds the fields already empty. The bounce costs a service
                // create and destroy and posts nothing, because this branch
                // never calls startForeground.
                onStopped?.invoke()
            }

            ACTION_START -> start(intent)

            // Nothing but the notification, for the WebRTC path. flutter_webrtc
            // calls getMediaProjection() itself and starts no service of its
            // own, and since targetSdk 29 the platform answers that with a
            // SecurityException thrown on the main thread from native code —
            // which kills the process before any Dart catch can see it. This
            // action exists so that call has somewhere to stand.
            ACTION_HOLD -> {
                startForegroundWithNotification("Over the network")
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
                // Mind the name: inside this object `projection` is the local
                // the callback captured, not the field, so a null test on it
                // would be a constant true and every stop would look like the
                // platform's. The field is what carries the answer.
                // stopCasting() nulls it, and the platform delivers this
                // callback as a post to the main looper, after the stop that
                // caused it has returned. So a field still pointing at *this*
                // projection means the capture was taken from us: consent
                // revoked from the cast chip, another app taking the
                // projection, the screen locking on Android 15 QPR1. Dart has
                // no other way to learn that, and goes on showing a live cast
                // that has already stopped. A field pointing at some other
                // projection is the stale callback of a cast that has been
                // replaced, and it must not tear down its replacement.
                if (this@UsbCastService.projection !== projection) return
                stopCasting()
                stopSelf()
                onStopped?.invoke()
            }
        }, null)

        val metrics = displayMetrics()
        // Encoder dimensions must be even; odd screen widths exist.
        val width = metrics.widthPixels and 1.inv()
        val height = metrics.heightPixels and 1.inv()
        bitrate = intent.getIntExtra(EXTRA_BITRATE, 6_000_000)
        socketName = intent.getStringExtra(EXTRA_SOCKET_NAME) ?: "aircast"

        val (codec, surface) = newEncoder(width, height)
        this.codec = codec
        encodedWidth = width
        encodedHeight = height

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

        watchRotation()
        rotationPending = false
        running = true
        thread(name = "aircast-usb") { serve() }
    }

    /**
     * A started encoder for a screen this size, and the surface a VirtualDisplay
     * draws into. Separate from [start] because rotation builds a second one:
     * MediaCodec cannot be reconfigured while it runs, and its input surface
     * carries the size it was created with.
     */
    private fun newEncoder(width: Int, height: Int): Pair<MediaCodec, Surface> {
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
                // That interval is counted in encoded frames, not in seconds,
                // and a VirtualDisplay hands the encoder a frame only when the
                // mirrored screen changes — so "one second" is really thirty
                // frames, and on a still screen thirty frames is a minute.
                // Measured on this tablet with screenrecord, which drives the
                // same VirtualDisplay through the same encoder: four seconds of
                // an idle screen produced two slices, and fourteen seconds with
                // an animation running produced a hundred and seventy-six, one
                // IDR among them. Without a floor under that rate the sync
                // frame pump() asks for on every new desktop waits for the user
                // to touch something. Re-encoding the last frame after a tenth
                // of a second of quiet costs a few hundred bytes of skipped
                // macroblocks while nothing is moving, and nothing at all while
                // something is, because it only fires when no real frame came.
                // setLong, not setInteger: the framework reads this key with
                // findInt64 and silently ignores an Int.
                setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L)
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
        return codec to surface
    }

    /**
     * The screen turning under a cast that is already running.
     *
     * Two earlier attempts at this were dropped because both built a second
     * VirtualDisplay, and MEDIA_PROJECTION_PREVENTS_REUSING_CONSENT forbids a
     * second createVirtualDisplay on one projection from targetSdk 34 -- this
     * app pins 36, and the SecurityException unwinds past every catch in pump()
     * and ends the cast. VirtualDisplay.resize and VirtualDisplay.setSurface are
     * the way round it: they change the display this projection already owns
     * rather than asking for another one, and neither re-opens consent.
     *
     * The flag is only ever acted on from the serve thread, so there is exactly
     * one thread building, swapping and releasing encoders.
     */
    private fun watchRotation() {
        if (displayListener != null) return
        val displays = getSystemService(DisplayManager::class.java)
        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) {}
            override fun onDisplayRemoved(displayId: Int) {}
            override fun onDisplayChanged(displayId: Int) {
                if (displayId == Display.DEFAULT_DISPLAY) rotationPending = true
            }
        }
        displays.registerDisplayListener(listener, Handler(Looper.getMainLooper()))
        displayListener = listener
    }

    /**
     * Rebuilds the encoder for the screen's new shape and returns the one to go
     * on reading, or null if the cast cannot continue.
     *
     * Order matters: the display is pointed at the new surface before the old
     * encoder is released, because releasing an encoder whose input surface a
     * display is still drawing into takes the surface out from under the
     * compositor.
     */
    private fun rotate(): MediaCodec? {
        val display = virtualDisplay ?: return null
        val old = codec ?: return null
        val metrics = displayMetrics()
        val width = metrics.widthPixels and 1.inv()
        val height = metrics.heightPixels and 1.inv()
        // onDisplayChanged also fires for brightness and refresh rate.
        if (width == encodedWidth && height == encodedHeight) return old

        val next = try {
            newEncoder(width, height)
        } catch (e: Exception) {
            Log.e(TAG, "no encoder for " + width + "x" + height + ", ending the cast", e)
            return null
        }
        // The kept header describes the old size, and a desktop connecting in the
        // next millisecond must not be told the picture is that shape. The new
        // encoder emits its own codec-config buffer as its first output.
        codecConfig = null
        display.resize(width, height, metrics.densityDpi)
        display.setSurface(next.second)
        codec = next.first
        encodedWidth = width
        encodedHeight = height
        old.runCatching { stop() }
        old.release()
        return next.first
    }

    /**
     * Accepts one desktop at a time; a new connection replaces the old one.
     * Reads the socketName and codec fields rather than taking them as
     * parameters, so there is one name in play and stopCasting can reach it.
     */
    private fun serve() {
        if (this.codec == null) return
        var mine: LocalServerSocket? = null
        try {
            val server = LocalServerSocket(socketName)
            mine = server
            serverSocket = server
            server.use {
                while (running) {
                    it.accept().use { client -> pump(client) }
                }
            }
        } catch (e: Exception) {
            // running == false is the ordinary stop: the socket was closed out
            // from under this thread on purpose. running == true means the bind
            // itself failed, and at that moment the notification says the
            // tablet is casting, the encoder is filling a VirtualDisplay at six
            // megabits and the screen is held awake, with nothing listening.
            // Dart was told the cast started a second before any of this ran,
            // so a log line has always been the only trace. End the session
            // instead: the notification going away is the one signal there is.
            if (running) {
                Log.e(TAG, "usb socket unavailable, ending the cast", e)
                stopCasting()
                stopSelf()
            }
        } finally {
            // Only if it is still ours. This thread is woken by the connect
            // stopCasting makes to its own name, and nothing orders its return
            // against the next cast's serve() setting the field to a new
            // socket. Clearing unconditionally could therefore hide the next
            // cast's listener from the next stopCasting, which would leave the
            // name bound and fail the cast after that on "Address already in
            // use" -- the failure the connect above exists to prevent.
            if (serverSocket === mine) serverSocket = null
        }
    }

    private fun pump(client: LocalSocket) {
        // A screen that turned while nobody was connected, so that the header
        // written below describes what this desktop is about to receive rather
        // than the shape before the turn.
        if (rotationPending) {
            rotationPending = false
            rotate()
        }
        var codec = this.codec ?: return
        val out: OutputStream = client.outputStream
        val info = MediaCodec.BufferInfo()
        // MediaCodec hands out the SPS and PPS exactly once, in the
        // codec-config buffer at the head of the stream, so the first desktop
        // of a cast is the only one that ever sees them. Every desktop after it
        // — and closing gst-launch and starting it again is the whole of this
        // path's workflow — was handed a stream beginning mid-GOP with nothing
        // for a decoder to configure itself from, which libavcodec answers with
        // "Invalid data found" and no picture at all, for as long as that
        // desktop stays connected. Replay the bytes that worked for the first
        // one. This encoder was checked rather than assumed: fourteen seconds
        // of its output, read off the tablet, carried one SPS and one PPS, both
        // at the head, so there is nothing in band to fall back on.
        val header = codecConfig
        if (header != null) {
            try {
                out.write(header)
                out.flush()
            } catch (e: Exception) {
                Log.i(TAG, "desktop disconnected", e)
                return
            }
        }
        // Headers alone put nothing on the screen. The decoder understands the
        // stream at that point, but every frame until the next IDR refers to
        // pictures this desktop never received and is discarded without a word,
        // and the wait for that IDR is not the one second KEY_I_FRAME_INTERVAL
        // reads like — it is thirty encoded frames, which is as long as the
        // screen takes to change thirty times. Asking for a sync frame costs
        // whatever the encoder had already queued while nobody was connected —
        // the decoder throws that away too — and then the IDR arrives and the
        // picture starts. It lands promptly only because the format above puts
        // a floor under the frame rate; the request itself is applied to the
        // next frame the input surface delivers, and a still screen delivers
        // none.
        codec.setParameters(Bundle().apply {
            putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
        })
        // MediaCodec warns that holding buffers stalls the codec, so every
        // buffer is written and released in the same iteration.
        while (running) {
            if (rotationPending) {
                rotationPending = false
                codec = rotate() ?: return
                continue
            }
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
                        // Kept before the write, so a desktop that dies on this
                        // very buffer still leaves the headers for the next one.
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            codecConfig = bytes
                        }
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

    private fun startForegroundWithNotification(text: String = "Over the USB cable") {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Screen cast", NotificationManager.IMPORTANCE_LOW)
        )
        // Tapping the body brings aircast back. Without it the notification was
        // a dead end: while a cast runs the user is by definition inside some
        // other app — that is the thing being mirrored — so the only route back
        // to the Stop button in our own window was to go and find the launcher
        // icon. NEW_TASK because a Service has no task of its own to start an
        // activity in; CLEAR_TOP against the manifest's singleTop so the
        // running instance is resumed and handed the intent rather than a
        // second copy being stacked on top of it. FLAG_IMMUTABLE is mandatory
        // from API 31 and has existed since 23, so at this app's floor of 26 it
        // needs no guard.
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this,
            0,
            Intent(this, UsbCastService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val notification: Notification = Notification.Builder(this, CHANNEL_ID)
            // Not the app's name: the notification header already carries
            // that and the icon, so the title line was spent saying something
            // the user could already see. This is the one sentence someone
            // reads out of the corner of their eye while using another app, so
            // it answers the thing they are actually worried about, in the same
            // words the app's own card uses (sender/lib/main.dart).
            .setContentTitle("Your screen is being mirrored")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setOngoing(true)
            .setContentIntent(open)
            // The same label as the button in the app, because it ends the same
            // thing. The framework wants an icon here even on templates that do
            // not draw one.
            .addAction(
                Notification.Action.Builder(
                    Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel),
                    "Stop mirroring",
                    stop,
                ).build()
            )
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
        displayListener?.let {
            runCatching { getSystemService(DisplayManager::class.java).unregisterDisplayListener(it) }
        }
        displayListener = null
        rotationPending = false
        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null
        runCatching { serverSocket?.close() }
        // Closing the listening socket does not return a thread already parked
        // in accept(): Linux leaves that syscall blocked, and the reference it
        // goes on holding keeps the abstract name bound for the life of the
        // process. That is why the second cast of a run died on "Address
        // already in use" while the first one's thread sat there for ever. One
        // connection to the name hands that accept a client; the loop sees
        // running == false and returns at once, and the name goes with the
        // thread. It fails harmlessly when the thread has already left, which
        // is the case where the desktop was still connected.
        runCatching { LocalSocket().use { it.connect(LocalSocketAddress(socketName)) } }
        virtualDisplay?.release()
        codec?.runCatching { stop() }
        codec?.release()
        projection?.stop()
        virtualDisplay = null
        codec = null
        // The next cast configures its own encoder, and if the tablet has been
        // turned meanwhile that encoder's SPS carries different dimensions.
        // Replaying this one's headers to that cast's first desktop would
        // describe a picture it is not about to receive.
        codecConfig = null
        projection = null
        encodedWidth = 0
        encodedHeight = 0
    }

    /**
     * The task swiped away from Recents destroys the Flutter engine, and
     * flutter_webrtc's detach stops its capture with it (GetUserMediaImpl
     * .removeVideoCapturer -> stopCapture). Nothing tells this service: on the
     * network path it goes on holding the screen awake under a notification
     * that says the screen is being mirrored, when nothing is. The USB capture
     * is this service's own and outlives the window on purpose; its
     * notification still has a working Stop.
     */
    override fun onTaskRemoved(rootIntent: Intent?) {
        if (projection == null) {
            stopCasting()
            stopSelf()
        }
        super.onTaskRemoved(rootIntent)
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

        /**
         * Fired when the platform ends the capture rather than us: consent
         * revoked from the cast chip, another app taking the projection, the
         * screen locking on Android 15 QPR1. Dart has no other way to learn
         * this and goes on showing a live cast that has already stopped.
         */
        @Volatile
        var onStopped: (() -> Unit)? = null
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
