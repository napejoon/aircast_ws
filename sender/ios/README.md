# iOS capture

Android's screen capture is one API call behind `getDisplayMedia`. iOS is not:
capturing anything outside our own window needs a **Broadcast Upload
Extension**, a second binary the user starts from Control Center, which talks to
the app over an App Group socket. That boundary is the whole of the iOS work.

`lib/session.dart` already asks for it — on iOS it passes
`'video': {'deviceId': 'broadcast'}`. Without the extension present that request
silently falls back to in-app capture, which mirrors the aircast UI and nothing
else, so the steps below are not optional on iOS.

## Setup, once, in Xcode

The scaffold (`flutter create`) does not generate an extension target; it has to
be added by hand after the scaffold exists.

1. **File → New → Target → Broadcast Upload Extension** (the variant *without*
   UI). Name it `AircastBroadcast`. Deployment target **iOS 14 or newer**, for
   both this target and `Runner`.
2. **App Group**, added under *Signing & Capabilities* to **both** `Runner` and
   `AircastBroadcast`, with the identical identifier:

   ```
   group.io.kagami.sender
   ```

3. **`Runner/Info.plist`** gets the same string:

   ```xml
   <key>RTCAppGroupIdentifier</key>
   <string>group.io.kagami.sender</string>
   ```

4. **Copy five files into the extension target**, from LiveKit's Flutter example
   (`client-sdk-flutter/example/ios/LiveKit Broadcast Extension`), overwriting
   the `SampleHandler.swift` Xcode generated:

   ```
   SampleHandler.swift  SampleUploader.swift  SocketConnection.swift
   DarwinNotificationCenter.swift  Atomic.swift
   ```

   They are deliberately **not vendored into this repo**: they are the upstream
   plumbing flutter_webrtc's iOS path expects, and a stale copy here would fail
   in ways that look like our bug. Pin the SDK version you copied them from in
   the commit message.

5. In the copied `SampleHandler.swift`, set `appGroupIdentifier` to
   `group.io.kagami.sender`.

Then: run the app, start mirroring, and pick **aircast** from the screen-record
long-press in Control Center.

## Why this shape, and what is likely to change

`docs/research/transport-webrtc-vs-custom.md` recorded that **every ReplayKit
capture entry point is deprecated as of iOS 27**, with Apple's own note that
"ScreenCaptureKit replaces ReplayKit for screen streaming and mirroring. A
broadcast extension is no longer necessary." ScreenCaptureKit on iOS was still
beta when that was written.

So this extension is the path that works now, not the path that lasts. When
ScreenCaptureKit on iOS ships properly, the whole extension target — App Group,
socket, five copied files — collapses into in-app capture with `screen-capture`
in the **host app's** `UIBackgroundModes`. Note the trap recorded in
`docs/research/stack-options.md` §3: that key in an *extension's* Info.plist is
an App Store rejection. It belongs to the ScreenCaptureKit path, in the host
app, and nowhere else.
