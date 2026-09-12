# Stack options for capture + streaming

Research asset for [#3](https://github.com/napejoon/aircast_ws/issues/3) (map [#1](https://github.com/napejoon/aircast_ws/issues/1)).
Date: 2026-09-10. Method: primary sources only — each package's own registry entry, repo, README, source
files and CI scripts; first-party platform docs. Six options were researched in parallel; the
decision-critical claims were then re-checked by two independent adversarial fact-checkers that fetched
every cited URL. Anything no primary source settles is marked UNSOURCED and is not used as a decision input.

Constraints carried in from [#2](https://github.com/napejoon/aircast_ws/issues/2): WebRTC (H.264, DTLS-SRTP,
ICE forced through own coturn) for the cross-network path; a second USB-only path carrying raw H.264 over a
plain TCP socket via `adb reverse`; recording at the receiver; 1:1, video only, 200-500 ms.

## 1. The finding that outranks the stack question

**H.264-only is not a safe sender floor on Android, and this is a build fact, not an opinion.**

- Upstream `webrtc.gni` computes `rtc_use_h264 = proprietary_codecs && !is_android && !is_ios && !(is_win && !is_clang)`,
  with the comment "this is supported on all platforms except Android and iOS". `internal_encoder_factory.cc`
  includes the OpenH264 adapter only `#if defined(WEBRTC_USE_H264)`. So libwebrtc has **no software H.264 on
  Android by construction**.
- The AAR everyone actually uses is built with it off: `webrtc-sdk/webrtc-build`'s `build/run.py` sets
  `rtc_use_h264=false` in `COMMON_GN_ARGS`, applied to the Android path, and `webrtc-sdk/android`'s workflow
  states it "Fetches webrtc.android*.tar.gz artifacts from a workflow run in webrtc-sdk/webrtc-build". (A
  root `webrtc_build_settings.yaml` says `rtc_use_h264: true` — it is dead config, single commit from
  2021, never read by `run.py`.)
- `DefaultVideoEncoderFactory.createEncoder()` wraps hardware in a software fallback only
  `if (hardwareEncoder != null && softwareEncoder != null)`. With no software H.264 on Android, H.264 has
  **no fallback at all**.
- Worse than expected: `HardwareVideoEncoderFactory` recognises hardware H.264 only when the codec name
  starts with `QCOM_PREFIX` or `EXYNOS_PREFIX`, and carries a hardcoded blocklist
  (`"SAMSUNG-SGH-I337", "Nexus 7", "Nexus 4"`, commented "List of devices with poor H.264 encoder quality").
  High Profile is Exynos-only. **A MediaTek or Unisoc phone is therefore not considered H.264-capable by
  libwebrtc, even though its MediaCodec can encode H.264.**
- Meanwhile RFC 7742 §5 requires both codecs of both endpoint kinds, in identical wording for each:
  "WebRTC Non-Browsers that support transmitting and/or receiving video MUST implement the VP8 video codec
  … and H.264 Constrained Baseline". Android's own platform table makes the VP8 encoder required since
  Android 4.3, and the shipped AAR does contain software VP8 (libvpx).

**Consequence for the spec: offer H.264 and VP8, in that preference order, and make the receiver decode
both.** H.264 stays the default (hardware, cheap, matches the USB path's raw stream); VP8 is the floor that
keeps a MediaTek phone from producing a black screen. This amends #2's "H.264" to "H.264 preferred, VP8
mandatory fallback" — the transport decision itself is unchanged.

Two related build facts, both traced to live CI scripts rather than READMEs:

- The prebuilt **desktop** libwebrtc that `flutter_webrtc` downloads (`webrtc-sdk/libwebrtc`, tag
  `libwebrtc.m150.7871.01`) **does** include H.264: `build/libwebrtc_linux_build.sh` passes
  `ffmpeg_branding="Chrome" rtc_use_h264=true`, and `build/libwebrtc_win_build.cmd` passes
  `rtc_use_h264=true ffmpeg_branding="Chrome" rtc_use_h265=true`. (That repo's README is stale — it still
  says m144.)
- On iOS the situation is the opposite of Android: `RTCDefaultVideoEncoderFactory.supportedCodecs` adds
  H.264, VP8 and VP9 unconditionally (only AV1 is `#if`-guarded), because `RTCVideoEncoderH264` wraps
  VideoToolbox rather than OpenH264.

## 2. Receiver: GStreamer is the only option that covers every requirement first-party

| Candidate | Receives WebRTC | Decodes H.264 | Renders | Records | USB path reuse | Windows | Ubuntu |
|---|---|---|---|---|---|---|---|
| **GStreamer `webrtcbin` + stock tail** | yes | yes (stock decoders) | yes (stock sinks) | yes (`tee` → mux, no re-encode) | yes, same decoder+sink | first-party installers | apt (`plugins-bad`, `libav`) |
| LiveKit C++ SDK | yes | yes (libwebrtc) | **no** — raw frames only | no | no | x64 | x64, arm64 |
| LiveKit Rust SDK | yes | yes | no (a `wgpu_room` example exists in-tree) | no | no | yes (no hw codec row) | yes |
| libwebrtc C++ direct | yes | yes | no (`VideoSinkInterface::OnFrame`) | no | no | GN/Ninja + clang only | GN/Ninja |
| webrtc-rs | yes | **no codecs** | no | no | no | undeclared | undeclared |
| str0m | transport only | no | no | no | no | declares targets, but no TURN client, **no adaptive jitter buffer** | same |
| Pion (Go) | yes | no (packetizers only) | no | no | no | undeclared | undeclared |
| aiortc (Python) | yes | yes (PyAV) | no render helper | yes (`MediaRecorder`) | no | classifier only | classifier only |
| Electron receiver | yes (Chromium) | **yes** — stock build sets `proprietary_codecs = true`, `ffmpeg_branding = "Chrome"` | yes | yes (MediaRecorder → Node `fs`) | no (page cannot open a TCP socket) | yes | yes, but `autoUpdater` has "no built-in support … on Linux" |
| Tauri receiver | **no on Ubuntu** | n/a | yes | yes (fs plugin + permission scope) | no | WebView2 fine | **dead** |
| flutter_webrtc receiver | yes | yes (desktop zips do carry H.264) | yes, but CPU `ConvertToARGB` per frame | **no** — no `startRecordToFile` in the desktop dispatcher | no | yes | yes |

Why GStreamer wins on this project's exact shape:

- `webrtcbin` exposes the two properties #2's design needs as configuration rather than code:
  `turn-server` ("of the form turn(s)://username:password@host:port") and `ice-transport-policy`, plus a
  jitter-buffer `latency` property defaulting to 200 ms — the first knob to tune against the budget.
- Its pads are `application/x-rtp` only, so you link the tail yourself: `rtph264depay ! h264parse !`
  decoder `! ` sink. Every element is stock — `rtph264depay` (Good), `d3d12h264dec` / `vah264dec` /
  `openh264dec` / `avdec_h264` (Bad, libav), `d3d12videosink` / `glimagesink`. VP8's tail
  (`rtpvp8depay ! vp8dec`) is equally stock, which is what makes §1's dual-codec floor cheap.
- **Recording is a `tee` branch off the depayloaded stream**, so the receiver can write the sender's
  original H.264 to disk with no decode-re-encode round trip. Every other native candidate makes recording
  hand-written work; the Electron receiver can only record via `MediaRecorder`, i.e. decode plus re-encode.
- **The USB path reuses the same pipeline**: `tcpclientsrc ! h264parse ! ` decoder `! ` sink. One decoder,
  one renderer, one recording branch, two transports. No other candidate gives that.
- Licensing is workable: core and plugins are LGPL, and patent-encumbered plugins are segregated into
  `gst-plugins-ugly`.

Watch-outs, stated rather than glossed:

- `webrtcsrc` (the "all-batteries-included" element that decodes internally and ships its own signalling
  server) lives in `gst-plugins-rs`, and **Ubuntu has no `gstreamer1.0-plugins-rs` package** —
  packages.ubuntu.com returns "Sorry, your search gave no results". On Windows the 1.28 notes say Rust
  plugins do ship, except in the ARM64 installer. So the portable choice is `webrtcbin`, not `webrtcsrc`.
- Neither the GStreamer download page nor the Windows install page enumerates which plugins the installers
  contain. Verify with `gst-inspect-1.0 webrtcbin` / `avdec_h264` right after install, on both OSes.
- The two first-party pages disagree on current stable (download page 1.28.6, releases index 1.28.7).

Runner-up, for the record: **Electron** is a genuinely viable receiver — its stock build really does ship
H.264, `setWebRTCUDPPortRange`/`setWebRTCIPHandlingPolicy` are documented, and recording is a renderer→main
IPC hop to Node `fs`. It loses on the USB path (needs a Node TCP server plus your own Annex-B framing and
WebCodecs handoff), on recording cost (decode+re-encode), and on Linux updates. **Tauri is disqualified for
the Ubuntu receiver**, not by preference but by build flags: WebKitGTK defaults
`ENABLE_WEB_RTC` to `${ENABLE_EXPERIMENTAL_FEATURES}`, which is defined `OFF` at two independent sites;
the runtime `WebKitSettings:enable-webrtc` also defaults FALSE; Ubuntu's `debian/rules` (2.52.6-1ubuntu1,
298 lines, grepped in full) sets neither flag; and `wry` never calls the setting. Fixing it means shipping
your own WebKitGTK build — at which point the "the engine does WebRTC for you" premise is gone.

## 3. Senders: what every option must write natively anyway

The per-platform work is not a property of the framework — it is a property of the platforms:

**Android, regardless of stack**
- A foreground service of type `mediaProjection`, with `FOREGROUND_SERVICE` +
  `FOREGROUND_SERVICE_MEDIA_PROJECTION`, started **before** `getMediaProjection()`, or Android 14+ throws.
  `flutter_webrtc`'s own manifest is empty and its README never mentions this; LiveKit's Kotlin docs mention
  it only in their Flutter section; Google's own AppRTCMobile sample does not meet the requirement as written.
- A `MediaProjection.Callback` (mandatory on API 34+, `IllegalStateException` otherwise), which is also
  where you handle user revocation and Android 15 QPR1's automatic stop-on-screen-lock.
- Re-consent per session: on API 34+ "each MediaProjection instance must be used only once", so every
  session start and every reconnect needs a fresh consent Intent and a fresh capturer.
- `createScreenCaptureIntent(MediaProjectionConfig.createConfigForDefaultDisplay())` if you want whole-screen
  rather than the user's app-picker default. No SDK does this for you.
- **The USB path needs its own capture pipeline in Kotlin.** libwebrtc gives you `ScreenCapturerAndroid` as a
  `VideoCapturer` feeding a PeerConnection; there is no supported way to tap its encoded output. So the USB
  path is a second `VirtualDisplay` → `MediaCodec` → Annex-B → socket writer, with its own SPS/PPS and
  keyframe-on-connect handling. This is true in every option, Flutter included.

**iOS, regardless of stack**
- A Broadcast Upload Extension target (Swift/ObjC by necessity — an extension is a separate binary), with
  `RPBroadcastProcessMode = RPBroadcastProcessModeSampleBuffer` and an App Group on both targets, because
  "the running app extension and containing app have no direct access to each other's containers".
- Your own extension→app IPC, and a frame format for it. LiveKit's shipped shape is a UNIX socket at
  `<app-group>/rtc_SSFD` with each frame **JPEG-encoded in the extension using a software CIContext** — a
  full re-encode on the latency path that you inherit if you copy it, or replace with IOSurface/shared-memory
  work of your own.
- Back-pressure that drops rather than blocks: ReplayKit "won't invoke the method again for any sample
  buffer type until the current invocation returns", and the buffer "is available only until the method
  returns".
- Out-of-band start/stop signalling between the two processes (LiveKit uses Darwin notifications), because
  the lifecycle callbacks fire in the extension.
- Note `UIBackgroundModes` in an extension's Info.plist causes App Store rejection — `screen-capture` is a
  host-app key, and it belongs to the ScreenCaptureKit path, not the extension path.

**Both, regardless of stack**: the WebSocket signalling client (SDP + ICE), the coturn credential wiring and
relay-only ICE policy, and the 6-digit-code/QR pairing flow. Nothing in any SDK touches pairing.

## 4. Fastest to MVP

**Flutter senders + native GStreamer receiver.** Reasoning, in the order it actually matters:

1. The receiver requirements (H.264 **and** VP8 decode, render, record without re-encode, plus the USB raw
   stream) are met by stock GStreamer elements on both OSes and by nothing else. That leg is settled by §2
   independently of the sender choice.
2. With the receiver native, Flutter's cross-platform claim is scoped to what it is actually good at: one
   Dart codebase for pairing UI, QR, signalling and session state across both senders. `flutter_webrtc` is
   MIT, at 1.6.2+hotfix.1 (published 2026-09-08, seven releases since June — not stale), and genuinely
   implements Android MediaProjection capture (`GetUserMediaImpl.java` calls `createScreenCaptureIntent()`,
   uses `MediaProjectionConfig.createConfigForDefaultDisplay()` on API 34+, drives an
   `OrientationAwareScreenCapturer`) plus the **app side** of the iOS broadcast pipeline
   (`FlutterBroadcastScreenCapturer`, reading `RTCAppGroupIdentifier` and opening the `rtc_SSFD` socket).
   Its `Helper.requestCapturePermission(fullScreenOnly: …)` is exactly the flag a full-screen mirror wants.
3. Its two disqualifying gaps are both on the receiver side — **no desktop recording** (no
   `startRecordToFile` handler in `common/cpp/src/flutter_webrtc.cc`, matching the blank MediaRecorder cells
   in its own README) and a per-frame CPU `ConvertToARGB` in the desktop renderer. Choosing GStreamer for
   the receiver makes both irrelevant.
4. Pin `flutter_webrtc >= 1.6.0`: the 1.6.0 changelog records replacing the private
   `RPSystemBroadcastPickerView buttonPressed:` selector with public UIKit APIs because "App Store review
   rejected binaries containing it".
5. Accept two vendored pieces honestly: the iOS extension's five Swift files come from LiveKit's
   `client-sdk-flutter` example (verified present), and the Android foreground service is either
   `flutter_background` (1.3.1, 2026-03-09, Android-only) or ~40 lines of your own Kotlin. Prefer your own.

**The credible alternative is fully native senders** (Kotlin + Swift on `io.github.webrtc-sdk:android` and
`livekit/webrtc-xcframework` or `stasel/WebRTC`) — pick it if the team is stronger in Kotlin/Swift than in
Dart, since the platform-specific work in §3 exists either way and Flutter's saving is confined to the UI
and signalling layer.

**LiveKit as a whole-stack shortcut was seriously considered and rejected for the MVP**, on scope rather
than quality. It is the only platform with first-party SDKs on all four legs, including a native C++ client
(v1.10.2, 2026-09-08, Linux x64/arm64 + Windows x64, no beta warning) and a Rust SDK with an in-tree
`wgpu_room` example. Its server is Apache-2.0 and self-hostable, it supports H.264, and — contrary to an
earlier gap in our notes — `config-sample.yaml` documents `rtc.turn_servers` for pointing at an external
TURN server, while relay-only ICE is available client-side. But adopting it inserts an SFU into a 1:1
topology, replaces #2's minimal signalling with a proprietary protobuf-over-WebSocket protocol with no
WHIP/WHEP interop, still leaves the renderer, receiver-side recording and the entire USB path as your code,
and lists no Windows hardware-codec path. It is the right thing to revisit if this ever grows past 1:1.

Ruled out quickly, each on its own docs: **mediasoup** (no Android or iOS SDK at all; `libmediasoupclient`
wants you to build libwebrtc m140 yourself; "mediasoup does not provide any signaling protocol"), **Janus**
(GPLv3, JavaScript client only), **Cloudflare Realtime** (managed-only, fails the self-hosting constraint),
**str0m** (no TURN client and no adaptive jitter buffer — the two things a relay-forced 200-500 ms design
needs most), **webrtc-rs** and **Pion** (no decoders, no render path, no declared OS support), **rust-av**
(pre-1.0, `av-codec` last released 2024-11-18, no H.264 decoder claimed).

## 5. The smallest spikes that would settle what documents cannot

1. **Codec reality check, 1 hour**: `gst-inspect-1.0 webrtcbin avdec_h264 vp8dec` on a fresh Windows install
   and a fresh Ubuntu install. Confirms the installer/apt plugin sets that no first-party page enumerates.
2. **Android codec probe, 1 hour**: log `HardwareVideoEncoderFactory.getSupportedCodecs()` on a MediaTek
   device. §1 predicts H.264 will be absent; if so, the VP8 fallback is load-bearing, not theoretical.
3. **Glass-to-glass measurement**: no primary source anywhere in this research states an end-to-end latency
   figure for any of these stacks. The 200-500 ms budget can only be validated by #13.
4. **iOS extension survival**: whether a Broadcast Upload Extension can hold the socket and stay under the
   (undocumented) memory ceiling for a long session, and whether the JPEG-per-frame IPC shape is acceptable
   or must be replaced.

## 6. Reliability of this document

Six option reports, then two independent adversarial fact-checkers over the decision-critical claims (codec
and engine capability; platform support, versions and licences). Nothing central was found factually wrong.
Corrections applied: the "adb has no UDP"-style inference pattern recurred and is labelled as inference
where it appears; Ubuntu's WebRTC-off conclusion is inferred (upstream default OFF + no override) rather
than stated by Ubuntu; the `wgpu_room` example is real in the repo tree but is not mentioned in the README
that was originally cited; Flutter's CI-tested set is narrower than its supported set (CI covers Windows 10
x64 and Ubuntu 22.04 only); `str0m` does declare target platforms (the original report said it did not);
and the desktop-libwebrtc H.264 question, originally UNSOURCED, was resolved from the live CI scripts.
Still open by their own admission: whether Ubuntu's exact 2.52.6 WebKit tarball matches upstream `main`'s
default, Electron's precise publish timestamp, and whether GN would even accept an `rtc_use_h264=true`
override on Android.
