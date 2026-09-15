import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show debugPrint;
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'signaling.dart';
import 'usb.dart';

/// One cast session: screen capture in, WebRTC out, relayed through our TURN.
///
/// Codec order is H.264 first, VP8 as the mandatory fallback — libwebrtc's
/// Android AAR is built with `rtc_use_h264=false`, so a MediaTek or Unisoc
/// phone has no H.264 encoder at all and would otherwise send a black screen
/// (docs/research/stack-options.md §1).
class CastSession {
  CastSession(this._signaling);

  final Signaling _signaling;

  /// 12 Mbit/s. It was 6, "the 1080p ceiling from #8", and that number was
  /// chosen for a stream this one outgrew: 1920x1080 is 2.07 Mpixel and the
  /// tablet sends 2304x1440, which is 3.32, and the frame rate cap went from 30
  /// to 60 in the same week. Three and a quarter times the pixels per second
  /// through an unchanged ceiling.
  ///
  /// What that costs is not softness, which would be the honest trade, but
  /// size. The encoder answers a ceiling it cannot meet by raising QP, and
  /// libwebrtc's QualityScaler reads QP and takes the resolution away.
  /// Measured on the device with a cast live: it opens AT the source,
  /// 2304x1440, and quality_scaler.cc drives it down three steps in 607 ms to
  /// 768x480, holds there fifteen seconds, and regains the source only
  /// forty-four seconds in. Congestion is not involved -- every
  /// video_stream_encoder.cc report across that window reads "dropped (due to
  /// congestion window pushback) 0". The receiver paints what it is sent at 1:1
  /// (receiver/main.c build_live_page), so the user watches a small picture
  /// grow for the first minute of every cast.
  ///
  /// 12 rather than the 20 the arithmetic alone would ask for. This is a
  /// ceiling and not a demand -- GCC still sets the actual rate and drops below
  /// it on a path that cannot hold it -- but the receiver has to decode
  /// whatever does arrive, and one notebook log already carries 123 QoS frame
  /// drops and "this computer is too slow" beside a D3D11 video device that
  /// failed to open with E_NOINTERFACE. Doubling is enough to move QP off the
  /// floor that triggers the scaler; going further is a question to settle
  /// after that decode path is understood, not before. Still well under the
  /// relay's 24 Mbit/s per-allocation cap (ops/turnserver.conf.template).
  static const maxBitrateBps = 12000000;

  /// Direct first, relay when direct cannot be had, matching the receiver's
  /// own default. This was relay-only on the grounds that neither peer would
  /// then learn the other's address, which was only ever half true: libwebrtc
  /// does empty a relayed candidate's related address under that filter, but
  /// libnice on the receiver has no such sanitiser and was sending this phone
  /// the desktop's address regardless.
  ///
  /// What it cost was every packet crossing a relay in another country, 47 ms
  /// of one-way path before anything else, and a retransmission crossing it
  /// twice — which is the whole reason the receiver's jitter buffer is 200 ms.
  /// On one Wi-Fi that structure covers a hop of a millisecond or two.
  ///
  /// ICE still gathers the relay candidates and still uses them on a network
  /// that blocks peer-to-peer traffic, which is the one this was built for;
  /// a host pair simply outranks a relay pair when both work.
  ///
  /// `--dart-define=AIRCAST_RELAY=true` puts relay-only back. The receiver has
  /// to agree (`--relay-only`): a mismatch is safe but pointless, because the
  /// relay-only side offers no host candidate for the other's to pair with.
  static const relayOnly = bool.fromEnvironment('AIRCAST_RELAY', defaultValue: false);

  RTCPeerConnection? _pc;
  MediaStream? _stream;

  /// Connected / disconnected / failed, for the UI. Relay-only ICE fails fast
  /// and silently otherwise.
  void Function(RTCPeerConnectionState state)? onState;

  /// Once a second while the connection is up, so the phone can show the same
  /// readings the desktop does.
  ///
  /// Both ends showing one number is the point. When the picture is small or
  /// slow the two people looking at it are usually looking at different
  /// screens, and "is it me or is it you" is not a question either end could
  /// answer before this existed.
  void Function(CastStats stats)? onStats;

  Timer? _statsTimer;

  /// Reads the peer connection's own report and reduces it to the four numbers
  /// worth a person's attention.
  ///
  /// The path is the one that decides everything else: a host pair is a hop on
  /// the same network, a relayed pair is a round trip through a machine in
  /// another country, and it is the only thing on this list a user can act on —
  /// by putting both ends on the same Wi-Fi.
  ///
  /// Read from the SELECTED pair rather than from the candidate list, because a
  /// connection gathers relay candidates it never uses; `state == 'succeeded'`
  /// is the one ICE actually settled on. Its local candidate carries the type.
  Future<CastStats?> _readStats(RTCPeerConnection pc) async {
    final reports = await pc.getStats();
    String? localId;
    double? rttMs;
    int? width, height, fps;
    var path = 'unknown';

    for (final r in reports) {
      final v = r.values;
      if (r.type == 'candidate-pair' && v['state'] == 'succeeded') {
        localId = v['localCandidateId'] as String?;
        final rtt = v['currentRoundTripTime'];
        // Seconds in the spec, milliseconds on a screen.
        if (rtt is num) rttMs = rtt.toDouble() * 1000;
      } else if (r.type == 'outbound-rtp' && v['kind'] == 'video') {
        final w = v['frameWidth'], h = v['frameHeight'], f = v['framesPerSecond'];
        if (w is num) width = w.toInt();
        if (h is num) height = h.toInt();
        if (f is num) fps = f.round();
      }
    }
    if (localId != null) {
      for (final r in reports) {
        if (r.id != localId) continue;
        final t = r.values['candidateType'] as String?;
        // libwebrtc's own spelling, passed through rather than prettied up
        // anywhere but here: host, srflx, prflx, relay.
        path = t == 'host' ? 'Direct' : (t == 'relay' ? 'Relayed' : 'Direct (NAT)');
      }
    }
    return CastStats(path: path, rttMs: rttMs, width: width, height: height, fps: fps);
  }

  Future<void> start() async {
    final turn = await _signaling.turn;

    // Android enforces three steps in this order, and the middle one is ours.
    // flutter_webrtc takes consent and then calls getMediaProjection() without
    // starting any service — its Android manifest declares none — and since
    // targetSdk 29 the platform answers that with
    //   SecurityException: Media projections require a foreground service of
    //   type ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
    // thrown on the main thread from native code, which kills the process
    // before any catch here runs. The app simply vanishes, with no Dart error
    // anywhere: that was the bug, on a Tab S10 FE running Android 16.
    //
    // Consent first, because a mediaProjection foreground service cannot be
    // started for an app the user has not granted capture to; the service
    // second; the capture last. requestCapturePermission caches the token it
    // obtains and getDisplayMedia reuses it, so the user still sees one dialog.
    if (Platform.isAndroid) {
      if (!await Helper.requestCapturePermission()) {
        throw Exception('Screen sharing was declined');
      }
      debugPrint('aircast: consent granted');
      await UsbCast.holdForeground();
      debugPrint('aircast: foreground service up');
    }

    // iOS: 'broadcast' selects the Broadcast Upload Extension, which is the
    // only way to capture the whole screen rather than this app's own window
    // (sender/ios/README.md). Without the extension installed the call falls
    // back to in-app capture, which for a mirroring product is useless.
    _stream = await navigator.mediaDevices.getDisplayMedia({
      'video': Platform.isIOS ? {'deviceId': 'broadcast'} : true,
      'audio': false,
    });

    debugPrint('aircast: capture started, ${_stream!.getVideoTracks().length} video track(s)');
    final pc = await createPeerConnection(turn.toConfiguration(relayOnly: relayOnly));
    _pc = pc;
    debugPrint('aircast: peer connection created');

    for (final track in _stream!.getVideoTracks()) {
      await pc.addTrack(track, _stream!);
    }
    // View-only: we never receive.
    for (final t in await pc.getTransceivers()) {
      await t.setDirection(TransceiverDirection.SendOnly);
    }
    await _preferCodecs(pc);
    await _capBitrate(pc);

    pc.onConnectionState = (state) => onState?.call(state);

    // One second, which is slow enough that getStats costs nothing and fast
    // enough that a reading is never stale by the time it is read. Started
    // here rather than on the connected state because the report is useful
    // before then too: until ICE picks a pair there is no selected candidate
    // and _readStats returns "unknown", which is the honest answer while a
    // connection is still being negotiated.
    _statsTimer?.cancel();
    _statsTimer = Timer.periodic(const Duration(seconds: 1), (_) async {
      final live = _pc;
      if (live == null || onStats == null) return;
      try {
        final s = await _readStats(live);
        if (s != null && _pc == live) onStats?.call(s);
      } on Object catch (e) {
        // getStats throws on a connection closed between the tick and the
        // call. Nothing here is worth ending a cast over, and the UI simply
        // keeps the last reading until the next tick replaces it.
        debugPrint('aircast: stats read failed: $e');
      }
    });

    pc.onIceCandidate = (c) => _signaling.sendCandidate(c.toMap().cast<String, dynamic>());
    _signaling.onAnswer = (sdp) async =>
        pc.setRemoteDescription(RTCSessionDescription(sdp, 'answer'));
    // sdpMid defaults to empty rather than null on purpose: libwebrtc's JNI
    // hands a null mid straight to a CHECK and aborts the process from
    // nativeAddIceCandidate, so one peer omitting a field the protocol calls
    // optional kills this app with no error anywhere. An empty mid is resolved
    // from sdpMLineIndex, which every peer sends.
    _signaling.onCandidate = (c) async => pc.addCandidate(RTCIceCandidate(
          c['candidate'] as String?,
          (c['sdpMid'] as String?) ?? '',
          c['sdpMLineIndex'] as int?,
        ));

    // The answerer cannot answer before it exists, so wait for it.
    await _signaling.peerJoined;
    final offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    _signaling.sendOffer(offer.sdp!);

    // Only now, so that the first offer and a re-offer can never be under
    // construction at the same time. A receiver can drop and come back while
    // the user is still reading the capture consent dialog: the completer above
    // was completed by the first of those frames the moment it arrived, so the
    // second lands here with nothing wired to it yet and is dropped. That is
    // the right outcome and not a hole — the offer built just above is the only
    // offer in flight, and it reaches whichever receiver is holding the role by
    // the time the server relays it.
    _signaling.onPeerRejoined = () async {
      try {
        await _reoffer();
      } on Object catch (e) {
        // Nothing awaits this: it runs from a signalling frame, so an error
        // escaping here would be an uncaught async error rather than something
        // main.dart could put on the status line. What the user sees is the
        // connection state, which is already wired to onState.
        debugPrint('aircast: the re-offer failed: $e');
      }
    };
  }

  /// True from the moment a re-offer starts until it is on the wire.
  ///
  /// Two offers in flight is the one way this can end worse than the bug it
  /// fixes: the receiver answers the first, this side has already replaced its
  /// local description with the second, and the answer then keys its
  /// connectivity checks to an ice-ufrag this side has thrown away. That is an
  /// ICE failure, which tears the cast down, rather than a negotiation that
  /// merely stalls. The window is the few milliseconds an offer takes to
  /// build, and a `peer` frame can only land inside it if a receiver rejoined
  /// twice that fast — its own reconnect backoff starts at a second and
  /// doubles (receiver/main.c).
  bool _reoffering = false;

  /// Offer again, to a receiver that has joined this code a second time.
  ///
  /// The renegotiation itself is ordinary: the capture track, the
  /// transceiver's codec preferences and the sender parameters all still
  /// belong to this peer connection, so the new offer is the old one with new
  /// ICE credentials and a bumped version, and nothing here has to be set up
  /// twice.
  ///
  /// The restart is what makes those credentials new, and it is not optional.
  /// The receiver that comes back has built a second webrtcbin, which means a
  /// second DTLS certificate: two runs of the shipped receiver on one code
  /// answered with fingerprints 59:14:F4:19… and 48:E7:4E:17… and with a
  /// different ice-ufrag each time. JSEP is explicit about that case (RFC 8829
  /// §5.10) — a changed remote fingerprint tears the DTLS connection down, and
  /// if the answer that changed it does not also carry new ICE credentials, an
  /// error MUST be generated instead. Asking for the restart here is what puts
  /// new credentials in our own offer and lets the answer's be legal. It costs
  /// a re-gather: the same host candidates from the same interfaces, and a
  /// fresh allocation on the relay.
  ///
  /// restartIce() rather than an `IceRestart` constraint on createOffer,
  /// because flutter_webrtc's Android side reads only the `mandatory` and
  /// `optional` keys of the constraints map (MediaConstraintsUtils), so a flat
  /// one would be dropped on the floor without a word.
  Future<void> _reoffer() async {
    final pc = _pc;
    // No connection to offer on: stop() has already disposed it, and this
    // handler is still wired to a session the UI has let go of.
    if (pc == null || _reoffering) return;
    _reoffering = true;
    try {
      await pc.restartIce();
      final offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      _signaling.sendOffer(offer.sdp!);
    } finally {
      _reoffering = false;
    }
  }

  /// Congestion control decides the bitrate; this is only the ceiling. It has
  /// to stay under coturn's per-allocation `max-bps` (24 Mbit/s, and that option
  /// is in *bytes*, and the relay silently drops what exceeds it within any one
  /// wall-clock second, so the relay's ceiling has to clear a key frame's burst
  /// and not just this average), and the same number is the USB path's default.
  Future<void> _capBitrate(RTCPeerConnection pc) async {
    for (final sender in await pc.getSenders()) {
      if (sender.track?.kind != 'video') continue;
      final params = sender.parameters;
      final encodings = params.encodings;
      if (encodings == null || encodings.isEmpty) continue;
      for (final e in encodings) {
        e.maxBitrate = maxBitrateBps;
        // A floor, so the first seconds are not soft: libwebrtc's congestion
        // control starts conservative and ramps, and for a screen that reads as
        // a blurry open that slowly sharpens. 2 Mbit/s is well under the relay's
        // 24 Mbit/s per-allocation cap and keeps text legible from the first
        // frame. If the path genuinely cannot hold it, GCC still drops below.
        e.minBitrate = 2000000;
        // The tablet composites on a 16.67 ms grid and this cap was throwing
        // away every other tick. Of the 39,332 inter-frame RTP timestamp gaps
        // in one 28-minute receiver log, 17.6% are one tick, 47% two and 17.7%
        // three; only 45 are shorter than 12 ms and 22 and 44 ms are all but
        // empty, so the grid is 60 Hz and not the panel's 90. The cap is the
        // only limiter there is: flutter_webrtc hands the capturer a frame rate
        // and discards it, naming the argument `ignoredFramerate` in
        // OrientationAwareScreenCapturer.startCapture, so all of the decimation
        // happens here. While the screen is moving a change waits a mean of
        // 18.5 ms for a frame that will carry it; one tick per frame halves
        // that to 8.3. 60 is also what libwebrtc uses when this field is left
        // unset (kDefaultVideoMaxFramerate, media/base/media_constants.cc), so
        // this is the cap getting out of the way rather than a new demand. It
        // can still be taken back: getDisplayMedia builds the source with
        // createVideoSource(true), and a screencast source sheds CPU overuse
        // whichever way the preference below names. It landed: every encoder
        // session in `adb shell dumpsys media.metrics` reports
        // frame-rate=6.000000e+01 at width 2304, height 1440.
        e.maxFramerate = 60;
      }
      // Name the degradation preference, because the round trip through
      // flutter_webrtc has been naming it for us. libwebrtc leaves
      // degradation_preference unset, so the JNI hands Java a null and the
      // Android getter omits the key; webrtc_interface maps the missing key
      // straight to BALANCED with no null guard, its
      // degradationPreferenceforString falling through to that value; and toMap
      // sends it back down. So every setParameters in this method has quietly
      // written "balanced" since the day it was written, and the
      // MAINTAIN_RESOLUTION a screencast source is supposed to inherit is only
      // reached while nobody names a preference at all.
      //
      // Naming MAINTAIN_FRAMERATE changes no behaviour at this frame size, and
      // that is the honest reason for it rather than any latency claim.
      // 2304x1440 is 3,317,760 pixels and BALANCED only starts shedding frames
      // below 640x480: its down-step asks MinFps first, which has no config
      // above 640x480 and returns int max, so CanDecreaseFrameRateTo is false
      // and the case falls through into MAINTAIN_FRAMERATE's DecreaseResolution
      // anyway. Five 3/5 steps separate us from the size where the two differ.
      // What this line buys is that the preference is ours and stays ours if
      // the resolution ever drops. If we ever cast to read text while the
      // tablet is busy, MAINTAIN_RESOLUTION is the one identifier to change.
      //
      // Never MAINTAIN_FRAMERATE_AND_RESOLUTION: webrtc_interface defines it
      // but libwebrtc's Java enum has no such constant, and the JNI answers an
      // unknown name with a check that aborts the process rather than throwing
      // something Dart can catch.
      params.degradationPreference = RTCDegradationPreference.MAINTAIN_FRAMERATE;
      await sender.setParameters(params);
    }
  }

  /// Reorder the video transceiver's codec preferences to H.264 then VP8, and
  /// drop everything else so the receiver only ever sees two tails.
  Future<void> _preferCodecs(RTCPeerConnection pc) async {
    final caps = await getRtpSenderCapabilities('video');
    final codecs = caps.codecs ?? const <RTCRtpCodecCapability>[];
    bool named(RTCRtpCodecCapability c, String name) =>
        c.mimeType.toLowerCase() == 'video/$name';

    final preferred = [
      ...codecs.where((c) => named(c, 'h264')),
      ...codecs.where((c) => named(c, 'vp8')),
      // Keep RTX/red/ulpfec — dropping them would drop retransmission with them.
      ...codecs.where((c) => !named(c, 'h264') && !named(c, 'vp8') && !named(c, 'vp9') && !named(c, 'av1')),
    ];
    if (preferred.isEmpty) return;

    for (final t in await pc.getTransceivers()) {
      if (t.sender.track?.kind == 'video') {
        await t.setCodecPreferences(preferred);
      }
    }
  }

  Future<void> stop() async {
    // Before the connection goes: the tick calls getStats on it.
    _statsTimer?.cancel();
    _statsTimer = null;
    for (final track in _stream?.getTracks() ?? const <MediaStreamTrack>[]) {
      await track.stop();
    }
    await _stream?.dispose();
    // close() then dispose(), because in flutter_webrtc they are not the same
    // thing. close() stops the transports; dispose() is what cancels the Dart
    // event subscription and frees the native PeerConnection. Without the
    // second call every start and stop in one app run leaves a native
    // connection behind, and the handler still subscribed still points at
    // onState, which points at _stop, on a session the UI has replaced.
    await _pc?.close();
    await _pc?.dispose();
    _stream = null;
    _pc = null;
    // The notification outlives the capture, not the other way round. A no-op
    // when start() never got as far as raising it.
    if (Platform.isAndroid) await UsbCast.stop();
  }
}

/// What the phone shows about a cast in progress, and the same four readings
/// the desktop puts in its own strip.
///
/// Immutable and rebuilt each second rather than mutated, so a widget that
/// holds one is holding a consistent set: a half-updated reading would show a
/// new resolution beside the old round trip, which is worse than showing
/// nothing.
class CastStats {
  const CastStats({
    required this.path,
    this.rttMs,
    this.width,
    this.height,
    this.fps,
  });

  /// 'Direct', 'Direct (NAT)', 'Relayed', or 'unknown' before ICE settles.
  final String path;

  /// Round trip on the selected pair. Null until there is a selected pair.
  final double? rttMs;

  /// What the encoder is actually sending, which is not the screen's size:
  /// libwebrtc's quality scaler takes resolution away when QP rises, and this
  /// is where that becomes visible instead of mysterious.
  final int? width, height;
  final int? fps;

  String get pictureLabel =>
      (width != null && height != null) ? '$width×$height' : '—';

  String get rttLabel => rttMs == null ? '—' : '${rttMs!.round()} ms';
}
