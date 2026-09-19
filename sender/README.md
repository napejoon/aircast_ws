# Kagami sender

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

The generated Flutter scaffold is not committed. Once, from the repo root:

```bash
bash tools/scaffold-sender.sh
```

It runs `flutter create`, which will not overwrite the files that are here, and
then patches `android/app/build.gradle.kts`: SDK levels, and the removal of the
scaffold's debug signing config. CI runs the same script, so what you build
locally is what gets released. Then:

```bash
cd sender
flutter pub get
flutter test
flutter run --dart-define=AIRCAST_SIGNAL=wss://signal.example.com/ws
```

## An APK for a bring-up

Both of the things a bring-up needs to change are compile-time constants —
`String.fromEnvironment` is resolved by the Dart compiler — so pointing the app
at a different relay means building another APK. The Actions tab has a
**Sender APK (bring-up)** workflow that takes them as inputs:

- `AIRCAST_SIGNAL` — where the signalling server is. The committed default is
  `wss://aircast.cloud/ws`, the deployed relay.
- `AIRCAST_RELAY` — `true` forces every packet through the TURN relay. The
  default is `false`, which lets ICE take the direct pair when the two devices
  can reach each other and fall back to the relay when they cannot. Forcing it
  costs a round trip through another country and conceals this device's address
  from the receiver, but not the receiver's from this one: libnice has no
  sanitiser for the related-address field. The receiver needs `--relay-only` to
  match, or it will offer host candidates this side will not pair with.

It produces a debug APK, because a release build is unsigned by design and
Android will not install it.

The manifest names `io.kagami.sender.MainActivity` and `.UsbCastService` in
full, because the Gradle namespace `flutter create` derives from the project
name is not the package these Kotlin files declare.

## Receiving the USB path

```bash
adb forward tcp:27183 localabstract:aircast   # desktop port → phone socket
gst-launch-1.0 tcpclientsrc host=127.0.0.1 port=27183 ! h264parse ! avdec_h264 ! autovideosink
```

Consent, the `mediaProjection` foreground service and the socket all live in
`UsbCastService`; the Dart side only asks it to start.
