import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'session.dart';
import 'signaling.dart';
import 'usb.dart';

/// The deployed server is compiled in, as it is in the receiver
/// (receiver/CMakeLists.txt, AIRCAST_SIGNAL), so an APK from ci.yml — which
/// passes no define — works out of the box. Anyone running their own server
/// overrides it: `flutter run --dart-define=AIRCAST_SIGNAL=wss://host/ws`,
/// or the field behind the settings button at run time.
const _defaultSignalUrl =
    String.fromEnvironment('AIRCAST_SIGNAL', defaultValue: 'wss://aircast.cloud/ws');

/// Same palette as the receiver's window (receiver/style.css.h), so the two
/// halves of one product look like one product.
const _room = Color(0xFF0D0F12);
const _card = Color(0xFF15181D);
const _edge = Color(0xFF23272E);
const _ink = Color(0xFFF2F5F9);
const _muted = Color(0xFF6F7784);
const _live = Color(0xFF3DDC91);
const _alarm = Color(0xFFFF6B5E);

void main() => runApp(const AircastApp());

class AircastApp extends StatelessWidget {
  const AircastApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'aircast',
        debugShowCheckedModeBanner: false,
        theme: ThemeData(
          useMaterial3: true,
          brightness: Brightness.dark,
          scaffoldBackgroundColor: _room,
          colorScheme: const ColorScheme.dark(
            surface: _room,
            primary: _ink,
            onPrimary: _room,
            error: _alarm,
          ),
        ),
        home: const SenderPage(),
      );
}

class SenderPage extends StatefulWidget {
  const SenderPage({super.key});

  @override
  State<SenderPage> createState() => _SenderPageState();
}

class _SenderPageState extends State<SenderPage> {
  final _code = TextEditingController();
  final _url = TextEditingController(text: _defaultSignalUrl);

  Signaling? _signaling;
  CastSession? _session;
  bool _usb = false;
  bool _connected = false;
  bool _busy = false;
  String _status = 'Enter the code shown on the desktop';
  bool _settingsOpen = false;

  bool get _casting => _session != null || _usb;

  @override
  void initState() {
    super.initState();
    if (Platform.isAndroid) {
      UsbCast.onStopped = () => _stop(status: 'Android stopped the screen capture');
    }
  }

  Future<void> _castOverNetwork() async {
    final code = _code.text.trim();
    if (code.length != 6) {
      return setState(() => _status = 'The code is six digits');
    }

    // tryParse, because parse throws and this line sits outside the try below:
    // a non-numeric port typed into the settings field made the button do
    // nothing at all. The FormatException completed a Future nobody holds, and
    // the status line went on inviting the user to enter a code.
    final url = Uri.tryParse(_url.text.trim());
    if (url == null) {
      return setState(() => _status = 'That server address is not a URL');
    }
    final signaling = Signaling(url, code);
    final session = CastSession(signaling);
    setState(() {
      _signaling = signaling;
      _session = session;
      _busy = true;
      _status = 'Connecting…';
    });
    signaling.onClosed = (reason) => _stop(status: reason);
    session.onState = (state) {
      if (!mounted) return;
      switch (state) {
        case RTCPeerConnectionState.RTCPeerConnectionStateConnected:
          setState(() {
            _connected = true;
            _busy = false;
            _status = 'Mirroring to $code';
          });
        case RTCPeerConnectionState.RTCPeerConnectionStateFailed:
          _stop(status: 'The connection failed — is the relay reachable?');
        case RTCPeerConnectionState.RTCPeerConnectionStateDisconnected:
          setState(() {
            _connected = false;
            _status = 'Reconnecting…';
          });
        default:
          break;
      }
    };
    try {
      await signaling.connect();
      setState(() => _status = 'Waiting for the desktop…');
      await session.start();
      setState(() => _status = 'Negotiating…');
    } on Object catch (e, st) {
      // Into logcat as well as onto the screen: a status line the user does not
      // read is a failure nobody can diagnose, and three of these were found by
      // adb, not by eye.
      debugPrint('aircast: session failed: $e');
      debugPrint('$st');
      await _stop(status: '$e');
    }
  }

  Future<void> _castOverUsb() async {
    try {
      await UsbCast.start();
      setState(() {
        _usb = true;
        _connected = true;
        _status = 'Casting over the cable';
      });
    } on Object catch (e) {
      setState(() => _status = '$e');
    }
  }

  Future<void> _stop({String status = 'Enter the code shown on the desktop'}) async {
    // Take the session out of the fields before the first await. Everything
    // that calls this arrives late and unordered: a connect that timed out ten
    // seconds ago, an onClosed from a socket already gone, a Failed from a peer
    // connection we just closed. Each of them used to read whatever _session
    // held at the moment it ran, which by then could be the cast the user
    // started afterwards, so a timeout from an abandoned attempt stopped a live
    // one. Whoever arrives first owns the teardown; everyone else finds nothing
    // and returns.
    final session = _session;
    final signaling = _signaling;
    final usb = _usb;
    if (session == null && signaling == null && !usb) return;
    _session = null;
    _signaling = null;
    _usb = false;

    await session?.stop();
    await signaling?.close();
    if (usb) await UsbCast.stop();
    if (!mounted) return;
    setState(() {
      _connected = false;
      _busy = false;
      _status = status;
    });
  }

  @override
  void dispose() {
    _stop();
    _code.dispose();
    _url.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        body: SafeArea(
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 20),
            child: Column(
              children: [
                _Header(
                  casting: _casting,
                  connected: _connected,
                  onSettings: () => setState(() => _settingsOpen = !_settingsOpen),
                ),
                if (_settingsOpen) _ServerField(controller: _url, enabled: !_casting),
                Expanded(
                  child: Center(
                    child: _casting
                        ? _CastingCard(code: _code.text, usb: _usb, connected: _connected)
                        : _CodeCard(controller: _code, onSubmit: _castOverNetwork),
                  ),
                ),
                Text(
                  _status,
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontSize: 12, color: _muted),
                ),
                const SizedBox(height: 16),
                if (_casting)
                  _PillButton(
                    label: 'Stop mirroring',
                    onPressed: _stop,
                    background: _card,
                    foreground: _alarm,
                  )
                else ...[
                  _PillButton(
                    label: _busy ? 'Connecting…' : 'Start mirroring',
                    onPressed: _busy ? null : _castOverNetwork,
                    background: _ink,
                    foreground: _room,
                  ),
                  if (Platform.isAndroid) ...[
                    const SizedBox(height: 10),
                    _PillButton(
                      label: 'Mirror over USB cable',
                      onPressed: _castOverUsb,
                      background: _card,
                      foreground: _ink,
                    ),
                  ],
                ],
              ],
            ),
          ),
        ),
      );
}

class _Header extends StatelessWidget {
  const _Header({required this.casting, required this.connected, required this.onSettings});

  final bool casting;
  final bool connected;
  final VoidCallback onSettings;

  @override
  Widget build(BuildContext context) => Row(
        children: [
          const Text(
            'AIRCAST',
            style: TextStyle(
              fontSize: 13,
              fontWeight: FontWeight.w600,
              letterSpacing: 3,
              color: _muted,
            ),
          ),
          const Spacer(),
          if (casting)
            // A dot, because "am I still sharing my screen?" should be
            // answerable from across the table.
            Container(
              width: 8,
              height: 8,
              margin: const EdgeInsets.only(right: 12),
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: connected ? _live : _alarm,
              ),
            ),
          IconButton(
            onPressed: onSettings,
            icon: const Icon(Icons.tune, size: 18, color: _muted),
            tooltip: 'Server',
          ),
        ],
      );
}

class _ServerField extends StatelessWidget {
  const _ServerField({required this.controller, required this.enabled});

  final TextEditingController controller;
  final bool enabled;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(top: 8),
        child: TextField(
          controller: controller,
          enabled: enabled,
          style: const TextStyle(fontSize: 13, color: _ink),
          decoration: InputDecoration(
            isDense: true,
            filled: true,
            fillColor: _card,
            labelText: 'Signalling server',
            labelStyle: const TextStyle(color: _muted, fontSize: 12),
            border: OutlineInputBorder(
              borderRadius: BorderRadius.circular(12),
              borderSide: const BorderSide(color: _edge),
            ),
          ),
        ),
      );
}

/// The code entry, mirroring the receiver's oversized display of the same six
/// digits: one number, read off one screen, typed into another.
class _CodeCard extends StatelessWidget {
  const _CodeCard({required this.controller, required this.onSubmit});

  final TextEditingController controller;
  final VoidCallback onSubmit;

  @override
  Widget build(BuildContext context) => _Card(
        children: [
          const Text('Pairing code', style: TextStyle(fontSize: 13, color: _muted)),
          const SizedBox(height: 18),
          TextField(
            controller: controller,
            autofocus: true,
            textAlign: TextAlign.center,
            keyboardType: TextInputType.number,
            inputFormatters: [
              FilteringTextInputFormatter.digitsOnly,
              LengthLimitingTextInputFormatter(6),
            ],
            onSubmitted: (_) => onSubmit(),
            style: const TextStyle(
              fontSize: 44,
              fontWeight: FontWeight.w700,
              letterSpacing: 10,
              color: _ink,
              fontFeatures: [FontFeature.tabularFigures()],
            ),
            decoration: const InputDecoration(
              counterText: '',
              hintText: '000000',
              hintStyle: TextStyle(
                fontSize: 44,
                fontWeight: FontWeight.w700,
                letterSpacing: 10,
                color: Color(0xFF2B3038),
              ),
              border: InputBorder.none,
            ),
          ),
        ],
      );
}

class _CastingCard extends StatelessWidget {
  const _CastingCard({required this.code, required this.usb, required this.connected});

  final String code;
  final bool usb;
  final bool connected;

  @override
  Widget build(BuildContext context) => _Card(
        children: [
          Icon(usb ? Icons.usb : Icons.screen_share_outlined, size: 34, color: _muted),
          const SizedBox(height: 16),
          Text(
            usb ? 'Cable' : code,
            style: const TextStyle(
              fontSize: 40,
              fontWeight: FontWeight.w700,
              letterSpacing: 8,
              color: _ink,
              fontFeatures: [FontFeature.tabularFigures()],
            ),
          ),
          const SizedBox(height: 8),
          Text(
            connected ? 'Your screen is being mirrored' : 'Setting up…',
            style: const TextStyle(fontSize: 13, color: _muted),
          ),
        ],
      );
}

class _Card extends StatelessWidget {
  const _Card({required this.children});

  final List<Widget> children;

  @override
  Widget build(BuildContext context) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 28, vertical: 32),
        decoration: BoxDecoration(
          color: _card,
          borderRadius: BorderRadius.circular(22),
          border: Border.all(color: _edge),
        ),
        child: Column(mainAxisSize: MainAxisSize.min, children: children),
      );
}

class _PillButton extends StatelessWidget {
  const _PillButton({
    required this.label,
    required this.onPressed,
    required this.background,
    required this.foreground,
  });

  final String label;
  final VoidCallback? onPressed;
  final Color background;
  final Color foreground;

  @override
  Widget build(BuildContext context) => SizedBox(
        width: double.infinity,
        height: 52,
        child: FilledButton(
          onPressed: onPressed,
          style: FilledButton.styleFrom(
            backgroundColor: background,
            foregroundColor: foreground,
            disabledBackgroundColor: _card,
            disabledForegroundColor: _muted,
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(26)),
          ),
          child: Text(label, style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
        ),
      );
}
