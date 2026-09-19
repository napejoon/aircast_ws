import 'package:flutter/services.dart';

/// USB path: the phone listens on an abstract-namespace socket and writes a
/// raw Annex-B H.264 elementary stream to whoever connects. The desktop opens
/// it with `adb forward tcp:<port> localabstract:aircast`. No WebRTC, no ICE, no TURN — the receiver reads
/// it with `tcpclientsrc ! h264parse ! <decoder> ! <sink>`.
///
/// Android only; the methods throw MissingPluginException elsewhere.
class UsbCast {
  static const _channel = MethodChannel('io.kagami.sender/usb');

  /// Asks for POST_NOTIFICATIONS. The manifest has declared it since the
  /// foreground service existed and nothing ever requested it, so on Android 13
  /// and later the cast notification — the only part of aircast a user sees
  /// while mirroring some other app — has never once been displayed.
  ///
  /// Call it when the app opens, not when a cast starts: the answer has to be
  /// in before the service posts, because a notification refused at enqueue is
  /// dropped rather than held. The future completes as soon as the dialog is up
  /// and carries no answer, because there is nothing to do with a refusal — a
  /// mediaProjection foreground service runs either way.
  static Future<void> askToNotify() => _channel.invokeMethod('askToNotify');

  /// Asks for screen-capture consent and starts the foreground service.
  /// Returns once the socket is listening; the receiver may connect at any
  /// time after that.
  static Future<void> start({String socketName = 'aircast', int bitrate = 6000000}) =>
      _channel.invokeMethod('start', {'socketName': socketName, 'bitrate': bitrate});

  /// Raises the same foreground service with nothing to do but exist: the
  /// notification, no consent token, no VirtualDisplay, no socket. The WebRTC
  /// path needs it because flutter_webrtc starts no service of its own and the
  /// platform kills an app that calls getMediaProjection() without one.
  ///
  /// Call it after consent is granted and before getDisplayMedia.
  static Future<void> holdForeground() => _channel.invokeMethod('holdForeground');

  /// Stops the capture and releases the service, on either path.
  static Future<void> stop() => _channel.invokeMethod('stop');

  /// Android ending the capture without being asked: consent revoked from the
  /// cast chip, another app taking the projection, the screen locking. Until
  /// this existed the UI went on showing a live cast that had already stopped,
  /// which is the one thing a mirroring app must not get wrong.
  static set onStopped(void Function() handler) =>
      _channel.setMethodCallHandler((call) async {
        if (call.method == 'stopped') handler();
      });
}
