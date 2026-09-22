# Kagami

Japanese for mirror: **kah-GAH-mee** (鏡). The window is a moon over deep
water, which is what a mirror in the dark gives back.

[![CI](https://github.com/napejoon/aircast_ws/actions/workflows/ci.yml/badge.svg)](https://github.com/napejoon/aircast_ws/actions/workflows/ci.yml)

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
| `docs/threat-model.md` | What the update channel defends against, and what it does not. |
| `installer/` | The Windows MSI definition. |
| `tools/` | A fake sender, so the receiver can be tested without a phone. |

## The decisions that shape everything else

- **WebRTC, with our own coturn behind it.** Only WebRTC ships congestion
  control. ICE takes the direct path when the two devices can reach each other
  and falls back to the relay when they cannot, which is what makes it usable
  on a network that blocks everything but 443.
- **H.264 preferred, VP8 mandatory.** libwebrtc's Android AAR is built without
  software H.264, so a MediaTek or Unisoc phone has no H.264 encoder at all —
  VP8 is what keeps it from sending a black screen.
- **The USB path is a separate, dumber pipeline.** `adb` forwards TCP only, and
  over a cable there is nothing for congestion control to adapt to, so it is a
  raw H.264 stream on a socket.
- **Recording happens at the receiver, off a `tee`**, so what lands on disk is
  the sender's own bitstream.
- **The update check never downloads or runs anything.** It verifies a manifest
  signed by an offline key and then hands a URL to the browser, where Mark of
  the Web and SmartScreen still apply. `docs/threat-model.md` says why, and what
  that leaves exposed.

## Quickstart

```bash
# 1. server (see server/README.md for the real deployment)
pip install -r server/requirements.txt
AIRCAST_TURN_SECRET=dev AIRCAST_TURN_URLS=turn:127.0.0.1:3478 python server/aircast_signal.py

# 2. receiver — prints the pairing code to type into the phone
cmake -S receiver -B receiver/build && cmake --build receiver/build
./receiver/build/kagami --signal ws://127.0.0.1:8443/ws --record cast.mkv

# 3. sender
bash tools/scaffold-sender.sh && cd sender
flutter run --dart-define=AIRCAST_SIGNAL=ws://<host>:8443/ws
```

## LAN bring-up

ICE tries a direct path first on both sides, so a phone and a desktop on one
Wi-Fi talk to each other and never touch a TURN server. That is also the
fastest the product gets: a relay in another country is about 47 ms of one-way
path, and the receiver's 200 ms jitter buffer is sized for a retransmission
crossing it twice.

The two peers do see each other's addresses when they take that path. Passing
`--relay-only` to the receiver and `AIRCAST_RELAY=true` to the sender forces
everything through the relay instead; both ends have to agree, or the one that
still offers host candidates simply finds nothing to pair with.

```bash
# the signalling server has to be reachable from the phone, not just loopback
AIRCAST_BIND=0.0.0.0 AIRCAST_TURN_SECRET=dev AIRCAST_TURN_URLS=turn:127.0.0.1:3478 python server/aircast_signal.py

./receiver/build/kagami --signal ws://<desktop-lan-ip>:8443 --insecure

flutter run --dart-define=AIRCAST_SIGNAL=ws://<desktop-lan-ip>:8443
```

A relayed session is not anonymous either, for the record: libnice writes this
machine's own address into the related-address field of every relayed candidate
it offers, and has no sanitiser for it. libwebrtc on the phone does scrub its
half. Relay-only buys a slower path and one direction of concealment, not two.

## Testing without a phone

`tools/fake_sender.py` is a real WebRTC peer speaking the same protocol, so the
receiver can be exercised end to end with nothing but Python:

```bash
pip install -r tools/requirements.txt
python tools/fake_sender.py --signal ws://127.0.0.1:8443 --code 123456
```

`./run-tests.sh` runs everything CI runs, in CI's order and with CI's flags: the
Python suite, then the receiver built `-Wall -Werror` and its `--selftest`, then
`flutter test`. A toolchain this machine lacks is a `SKIP` line on stderr, not a
failure, so the run always says what it did not check.

`pytest` alone is the Python half -- `pytest.ini` carries the asyncio mode and the
test roots -- and it includes the one test where two real peers pair through the
real server and decoded video is asserted to move.

## State

Every step of the bring-up order below has been walked except the last two, and
the git log is the record of it: a tablet at 2304x1440 mirroring to a notebook
at 1920x1080, Netflix against the 6 Mbit ceiling, a relay on a university
network that drops everything but 443, and a Windows crash dump reading
c0000374. What is left is the Ubuntu receiver and iOS.

| | Built | Run |
|---|---|---|
| `server/`, `tools/` | — | yes, locally and in CI (16 tests) |
| `receiver/` Windows | CI, `-Wall -Werror`, MSI | yes, on real casts |
| `receiver/` Ubuntu | CI, `-Wall -Werror` | **never** |
| `gtk4paintablesink` | CI, from pinned sources | yes, on Windows |
| `sender/` Android | CI, debug APK; unsigned release APK on a tag | yes, on a tablet |
| `sender/` iOS | not built anywhere | **never** |
| USB path | compiles inside the APK | yes, including a second cast in one run |
| TURN relay | — | yes, on the deployed VPS |

The end-to-end test proves the protocol, not the product: both peers in it are
aiortc, so no GStreamer and no libwebrtc were in that loop. Everything above it
in this table was proven the other way, by casting and reading the log.

## Bring-up order

Each step exists to fail on its own, rather than three at once. Steps 1 to 6
have been done on Windows; what each one cost is in the commits it produced.

1. ~~**Build the receiver on a real Linux box**, `gtk4paintablesink` included.~~
   Done on Windows instead, through MSYS2 UCRT64 and the MSI — `receiver/README.md`
   §Windows. A real Linux box is still the one platform this has never run on,
   so step 1 is the outstanding one, not a finished one.
2. ~~**`tools/fake_sender.py` into that receiver.**~~ Done.
3. ~~**A real Android phone on the same LAN.**~~ Done, on a 2304x1440 tablet.
   MediaProjection consent, the foreground service, flutter_webrtc at runtime and
   libwebrtc talking to `webrtcbin` all landed here, and so did the aspect and
   orientation work that followed from a screen whose shape is not the notebook's.
4. ~~**The record button**, mid-session.~~ Done, and the 700 ms guess at how long
   the muxer needs is gone: the branch comes out when its own EOS arrives,
   forwarded out of the bin by `message-forward`. The timer that remains is a
   five-second deadline rather than the plan.
5. ~~**The USB path.** `adb forward`, then `gst-launch`.~~ Done, including the
   second cast in one run, which is where the parked `accept()` thread and the
   once-only SPS/PPS turned up. Rotation is followed now, by resizing the one
   VirtualDisplay rather than asking the projection for a second one.
6. ~~**Then buy the VPS**~~ Done — `docs/ops/provision-vps.md` describes the
   server that exists. TURNS shares 443 with the websites on that box, which is
   the only reason the tablet works on a university network.
7. **iOS**, still last and still untouched: it needs a Mac, an Apple Developer
   account and a manual Xcode step (`sender/ios/README.md`) — and Apple has
   deprecated every ReplayKit capture entry point as of iOS 27, so it is the part
   with a known expiry date.
