# aircast

Mirror a phone screen to a desktop, over the internet or over a USB cable, with
no third-party media service in the path.

```
 Android / iOS                    Singapore VPS                     Windows / Ubuntu
┌──────────────┐   WebSocket   ┌──────────────────┐   WebSocket   ┌──────────────────┐
│   sender/    │──────────────▶│     server/      │◀──────────────│    receiver/     │
│   Flutter    │  SDP + ICE    │  aircast-signal  │  SDP + ICE    │ GStreamer webrtc │
│              │               │  + coturn (TURN) │               │                  │
│              │═══════════════════ H.264 / VP8, relayed ════════▶│  render + record │
└──────────────┘               └──────────────────┘               └──────────────────┘
        ║
        ╚══ USB: adb forward ! raw H.264 ! ═══════════════════════▶  gst-launch
```

| Directory | What it is |
|---|---|
| `sender/` | Flutter app. WebRTC path in Dart; the Android USB path is Kotlin. |
| `server/` | Signalling: pairing codes, offer buffering, TURN credential minting. |
| `receiver/` | GStreamer `webrtcbin` app. Renders, and records without re-encoding. |
| `ops/` | systemd unit, coturn and nginx templates. |
| `docs/research/` | Why each of those is what it is. Primary sources only. |
| `docs/protocol/` | The signalling wire protocol. |
| `tools/` | A fake sender, so the receiver can be tested without a phone. |

## The decisions that shape everything else

- **WebRTC, relayed through our own coturn.** Only WebRTC ships congestion
  control, and forcing `relay` means neither peer learns the other's address.
- **H.264 preferred, VP8 mandatory.** libwebrtc's Android AAR is built without
  software H.264, so a MediaTek or Unisoc phone has no H.264 encoder at all —
  VP8 is what keeps it from sending a black screen.
- **The USB path is a separate, dumber pipeline.** `adb` forwards TCP only, and
  over a cable there is nothing for congestion control to adapt to, so it is a
  raw H.264 stream on a socket.
- **Recording happens at the receiver, off a `tee`**, so what lands on disk is
  the sender's own bitstream.

## Quickstart

```bash
# 1. server (see server/README.md for the real deployment)
pip install -r server/requirements.txt
AIRCAST_TURN_SECRET=dev AIRCAST_TURN_URLS=turn:127.0.0.1:3478 python server/aircast_signal.py

# 2. receiver — prints the pairing code to type into the phone
cmake -S receiver -B receiver/build && cmake --build receiver/build
./receiver/build/aircast-receiver --signal ws://127.0.0.1:8443/ws --record cast.mkv

# 3. sender
cd sender && flutter create --platforms=android,ios --org io.aircast --project-name aircast_sender .
flutter run --dart-define=AIRCAST_SIGNAL=ws://<host>:8443/ws
```

## LAN bring-up

Relay-only ICE is the product's privacy promise and the default on both sides.
Getting a phone and a desktop talking for the first time is easier without a
TURN server in the middle, so both ends take an explicit opt-out — and both say
so when it is on:

```bash
# the signalling server has to be reachable from the phone, not just loopback
AIRCAST_BIND=0.0.0.0 AIRCAST_TURN_SECRET=dev   AIRCAST_TURN_URLS=turn:127.0.0.1:3478 python server/aircast_signal.py

./receiver/build/aircast-receiver --signal ws://<desktop-lan-ip>:8443 --no-relay

flutter run --dart-define=AIRCAST_SIGNAL=ws://<desktop-lan-ip>:8443             --dart-define=AIRCAST_RELAY=false
```

With the flags off — which is what ships — the two peers only ever see the
relay's address.

## Testing without a phone

`tools/fake_sender.py` is a real WebRTC peer speaking the same protocol, so the
receiver can be exercised end to end with nothing but Python:

```bash
pip install -r tools/requirements.txt
python tools/fake_sender.py --signal ws://127.0.0.1:8443 --code 123456
```

`pytest tools server --asyncio-mode=auto` runs the whole suite, including one
test where two real peers pair through the real server and decoded video is
asserted to move.

## State

`server/` runs and is tested (11 tests, including the end-to-end one).
`server/` and `tools/` run locally. `sender/` and `receiver/` have never been
compiled on a developer machine here — neither toolchain was available — so
`.github/workflows/ci.yml` is where they meet a compiler: the receiver is built
with `-Wall -Werror` against GTK4 and GStreamer, and the sender is analysed,
tested and built into a debug APK after CI restores the scaffold that is not
committed.

iOS needs one manual Xcode step (`sender/ios/README.md`): a Broadcast Upload
Extension, which is what ReplayKit requires to see anything outside our own
window. It is the part of this project with a known expiry date — Apple has
deprecated every ReplayKit capture entry point as of iOS 27.
