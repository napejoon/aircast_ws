import 'package:flutter/services.dart';

/// USB path: the phone listens on an abstract-namespace socket and writes a
/// raw Annex-B H.264 elementary stream to whoever connects. The desktop opens
/// it with `adb forward tcp:<port> localabstract:aircast`. No WebRTC, no ICE, no TURN — the receiver reads
/// it with `tcpclientsrc ! h264parse ! <decoder> ! <sink>`.
///
/// Android only; the methods throw MissingPluginException elsewhere.
class UsbCast {
  static const _channel = MethodChannel('io.aircast.sender/usb');

  /// Asks for screen-capture consent and starts the foreground service.
  /// Returns once the socket is listening; the receiver may connect at any
  /// time after that.
  static Future<void> start({String socketName = 'aircast', int bitrate = 6000000}) =>
      _channel.invokeMethod('start', {'socketName': socketName, 'bitrate': bitrate});

  static Future<void> stop() => _channel.invokeMethod('stop');
}
