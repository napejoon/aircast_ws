# iOS sender

Android's screen capture is one API call behind `getDisplayMedia`. iOS is not:
capturing anything outside our own window needs a **Broadcast Upload
Extension**, a second binary that ReplayKit starts and feeds, which passes the
frames to the app over a socket in a shared **App Group** container. That
boundary is the whole of the iOS work.

Nobody maintaining this project has a Mac, so none of it is done in Xcode.
CI's macOS runner builds it (`sender (iOS)` in `.github/workflows/ci.yml`):

| Piece | Where |
|---|---|
| Extension target, App Group on both targets, embed phase | `tools/ios/add-broadcast-extension.rb` |
| Scaffold, Info.plist keys, floor, icon, keep-alive | `tools/scaffold-ios.sh` |
| `SampleHandler.swift`, the extension's Info.plist and entitlements | `KagamiBroadcast/` (ours) |
| `SampleUploader`, `SocketConnection`, `DarwinNotificationCenter`, `Atomic` | fetched by the scaffold from LiveKit's `client-sdk-flutter` example at a pinned commit, hash-checked |
| Keeping the app awake while it relays frames | `Kagami/KagamiKeepAlive.m` |
| Ad hoc signing with entitlements, and the bundle checks | `tools/ios/package-ipa.sh` |

The result is the `kagami-ios-sideload` artifact: `Kagami-sideload.ipa`.

## Which iOS

**iOS 15 and later**, which is every iPhone from the 6s and the first SE on
(2015). The floor is Flutter's own — the scaffold prints it (`iOS floor: …`) and
the IPA's `MinimumOSVersion` carries it — and nothing built with this Flutter
runs below it; flutter_webrtc (13) and mobile_scanner (12) would go lower. An
iPhone that cannot update past iOS 12 (the 6 and older) would need an old
Flutter and old plugins for the whole app, Android included.

**Every version from the floor through iOS 27 uses the same path.** Apple
deprecated ReplayKit's capture entry points in iOS 27 in favour of
ScreenCaptureKit (`docs/research/transport-webrtc-vs-custom.md`); deprecated is
not removed, and the extension still runs there. When a future iOS removes it,
the extension, App Group and socket collapse into in-app ScreenCaptureKit
capture with `screen-capture` in the **app's** `UIBackgroundModes` — never the
extension's, which is an App Store rejection (`docs/research/stack-options.md` §3).

## Installing it with a free Apple ID, from Windows

There is no App Store or TestFlight build: both need the $99/year Apple
Developer Program. A free Apple ID can sign an app for your own device.

1. Download `Kagami-sideload.ipa` from the latest CI run's artifacts (Actions →
   CI → a push run on `main` → `kagami-ios-sideload`).
2. Install **Sideloadly** on Windows, with **iTunes and iCloud from Apple's
   website** — not the Microsoft Store versions, which Sideloadly cannot talk to.
3. Plug the iPhone in, trust the computer, open Sideloadly, drop the IPA on it,
   sign in with the Apple ID, Start.
4. On the iPhone: **Settings → General → VPN & Device Management** → trust your
   Apple ID. On iOS 16 and later also **Settings → Privacy & Security →
   Developer Mode** → on, and restart.

Limits of a free Apple ID: the signature **expires after 7 days** (re-run
Sideloadly to renew; nothing in the app is lost), at most 3 sideloaded apps at
once, and 10 new app IDs a week — this one uses two, the app and its extension.

## Using it

Open Kagami, scan the desktop's code or type it, **Start mirroring**. iOS shows
its broadcast picker with **Kagami** selected: **Start Broadcast**. The red
indicator in the status bar is the capture; Kagami keeps itself awake behind
it by playing silence (`Kagami/KagamiKeepAlive.m` says why), mixed so music
keeps playing. Stop from the red indicator or from Kagami.

## Not yet proven on a phone

CI proves the bundle is put together right. These are what only a device can
answer, in the order to check them:

1. The picker lists **Kagami** and starting it reaches the desktop at all —
   the App Group reaching the free-Apple-ID signature is the likeliest failure,
   and shows as a broadcast that runs while the desktop never gets a frame.
2. Mirroring survives switching to another app for more than 30 seconds (the
   keep-alive).
3. A long session stays under the extension's memory ceiling — LiveKit's
   uploader JPEG-encodes every frame in the extension
   (`docs/research/stack-options.md` §5.4).
