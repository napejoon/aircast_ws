# Transport: WebRTC vs custom H.264 stream

Research asset for [#2](https://github.com/napejoon/aircast_ws/issues/2) (map [#1](https://github.com/napejoon/aircast_ws/issues/1)).
Date: 2026-09-09. Method: primary sources only (RFC/IETF/W3C, developer.android.com, developer.apple.com,
learn.microsoft.com, project repos). Every sub-topic report was re-checked by an independent adversarial
citation checker that fetched each cited URL. Numbers with no primary source are marked UNSOURCED and are
NOT used as decision inputs.

Fixed constraints: sender Android + iOS, receiver Windows + Ubuntu, video only, view-only, 1:1,
200-500 ms acceptable, cross-network via own self-hosted signaling + TURN/relay, Android USB cast in scope,
iOS USB out of scope, recording at receiver only.

## 1. Verdict

**Use WebRTC (H.264 + DTLS-SRTP + ICE, forced-relay through own coturn) as the cross-network transport.
Add a second, USB-only path that is a raw H.264 elementary stream over a plain TCP socket tunnelled by
`adb reverse` (the scrcpy shape).** Do not hand-roll the cross-network transport.

Three reasons, each sourced below:

1. **Adaptive bitrate is the whole game, and only WebRTC ships one.** No standards-track document specifies a
   congestion-control *algorithm* for interactive media — RFC 8834 §7 says so in as many words ("there is no
   standard congestion control algorithm that can be used for interactive media applications such as WebRTC's
   flows … will have to be proprietary"). But libwebrtc *implements* one (GCC + transport-wide-cc), and a custom
   design gets nothing: RFC 3550 §10 delegates congestion control to the profile, and RFC 4585 states outright
   that "RTCP feedback itself is insufficient for congestion control purposes". Building the loop yourself means
   measuring per-packet arrival times and feeding them back every 50-200 ms (RFC 8888 §3-§4), handling feedback
   loss (RFC 8888 §5 MUST), and reacting to a Wi-Fi-to-cellular change within "a few RTTs … likely within a
   second" (RFC 8836 §2 req 1(c)-(d)).
2. **iOS makes the custom path undocumented territory.** Apple documents no persistent-socket guarantee for a
   Broadcast Upload Extension (the extension guide offers only NSURLSession background transfers and states
   VoIP-style background execution is "not available to extensions"), documents no memory-limit number, and
   documents *no* mid-session writability for any VideoToolbox bitrate property — the only documented runtime
   mechanism is probing `kVTPropertyReadWriteStatusKey`. A custom ABR loop needs exactly that mid-session
   bitrate write. WebRTC's iOS path hides all of it, and the ReplayKit-extension-to-WebRTC pattern is at least
   a documented, trodden one (LiveKit's own ios-screen-sharing doc).
3. **Nothing else in the field does this shape.** No open-source project sends Android *and* iOS screen to a
   Windows *and* Ubuntu receiver over a self-hosted non-WebRTC relay. The near misses each fail on one axis:
   scrcpy is Android-only, adb-only, and its own docs say wireless requires "the same network as the computer";
   RustDesk ships its own relay (hbbs/hbbr) but its docs say "iOS does not yet support screen sharing";
   ws-scrcpy's iOS support is "Experimental … not built by default" and its iOS-over-USB capture route has no
   Windows support at all ("I have given up on windows support"); UxPlay is LAN-only; Guacamole has no phone
   capture source. Choosing custom means being first, with no reference implementation to copy.

What the custom path *does* win: the USB case. `adb forward`/`reverse` carry TCP only — adb's socket spec
recognises `tcp:`, `vsock:`, `acceptfd:` and the `local*` family, all created `SOCK_STREAM`, with no UDP scheme
anywhere (a code-level fact; no Android page states the restriction as prose). So a TCP stream runs over USB
with only its address changed, while an ICE/UDP design has no adb path at all. That is why the recommendation is
two paths, not one.

## 2. Transport candidates, as their own specs describe them

| Transport | Doc status | Encryption built in | Congestion control built in | Loss recovery built in | NAT story | First-party latency statement |
|---|---|---|---|---|---|---|
| WebSocket/TLS (RFC 6455) | Standard | via TLS | none | TCP retransmit (HOL blocking) | outbound only | none |
| QUIC + DATAGRAM (RFC 9000/9221) | Standards Track | mandatory (TLS 1.3) | yes (NewReno per RFC 9002, CUBIC allowed) | streams yes; datagrams never retransmitted | no hole punching; survives address change | none |
| WebTransport (W3C CR Snapshot 2026-07-30; IETF drafts in WG Last Call) | unstable by its own words | via QUIC | via QUIC | datagrams unreliable on H3; **on H/2 both properties are lost** | outbound only | none |
| MoQ (draft-ietf-moq-transport-21, 2026-09-08) | WG draft, 159 pp, moving | via QUIC | via QUIC | app-defined | relays in charter | none |
| Hand-written RTP/UDP (RFC 3550 + 6184) | Standard | none (add SRTP RFC 3711, which excludes key mgmt) | none (§10 delegates to profile) | none (add RFC 4585/4588/5109/8627) | you add ICE | none |
| SRT | MPLv2.0; protocol draft EXPIRED (individual, Informational) | AES | "live" controller | ARQ | rendezvous mode | "ultra low (sub-second)"; `SRTO_LATENCY` default **120 ms** |
| RIST | VSF TR-06; **patent claims over TR-06-1 §§4-5.4** | Main profile: DTLS/PSK | no | RFC 4585 NACK | GRE tunnel | suggested receiver buffer **1000 ms** |
| HLS (RFC 8216) | Standard | — | — | — | — | 10 s target duration + 3-duration hold-back ≈ 30 s |
| LL-HLS (draft-pantos-hls-rfc8216bis-22) | ISE Informational | — | — | — | — | PART-HOLD-BACK MUST be >= 2x part duration; with Apple's own 200 ms example that is >= 400 ms **before** encode/network/decode |
| DASH low latency (DASH-IF CR) | industry doc | — | — | — | — | "target latency of typically 2 to 10 seconds" |

Rulings: HLS, LL-HLS and DASH are out on their own numbers. SRT and RIST are broadcast-shaped — SRT's README
says nothing about mobile senders (iOS/Android appear only as build targets), RIST's suggested defaults sit
outside the budget, and RIST carries a Video-Flow Ltd IPR claim over the core of its Simple Profile with no
stated terms. MoQ is the interesting future — its WG charter explicitly scopes relays and non-browser
endpoints — but the draft moved to -21 on 2026-09-08 and every relay implementation on the WG's own interop
wiki lags it. Not a foundation for this project yet.

## 3. What a custom design makes you write

Free in WebRTC (as a libwebrtc implementation, not as a standard), yours to write otherwise:

- **Congestion control / ABR feedback loop** — see §1.1. The only Standards-Track piece is RFC 8888, a feedback
  *format*, not an algorithm. NADA (RFC 8698) and SCReAM (RFC 8298) are Experimental; GCC's draft expired at
  revision 02 in 2016 and never became an RFC.
- **H.264 packetization** (RFC 6184): mode 1 (single NALU + STAP-A + FU-A) is the low-delay choice; mode 0
  forbids fragmentation. Note the commonly quoted ~1200-byte MTU cap is **not** in RFC 6184 — all six MTU
  mentions are advisory. Losing SPS/PPS is the fatal case: §8.4 says "a reference to a corrupt parameter set
  normally has fatal results to the decoding process", and §1.2 wants parameter sets sent reliably and in
  advance — which a reliable custom transport actually makes easy.
- **Playout / jitter buffer**: RFC 3550 specifies none. The word "playout" does not appear in the document.
  Only the interarrival-jitter formula (§6.4.1) is mandated.
- **Loss recovery and keyframe requests**: PLI, not FIR (RFC 5104 §3.5.1 explicitly disallows FIR for error
  recovery), plus request suppression — RFC 5104 requires waiting 2x the longest known RTT before repeating,
  or a loss burst turns into an IDR storm. With no recovery at all, a lost P-frame is not a blink: RFC 6184
  §12.2 calls it "decoder drift", cured only by an intra refresh.
- **Retransmission is marginal at this budget**: RFC 4588 §1 says interactivity's "at most a few hundred
  milliseconds … usually excludes the use of retransmission".
- **Encryption**: TLS over TCP/QUIC gives replay and order protection because it demands them of the transport
  (RFC 8446 §1). DTLS makes replay detection "optional" (RFC 9147 §3.4) and provides no order protection
  (§11). SRTP excludes key management entirely (RFC 3711 §8) and sends RTP headers in the clear (§9.4).
- **Reconnect on a network change**: TCP/TLS gives nothing (new connection). QUIC migration survives a client
  address change via connection IDs, but RFC 9000 §9.4 requires resetting the congestion controller and RTT
  estimator to initial values — so the bitrate ramp restarts and the recovery behaviour is still yours. Only
  clients may migrate, and migration is forbidden before handshake confirmation (§9.2).
- **Dropping audio does remove real work**: RFC 3550's whole inter-media sync half (NTP wallclock alignment,
  lip-sync drift) goes away. Intra-media playout timing remains — and loses audio as its master clock.

Always-relay (both endpoints dial out to a public-address server) legitimately skips ICE (RFC 8445), STUN as a
traversal tool, and TURN's Allocate/Refresh/ChannelBind control protocol. The price is the one RFC 8656 §2
names: "it comes at a high cost to the provider of the TURN server … it is best to use a TURN server only when
a direct communication path cannot be found." This project chooses that last-resort case as its default, which
is a cost decision, not a technical error.

## 4. Sender capture + encode (both branches need this)

| | Android | iOS |
|---|---|---|
| Capture | MediaProjection + `createVirtualDisplay` into a Surface; consent per session (API 34+: one token, one call); Android 14+ needs `foregroundServiceType="mediaProjection"` started *before* `getMediaProjection` | ReplayKit in-app `startCapture` or Broadcast Upload Extension (`processSampleBuffer`) — **all ReplayKit capture entry points are deprecated as of 27.0**; Apple: "ScreenCaptureKit replaces ReplayKit for screen streaming and mirroring. A broadcast extension is no longer necessary." ScreenCaptureKit on iOS is 27.0 **beta** |
| Encode | `MediaCodec.createInputSurface()` (API 18) = zero-copy Surface-to-hardware-encoder | VideoToolbox `VTCompressionSession`; `EnableLowLatencyRateControl` (iOS 14.5+) forces infinite GOP, no B-frames, High profiles — **creation-time only** |
| Runtime bitrate change | `setParameters` + `PARAMETER_KEY_VIDEO_BITRATE` (API 19), documented, though "some of these parameter changes may silently fail to apply" | **Not documented.** No Apple property page states mid-session writability; the documented probe is `VTSessionCopySupportedPropertyDictionary` → `kVTPropertyReadWriteStatusKey` |
| Force keyframe | `PARAMETER_KEY_REQUEST_SYNC_FRAME` (API 19), best-effort | `kVTEncodeFrameOptionKey_ForceKeyFrame`; keyframe *detection* is inverted — only `kCMSampleAttachmentKey_NotSync` exists |
| Bitstream framing | start codes documented only for parameter sets (csd-0/csd-1); "Annex B" appears nowhere in the docs — verify at runtime | AVCC / length-prefixed (`NALUnitLength` in an "AVC decoder configuration record"); Apple never says "convert to Annex-B yourself" |
| Latency knobs | `KEY_LATENCY` (API 26, in frames); `KEY_INTRA_REFRESH_PERIOD` (API 24) is the one key Android itself calls "recommended for video streaming applications"; `KEY_LOW_LATENCY` is **decoder-only** | `AllowFrameReordering` defaults to **true** and `MaxFrameDelayCount` to unlimited — both wrong here, both must be set explicitly |
| Codec floor | H.264 Baseline encoder mandatory since Android 3.0; Main encoder only "recommended" 6.0+; HEVC encoder has no mandate; AV1 encoder+decoder mandatory from Android 14 | low-latency rate control is H.264-only |

Extension constraints worth planning around: `processSampleBuffer` is invoked serially and "the sample buffer
passed to this method is available only until the method returns", audio buffers arrive regardless (continuous
silence when idle), and MediaCodec warns that "holding onto input and/or output buffers may stall the codec" —
i.e. never block the codec on a socket write.

## 5. Receiver decode + render (both branches need this)

- Feed libavcodec directly with `avcodec_send_packet` — no libavformat, no custom AVIO — but pad the buffer by
  `AV_INPUT_BUFFER_PADDING_SIZE`. FFmpeg's own docs warn that input probing costs latency (`analyzeduration`:
  "will increase latency"; `fflags nobuffer` is scoped to startup analysis only).
- GStreamer's defaults are latency traps: `queue` defaults to 1 s / 200 buffers with `leaky=no`, and
  `rtpjitterbuffer` defaults `latency` to **200 ms** — alone the low end of the whole budget.
- Only `nvh264dec` exposes a latency knob (`max-display-delay`, default -1); `vah264dec` and `d3d11h264dec`
  document none. NVDEC's own API does: `ulMaxDisplayDelay` "0 = no delay", plus `CUVID_PKT_ENDOFPICTURE` — a
  natural fit when the transport delivers one access unit per message.
- Presentation is where naive code loses a frame or three: DXGI `MaxLatency` "defaults to 3", Microsoft says a
  plain Present loop waits "almost a full extra frame", waitable swap chains default to 1 ("the least possible
  latency"), and Independent Flip can reach "1 frame of latency". wgpu corroborates the pattern: `Fifo` is
  "approximately 3 frames long", `Mailbox` one, `Immediate` none.
- Off-the-shelf receiver: mpv's `[low-latency]` profile is exactly `audio-buffer=0, vd-lavc-threads=1,
  cache-pause=no, fflags=+nobuffer, probe-info=nostreams, analyzeduration=0.1, video-sync=audio,
  interpolation=no, video-latency-hacks=yes, stream-buffer-size=4k` — useful as a throwaway receiver for the
  USB/TCP path on day one.
- Licences: FFmpeg LGPL-2.1+ (GPL-2+ if GPL parts enabled), GStreamer LGPL, mpv GPLv2+, SDL3 zlib, GLFW
  zlib/libpng, wgpu Apache-2.0 + MIT, libva MIT. The NVIDIA Video Codec SDK licence was not read — read it
  before depending on NVDEC in a shipped receiver.

## 6. The USB path

- `adb reverse` (device listens, computer connects) is scrcpy's default, with `adb forward` as fallback; adb
  carries **TCP only**.
- adb costs a consumer-facing UX price: USB debugging lives under Developer options (hidden by default since
  Android 4.2), and the RSA host-key dialog means adb commands "cannot be executed unless you're able to
  unlock the device and acknowledge the dialog". Windows also needs an OEM USB driver.
- AOA (Android Open Accessory) avoids adb entirely — the desktop takes the accessory/host role, control
  requests 51/52/53, the device re-appears as VID 0x18D1 / PID 0x2D00-0x2D01 with bulk endpoints and
  16384-byte buffers. AOSP itself ships a host-side implementation
  (`frameworks/base/libs/usb/tests/accessorytest/usb.c`), so Linux can do it; on Windows libusb needs
  WinUSB/libusbK binding (Zadig). Android states no throughput figure. AOA v2 is still documented; only its
  audio output was deprecated (Android 8.0).
- USB tethering / RNDIS is **not** an option: `TETHERING_USB` is `@hide @SystemApi` and `startTethering`
  requires `TETHER_PRIVILEGED`.

## 7. What no document can answer (spike material)

- End-to-end latency of either branch over a real relayed cross-network path. **No** primary source in this set
  publishes a measurement; every number above is a protocol default, a normative floor, or a project's own
  self-report. scrcpy's "35~70 ms" is a local Nexus-5 measurement that does not separate USB from Wi-Fi.
- Whether an iOS Broadcast Upload Extension can hold a long-lived socket, and what its real memory ceiling is.
  Apple states only "significantly lower" — the widely quoted 50 MB appears solely in forum posts quoting OS
  crash text.
- Whether VideoToolbox bitrate properties are actually writable mid-session on current iOS.
- Whether MediaCodec's coded-slice output is Annex-B framed (the docs cover parameter sets only).
- Whether either platform emits frames at all on a fully idle screen.
- Whether ScreenCaptureKit-on-iOS (27.0 beta) is usable in time, which would delete the extension boundary
  entirely — the sample declares `screen-capture` in `UIBackgroundModes`, so in-app capture survives
  backgrounding.

## 8. Reliability of this document

Six sub-topic reports, each re-verified by an independent checker that fetched the cited URLs. Result: no
KEY CLAIM was found factually wrong or citing a page that fails to support it. Corrections applied:
RFC 9000 migration quotes belong to §9.2/§9.3/§9.4/§9.5 rather than a bare §9; the adb-has-no-UDP fact is
inference from adb's source, not a documented sentence; GADS is dual-licensed (AGPL-3.0 core + proprietary
hub-ui), not plain AGPL-3.0; a Cloudflare MoQ public-relay detail could not be re-confirmed on the WG wiki.
Everything the reports marked UNSOURCED stayed unsourced under re-check, with one addition: RFC 9293's
4-tuple definition would upgrade the "TCP identity dies on a network change" claim from inference to sourced.
