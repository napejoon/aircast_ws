package io.aircast.sender

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionConfig
import android.media.projection.MediaProjectionManager
import android.os.Build
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.PluginRegistry

/**
 * Dart side of the USB path. Owns nothing but the consent dance: it asks for a
 * MediaProjection token and hands it to [UsbCastService], which is where the
 * foreground service, the capture and the socket live.
 *
 * On API 34+ a projection token may be used only once, so consent is requested
 * per session and never cached.
 */
class UsbCastPlugin(
    private val activity: Activity,
    messenger: io.flutter.plugin.common.BinaryMessenger,
) : MethodChannel.MethodCallHandler, PluginRegistry.ActivityResultListener {

    private val channel = MethodChannel(messenger, "io.aircast.sender/usb").also { c ->
        c.setMethodCallHandler(this)
        // The service is the only thing that hears the platform take the
        // capture away. onStop is delivered on the main thread, which is the
        // one a MethodChannel may be answered from.
        UsbCastService.onStopped = { c.invokeMethod("stopped", null) }
    }
    private var pending: MethodChannel.Result? = null
    private var socketName = "aircast"
    private var bitrate = 6_000_000

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "start" -> {
                if (pending != null) {
                    result.error("busy", "a consent request is already in flight", null)
                    return
                }
                socketName = call.argument<String>("socketName") ?: "aircast"
                bitrate = call.argument<Int>("bitrate") ?: 6_000_000
                pending = result
                val manager = activity.getSystemService(Context.MEDIA_PROJECTION_SERVICE)
                    as MediaProjectionManager
                val intent = if (Build.VERSION.SDK_INT >= 34) {
                    manager.createScreenCaptureIntent(
                        MediaProjectionConfig.createConfigForDefaultDisplay()
                    )
                } else {
                    manager.createScreenCaptureIntent()
                }
                activity.startActivityForResult(intent, REQUEST_CONSENT)
            }

            // The WebRTC path's half of the same rule. flutter_webrtc takes
            // consent itself and then calls getMediaProjection() with no
            // foreground service running, which is a SecurityException thrown
            // on the main thread from native code — the process dies before
            // Dart hears about it. Dart calls this between the two, so the
            // service is already up when the plugin's own call lands.
            //
            // startForegroundService, not startService: the app is in the
            // foreground when this runs, but the five-second window it opens is
            // what makes the ordering guarantee rather than a race.
            "holdForeground" -> {
                // Answer when the service has actually called startForeground,
                // not when it has merely been asked to: startForegroundService()
                // returns before onStartCommand runs, and the first bring-up hit
                // exactly that gap — getDisplayMedia raced the service and died
                // with the SecurityException the service exists to prevent.
                val handler = android.os.Handler(android.os.Looper.getMainLooper())
                var answered = false
                val timeout = Runnable {
                    if (!answered) {
                        answered = true
                        UsbCastService.onForeground = null
                        result.error("foreground", "the cast service did not reach the foreground in 5 s", null)
                    }
                }
                UsbCastService.onForeground = {
                    if (!answered) {
                        answered = true
                        handler.removeCallbacks(timeout)
                        result.success(null)
                    }
                }
                handler.postDelayed(timeout, 5_000)
                // Keeping the screen on — Android stops a projection when it
                // locks — is the service's job (UsbCastService.keepScreenOn): a
                // flag on this window held only while this window was in front,
                // and the user leaves it at once to show some other app.
                activity.startForegroundService(
                    Intent(activity, UsbCastService::class.java).setAction(UsbCastService.ACTION_HOLD)
                )
            }

            "stop" -> {
                activity.startService(
                    Intent(activity, UsbCastService::class.java).setAction(UsbCastService.ACTION_STOP)
                )
                result.success(null)
            }

            else -> result.notImplemented()
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (requestCode != REQUEST_CONSENT) return false
        val result = pending ?: return true
        pending = null
        if (resultCode != Activity.RESULT_OK || data == null) {
            result.error("denied", "screen capture consent was denied", null)
            return true
        }
        val intent = Intent(activity, UsbCastService::class.java)
            .setAction(UsbCastService.ACTION_START)
            .putExtra(UsbCastService.EXTRA_RESULT_CODE, resultCode)
            .putExtra(UsbCastService.EXTRA_RESULT_DATA, data)
            .putExtra(UsbCastService.EXTRA_SOCKET_NAME, socketName)
            .putExtra(UsbCastService.EXTRA_BITRATE, bitrate)
        // Must be a foreground service before getMediaProjection() or Android 14+ throws.
        activity.startForegroundService(intent)
        result.success(null)
        return true
    }

    fun dispose() {
        // Left set, the closure answers on a channel whose engine has gone.
        UsbCastService.onStopped = null
        channel.setMethodCallHandler(null)
    }

    private companion object {
        const val REQUEST_CONSENT = 0xA1C
    }
}
