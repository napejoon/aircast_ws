# aircast sender

Flutter screen sender. The UI is the other half of the receiver's: same dark
palette, and the six digits the desktop shows in large type are typed back in
large type here. A dot in the header stays green while the screen is actually
being mirrored, so "am I still sharing?" is answerable at a glance. Two paths, per `docs/research/`:

- **Network** — WebRTC, ICE forced through our own coturn, H.264 preferred with
  VP8 as the mandatory fallback. `lib/signaling.dart`, `lib/session.dart`.
- **USB (Android only)** — `VirtualDisplay → MediaCodec → Annex-B H.264 → local
  socket`. `android/.../UsbCastService.kt`, reached from Dart through
  `lib/usb.dart`.

## iOS

Whole-screen capture on iOS needs a Broadcast Upload Extension added by hand
after the scaffold exists — `ios/README.md` has the five steps. The Dart side
already asks for it.

## Build

The generated Flutter scaffold is not committed. Once, in this directory:

```bash
flutter create --platforms=android,ios --org io.aircast --project-name aircast_sender .
```

It will not overwrite the files that are here. Then:

```bash
flutter pub get
flutter test
flutter run --dart-define=AIRCAST_SIGNAL=wss://signal.example.com/ws
```

In `android/app/build.gradle.kts`, after the scaffold is generated, set
`minSdk = 23` (`flutter_webrtc`'s floor) and `compileSdk = 35` (androidx.fragment
refuses to be compiled against anything below 34). CI does both with `sed`.

The manifest names `io.aircast.sender.MainActivity` and `.UsbCastService` in
full, because the Gradle namespace `flutter create` derives from the project
name is not the package these Kotlin files declare.

## Receiving the USB path

```bash
adb forward tcp:27183 localabstract:aircast   # desktop port → phone socket
gst-launch-1.0 tcpclientsrc host=127.0.0.1 port=27183 ! h264parse ! avdec_h264 ! autovideosink
```

Consent, the `mediaProjection` foreground service and the socket all live in
`UsbCastService`; the Dart side only asks it to start.
