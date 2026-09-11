package io.aircast.sender

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine

class MainActivity : FlutterActivity() {
    private var usb: UsbCastPlugin? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val plugin = UsbCastPlugin(this, flutterEngine.dartExecutor.binaryMessenger)
        usb = plugin
        // The consent Intent comes back through the activity, not the engine.
        flutterEngine.activityControlSurface.addActivityResultListener(plugin)
    }

    override fun onDestroy() {
        usb?.dispose()
        usb = null
        super.onDestroy()
    }
}
