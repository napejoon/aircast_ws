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

  /// 6 Mbit/s, the 1080p ceiling from #8.
  static const maxBitrateBps = 6000000;

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
    for (final track in _stream?.getTracks() ?? const <MediaStreamTrack>[]) {
      await track.stop();
    }
    await _stream?.dispose();
    await _pc?.close();
    _stream = null;
    _pc = null;
    // The notification outlives the capture, not the other way round. A no-op
    // when start() never got as far as raising it.
    if (Platform.isAndroid) await UsbCast.stop();
  }
}
