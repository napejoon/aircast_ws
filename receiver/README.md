# aircast receiver

A GTK4 window around a GStreamer `webrtcbin`. Dark room, the pairing code until
a phone connects, then the mirrored screen in a device bezel with a toolbar that
appears on mouse movement.

```
┌──────────────────────────────────────────────┐   ┌──────────────────────────────┐
│                                              │   │        ╭──────────────╮      │
│                  AIRCAST                     │   │        │              │      │
│   On your phone, enter this code              │ → │        │   mirrored   │      │
│                                              │   │        │    screen    │      │
│            4 2 8 1 7 3                       │   │        ╰──────────────╯      │
│         Waiting for a phone                  │   │      ( ● 00:12  ⛶  ✕ )       │
└──────────────────────────────────────────────┘   └──────────────────────────────┘
```

| Control | What it does |
|---|---|
| `●` / `R` | Start or stop recording, mid-session, without re-encoding |
| `⛶` / `F` / `F11` | Fullscreen; `Esc` leaves it |
| `✕` | Disconnect and quit |

## Dependencies

GTK **4.12 or newer** (`gtk_css_provider_load_from_string`), which Ubuntu 24.04
has and 22.04 does not.

Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libgtk-4-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav \
  libjson-glib-dev libsoup-3.0-dev libsodium-dev
```

**`gtk4paintablesink` is the one plugin apt does not ship.** It lives in
`gst-plugins-rs`, which Ubuntu has no package for (the same gap that ruled
`webrtcsrc` out in `docs/research/stack-options.md` §2 — the difference is that
this one is a sink we can build on its own):

```bash
cargo install cargo-c
git clone --depth 1 https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs
cd gst-plugins-rs/video/gtk4
cargo cbuild -p gst-plugin-gtk4 --release
cargo cinstall -p gst-plugin-gtk4 --release --prefix=/usr/local
GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 gst-inspect-1.0 gtk4paintablesink
```

If the plugin is missing at runtime the window says so instead of showing a
black rectangle.

CI builds it the same way (`.github/workflows/ci.yml`, job `paintablesink`) and
checks that `gst-inspect-1.0` finds it afterwards, so these commands are tested
rather than remembered. The tag is pinned there for the same reason it should be
here: 0.13 is the series built against the GStreamer 1.24 that Ubuntu 24.04
ships.

Windows: the first-party GStreamer MSVC **development** installer, GTK4 from
`gvsbuild` or MSYS2, and `gtk4paintablesink` built the same way as above.
Configure with `PKG_CONFIG_PATH` pointing at both `lib/pkgconfig` directories.

## Windows

The Windows build is an MSYS2 UCRT64 build bundled into a self-contained tree
and packaged as an MSI — `tools/bundle-windows.sh`, `installer/aircast.wxs`, and
the `windows` job in `.github/workflows/ci.yml`, which is where those are
actually exercised. `gtk4paintablesink` needs no cargo build there:
`mingw-w64-ucrt-x86_64-gst-plugins-rs` ships it.

The installer is **per-machine**, into `%ProgramFiles%`. That costs a UAC prompt
and buys the only thing that matters here: the directory the program loads its
plugins from is not writable by the user.

Releases are **not code-signed**, so Windows shows "Windows protected your PC".
Nothing about the format avoids that; only a certificate would.

## Updates

The app checks for a new version and, if there is one, offers a button that
opens the release page in your browser. **It never downloads and never runs
anything.** The manifest it checks is signed with a key that lives offline and
never enters CI, so a compromise of this repository cannot make the program
point users anywhere.

The public key, for cross-checking against `AIRCAST_UPDATE_PK` in
`receiver/update_check.c`:

```
(not generated yet — the check is disabled and the app says so)
```

`docs/threat-model.md` has the full model, including the parts this does not
protect. `--selftest` runs the version and signature tests;
`--verify-manifest FILE --verify-signature FILE` checks a manifest against this
build's key, which is the last step of the release ceremony.

## Build and run

```bash
cmake -B build && cmake --build build
./build/aircast-receiver --signal wss://signal.example.com/ws
```

| Flag | Meaning |
|---|---|
| `--signal` | signalling server URL (required) |
| `--code` | pairing code; generated and displayed when omitted |
| `--record-dir` | where the record button writes `aircast-<timestamp>.mkv` (default: home) |
| `--latency` | jitter buffer in ms, default 200; first knob against the 200-500 ms budget |
| `--insecure` | allow a plaintext `ws://` signalling URL; `wss://` is required otherwise |
| `--no-relay` | allow direct ICE, for LAN bring-up only — both peers then learn each other's address |

Test it without a phone:

```bash
python ../tools/fake_sender.py --signal ws://127.0.0.1:8443 --code <the code shown>
```

## USB path

No signalling, no TURN, no window of ours — the phone's socket is a plain H.264
elementary stream:

```bash
adb forward tcp:27183 localabstract:aircast
gst-launch-1.0 tcpclientsrc host=127.0.0.1 port=27183 ! h264parse ! avdec_h264 ! autovideosink
```
