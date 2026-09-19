package io.kagami.sender

import android.content.Intent
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine

class MainActivity : FlutterActivity() {
    private var usb: UsbCastPlugin? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val plugin = UsbCastPlugin(this, flutterEngine.dartExecutor.binaryMessenger)
        usb = plugin
    }

    // The consent Intent comes back here. Registering with the engine's
    // ActivityControlSurface is the route for a packaged plugin; for one that
    // lives in the app, forwarding the override is both shorter and harder to
    // get wrong.
    @Deprecated("deprecated in ComponentActivity, still the delivery path for startActivityForResult")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        usb?.onActivityResult(requestCode, resultCode, data)
    }

    override fun onDestroy() {
        usb?.dispose()
        usb = null
        super.onDestroy()
    }
}
