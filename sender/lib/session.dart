import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'signaling.dart';

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

  RTCPeerConnection? _pc;
  MediaStream? _stream;

  /// Connected / disconnected / failed, for the UI. Relay-only ICE fails fast
  /// and silently otherwise.
  void Function(RTCPeerConnectionState state)? onState;

  Future<void> start() async {
    final turn = await _signaling.turn;

    // Android: the consent prompt and the mediaProjection foreground service
    // both live behind this call, in flutter_webrtc's GetUserMediaImpl.
    // iOS: 'broadcast' selects the Broadcast Upload Extension, which is the
    // only way to capture the whole screen rather than this app's own window
    // (sender/ios/README.md). Without the extension installed the call falls
    // back to in-app capture, which for a mirroring product is useless.
    _stream = await navigator.mediaDevices.getDisplayMedia({
      'video': Platform.isIOS ? {'deviceId': 'broadcast'} : true,
      'audio': false,
    });

    final pc = await createPeerConnection(turn.toConfiguration());
    _pc = pc;

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
    _signaling.onCandidate = (c) async => pc.addCandidate(RTCIceCandidate(
          c['candidate'] as String?,
          c['sdpMid'] as String?,
          c['sdpMLineIndex'] as int?,
        ));

    // The answerer cannot answer before it exists, so wait for it.
    await _signaling.peerJoined;
    final offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    _signaling.sendOffer(offer.sdp!);
  }

  /// Congestion control decides the bitrate; this is only the ceiling. It has
  /// to stay under coturn's per-allocation `max-bps` (8 Mbit/s — that option
  /// is in *bytes*, and the relay drops what exceeds it), and the same number
  /// is the USB path's default.
  Future<void> _capBitrate(RTCPeerConnection pc) async {
    for (final sender in await pc.getSenders()) {
      if (sender.track?.kind != 'video') continue;
      final params = sender.parameters;
      final encodings = params.encodings;
      if (encodings == null || encodings.isEmpty) continue;
      for (final e in encodings) {
        e.maxBitrate = maxBitrateBps;
        e.maxFramerate = 30;
      }
      await sender.setParameters(params);
    }
  }

  /// Reorder the video transceiver's codec preferences to H.264 then VP8, and
  /// drop everything else so the receiver only ever sees two tails.
  Future<void> _preferCodecs(RTCPeerConnection pc) async {
    final caps = await getRtpSenderCapabilities('video');
    final codecs = caps.codecs;
    bool named(RTCRtpCodecCapability c, String name) =>
        (c.mimeType ?? '').toLowerCase() == 'video/$name';

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
  }
}
