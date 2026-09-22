import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'session.dart';
import 'signaling.dart';
import 'pairing.dart';
import 'scan.dart';
import 'usb.dart';

/// The deployed server is compiled in, as it is in the receiver
/// (receiver/CMakeLists.txt, AIRCAST_SIGNAL), so an APK from ci.yml — which
/// passes no define — works out of the box. Anyone running their own server
/// overrides it: `flutter run --dart-define=AIRCAST_SIGNAL=wss://host/ws`,
/// or the field behind the settings button at run time.
const _defaultSignalUrl =
    String.fromEnvironment('AIRCAST_SIGNAL', defaultValue: 'wss://aircast.cloud/ws');

/// Same palette as the receiver's window (receiver/style.css.h), so the two
/// halves of one product look like one product. Kagami is a mirror, and the
/// room both halves draw is a moon over deep water.
const _room = Color(0xFF05080F);
const _card = Color(0xFF0D1726);
const _edge = Color(0xFF1A2B45);
const _ink = Color(0xFFE8EDF2);
const _muted = Color(0xFF7B8EA6);

/// The wordmark, and every other place the product says its own name. Dimmed
/// moonlight rather than a saturated colour: the brightest thing on a phone
/// held at arm's length should be the state of the cast, not the brand.
const _brand = Color(0xFF9FB3C8);

/// The green the sea goes where the light lands, and the only saturated colour
/// on this screen. It answers "am I still sharing my screen?" and nothing
/// else. It was the brand colour when the brand was turquoise, which is
/// exactly why it is not any more: a wordmark and a live dot in one colour
/// means the card is brightest when it has nothing to say.
const _live = Color(0xFF6FE3C4);
const _alarm = Color(0xFFFF7A5C);

/// Amber, for the state between working and failed: reconnecting, waiting,
/// degraded. It had no colour of its own before, so a link dropping looked
/// either fine or fatal and never like what it is. Warm, on a window that is
/// otherwise entirely cold, which is what makes it findable without shouting.
const _caution = Color(0xFFFFB35C);

/// Violet, for the second way to do the same thing -- the USB path beside the
/// network one. Not the indigo it used to be: on this ground an indigo is the
/// ground. Far enough from the green and the amber to be neither.
const _accent = Color(0xFFB693FF);

void main() => runApp(const AircastApp());

class AircastApp extends StatelessWidget {
  const AircastApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'Kagami',
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

  /// The last reading from the peer connection, or null before the first one.
  /// Kept rather than streamed into the widget so a rebuild for any other
  /// reason still has the numbers to draw.
  CastStats? _stats;

  bool get _casting => _session != null || _usb;

  /// Owned here rather than left to `autofocus` on the field.
  ///
  /// _casting is a getter over _session, so every stop rebuilds _CodeCard from
  /// nothing -- and an autofocus on a field that is created afresh summons the
  /// keyboard every time. Start a cast with no network and the failure is
  /// immediate: the card goes, the card comes back, the keyboard comes up, and
  /// pressing Start again does the same thing. That is the bounce. Focused once
  /// when the page opens, which is the one moment it is wanted.
  final _codeFocus = FocusNode();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _codeFocus.requestFocus();
    });
    if (Platform.isAndroid) {
      // Fires when Android takes the capture away — consent revoked, the screen
      // locking, another app claiming the projection — and now also when the
      // user presses Stop in the notification. That second case is why the
      // wording no longer blames Android: it would be a lie in the commonest
      // case there is, someone ending their own cast from the shade.
      UsbCast.onStopped = () => _stop(status: 'Mirroring stopped');
      // Asked here rather than when a cast begins: the answer has to be in
      // before the service posts, since a notification refused at enqueue is
      // dropped and not held, and every moment inside a cast is either racing
      // the capture-consent dialog or sitting inside the foreground-service
      // deadline. Not awaited, because a refusal changes nothing we do and no
      // cast should wait on a dialog it does not need.
      UsbCast.askToNotify();
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
    // Guarded the way onStats below is, and for the same reason. These
    // callbacks outlive the attempt that installed them: the socket from a
    // cast that dropped can report itself closed after the user has pressed
    // Start again, and an unguarded _stop then tears down the attempt they
    // just made. From the outside that is a button that does nothing.
    signaling.onClosed = (reason) {
      if (_signaling != signaling) return;
      _stop(status: reason);
    };
    session.onStats = (s) {
      // A tick can land after the widget is gone, and after _stop has replaced
      // the session: both would be a setState on a dead State.
      if (mounted && _session == session) setState(() => _stats = s);
    };
    session.onState = (state) {
      if (!mounted || _session != session) return;
      switch (state) {
        case RTCPeerConnectionState.RTCPeerConnectionStateConnected:
          setState(() {
            _connected = true;
            _busy = false;
            _status = 'Mirroring to $code';
          });
        case RTCPeerConnectionState.RTCPeerConnectionStateFailed:
          // Names the host it failed on. The line used to ask the user "is the
          // relay reachable?", which is a question only the app is in a
          // position to answer, and it named nothing they could go and check.
          _stop(
            status: 'Cannot reach ${url.host}. Check the server address, '
                'or try another network',
          );
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
      debugPrint('kagami: session failed: $e');
      debugPrint('$st');
      await _stop(status: '$e');
    }
  }

  /// Fills the two fields a pairing QR carries and stops there.
  ///
  /// Deliberately not a cast. The server address decides where this
  /// screen is sent, and a QR is printed by whoever printed it: scanning
  /// one and mirroring immediately would put a screen on a stranger's
  /// relay before its owner had read the host it was going to. The status
  /// line names that host, and Start stays where it was.
  Future<void> _scan() async {
    final payload = await Navigator.of(context).push<PairingPayload>(
      MaterialPageRoute(builder: (_) => const ScanPage()),
    );
    if (payload == null || !mounted) return;
    setState(() {
      _code.text = payload.code;
      _url.text = payload.url;
      _status = 'Scanned ${Uri.parse(payload.url).host} — press Start to mirror';
    });
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

    // The bye first, and the teardown after. It used to be the other way
    // round, and session.stop() is four platform round-trips into
    // libwebrtc -- tracks, stream, close, dispose -- so the one message
    // that tells the desktop this cast is over was queued behind all of
    // them. Nothing is lost by telling the far end first: it has no more
    // use for a peer connection this side is about to destroy.
    await signaling?.close();
    await session?.stop();
    if (usb) await UsbCast.stop();
    if (!mounted) return;
    setState(() {
      _connected = false;
      _busy = false;
      _status = status;
      // Cleared, not kept. A reading left over from a cast that ended reads as
      // a cast still running.
      _stats = null;
    });
  }

  @override
  void dispose() {
    _stop();
    _code.dispose();
    _url.dispose();
    _codeFocus.dispose();
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
                // Centred while there is room and scrollable when there is not.
                // The card is a fixed height and the keyboard takes about half
                // the screen, so a plain Center had nowhere to put it and Flutter
                // drew BOTTOM OVERFLOWED BY 36 PIXELS across the bottom of the
                // one card the user is trying to type into.
                Expanded(
                  child: LayoutBuilder(
                    builder: (context, constraints) => SingleChildScrollView(
                      child: ConstrainedBox(
                        constraints:
                            BoxConstraints(minHeight: constraints.maxHeight),
                        child: Center(
                          child: _casting
                              ? _CastingCard(
                                  code: _code.text,
                                  usb: _usb,
                                  connected: _connected,
                                  stats: _stats,
                                )
                              : _CodeCard(
                                  controller: _code,
                                  focusNode: _codeFocus,
                                  onSubmit: _castOverNetwork,
                                ),
                        ),
                      ),
                    ),
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
                    // The green, because this is the button that makes the dot
                    // green: the one action on the screen wears the colour of
                    // the state it creates. The wordmark above it does not --
                    // it is dimmed moonlight now, which is what leaves this the
                    // only saturated thing anyone has to find.
                    background: _live,
                    foreground: _room,
                  ),
                  if (Platform.isAndroid) ...[
                    const SizedBox(height: 10),
                    _PillButton(
                      label: 'Scan the code on the desktop',
                      onPressed: _busy ? null : _scan,
                      background: _card,
                      foreground: _ink,
                    ),
                    const SizedBox(height: 10),
                    _PillButton(
                      label: 'Mirror over USB cable',
                      onPressed: _castOverUsb,
                      background: _card,
                      // Violet: the second way to do the same thing. It reads
                      // as a choice beside the green button rather than as a
                      // lesser version of it, which grey on grey did.
                      foreground: _accent,
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
            'KAGAMI',
            style: TextStyle(
              fontSize: 13,
              fontWeight: FontWeight.w600,
              letterSpacing: 3,
              color: _brand,
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
                // Amber, not coral. A cast that is up but not yet connected --
                // or reconnecting after the link dropped -- is not a failure,
                // and painting it in the same colour as one told the user their
                // screen had stopped going out when it was about to resume.
                color: connected ? _live : _caution,
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
  const _CodeCard({
    required this.controller,
    required this.focusNode,
    required this.onSubmit,
  });

  final TextEditingController controller;
  final FocusNode focusNode;
  final VoidCallback onSubmit;

  @override
  Widget build(BuildContext context) => _Card(
        children: [
          const Text('Pairing code', style: TextStyle(fontSize: 13, color: _muted)),
          const SizedBox(height: 18),
          TextField(
            controller: controller,
            focusNode: focusNode,
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
              border: InputBorder.none,
            ),
          ),
          const SizedBox(height: 10),
          // The hint used to be a dimmed 000000 in the same face and size as a
          // typed code, which reads as a value already entered rather than as
          // an empty field. Six marks say the same thing -- this many digits,
          // this many still to go -- without pretending to be digits.
          ValueListenableBuilder<TextEditingValue>(
            valueListenable: controller,
            builder: (_, value, __) => _Slots(filled: value.text.length),
          ),
        ],
      );
}

/// Six marks under the field, filled from the left as digits arrive.
class _Slots extends StatelessWidget {
  const _Slots({required this.filled});

  final int filled;

  @override
  Widget build(BuildContext context) => Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: List.generate(
          6,
          (i) => Container(
            width: 20,
            height: 2,
            margin: const EdgeInsets.symmetric(horizontal: 3),
            color: i < filled ? _muted : _edge,
          ),
        ),
      );
}

class _CastingCard extends StatelessWidget {
  const _CastingCard({
    required this.code,
    required this.usb,
    required this.connected,
    this.stats,
  });

  final String code;
  final bool usb;
  final bool connected;
  final CastStats? stats;

  @override
  Widget build(BuildContext context) => _Card(
        children: [
          Icon(usb ? Icons.usb : Icons.screen_share_outlined, size: 34, color: _muted),
          const SizedBox(height: 16),
          Text(
            // Grouped the way the desktop shows it, so the two screens read as
            // the same number rather than as two strings that happen to match.
            usb
                ? 'Cable'
                : code.length == 6
                    ? '${code.substring(0, 3)} ${code.substring(3)}'
                    : code,
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
            // Green once it is true. This is the line that answers "is my
            // screen out there right now?", and in muted grey it read as a
            // caption for the number above it.
            style: TextStyle(fontSize: 13, color: connected ? _live : _muted),
          ),
          // The same four readings the desktop puts along its bottom edge.
          // Only on the WebRTC path: the USB one has no peer connection to ask,
          // and a grid of dashes says less than no grid at all.
          if (!usb && stats != null) ...[
            const SizedBox(height: 18),
            _StatGrid(stats: stats!),
          ],
        ],
      );
}

/// Two by two, because four readings in a row on a phone are four columns too
/// narrow to hold "2304×1440".
class _StatGrid extends StatelessWidget {
  const _StatGrid({required this.stats});

  final CastStats stats;

  @override
  Widget build(BuildContext context) => Column(
        children: [
          Row(
            children: [
              Expanded(child: _Stat(k: 'PATH', v: stats.path)),
              // The link estimate, because it is the number every other number
              // on this card is downstream of: a small picture on a narrow link
              // is the mirror working correctly, and this is what says so.
              Expanded(child: _Stat(k: 'LINK', v: stats.linkLabel)),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(child: _Stat(k: 'LATENCY', v: stats.rttLabel)),
              Expanded(child: _Stat(k: 'PICTURE', v: stats.pictureLabel)),
            ],
          ),
        ],
      );
}

class _Stat extends StatelessWidget {
  const _Stat({required this.k, required this.v});

  final String k;
  final String v;

  @override
  Widget build(BuildContext context) => Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(height: 1, color: _edge),
          const SizedBox(height: 7),
          Text(
            k,
            style: const TextStyle(
              fontSize: 9,
              fontWeight: FontWeight.w700,
              letterSpacing: 1.6,
              color: _muted,
            ),
          ),
          const SizedBox(height: 2),
          Text(
            v,
            // Tabular, so a value that changes every second does not shuffle
            // the one beside it sideways as digits swap width.
            style: const TextStyle(
              fontSize: 14,
              color: _ink,
              fontFeatures: [FontFeature.tabularFigures()],
            ),
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
