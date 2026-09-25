/// What a pairing QR carries: the code to join with, and the server to join it
/// on. The receiver draws it (receiver/main.c, build_pairing_qr) as
/// `kagami://pair?c=<code>&s=<url>`.
///
/// The URL is the whole point. Without it one APK talks only to the server it
/// was compiled against, and a room with its own receiver and its own relay
/// needs its own build.
///
/// In its own file, with no camera in it, so the parsing can be tested without
/// a plugin or a device. The parser is the part that reads whatever a camera
/// happened to be pointed at, which makes it the part worth a test.
class PairingPayload {
  const PairingPayload({required this.code, required this.url});

  final String code;
  final String url;

  /// Parses one scanned string, or returns null if it is not ours.
  ///
  /// Strict on purpose. A camera pointed at a room reads whatever is in frame,
  /// and everything that is not exactly this shape has to leave the scanner
  /// running rather than half-fill the form:
  ///
  ///  * scheme `kagami`, host `pair`
  ///  * `c` exactly six digits, which is what the server hands out and what the
  ///    code field accepts
  ///  * `s` a ws:// or wss:// URL with a host — the only two things Signaling
  ///    can open, and an http:// or a file:// here would fail later and further
  ///    from the cause
  static PairingPayload? parse(String raw) {
    final uri = Uri.tryParse(raw.trim());
    if (uri == null || uri.scheme != 'kagami' || uri.host != 'pair') return null;

    final code = uri.queryParameters['c'] ?? '';
    // Length and digits both: int.tryParse takes '+12345' and ' 123456'.
    if (code.length != 6 || !RegExp(r'^\d{6}$').hasMatch(code)) return null;

    final url = uri.queryParameters['s'] ?? '';
    final server = Uri.tryParse(url);
    if (server == null) return null;
    if (server.scheme != 'ws' && server.scheme != 'wss') return null;
    if (server.host.isEmpty) return null;

    return PairingPayload(code: code, url: url);
  }

  /// Whether this QR points at [server]: same scheme, host and port.
  ///
  /// What decides whether a scan may start the cast by itself. A QR naming the
  /// server this app was built for can only pair it with a receiver on that
  /// server, which is what typing its code does anyway; one naming any other
  /// host would send the screen to a relay somebody else runs, so that one
  /// still waits for Start with the host on the status line.
  bool isOn(String server) {
    final a = Uri.parse(url);
    final b = Uri.tryParse(server);
    return b != null && a.scheme == b.scheme && a.host == b.host && a.port == b.port;
  }
}
