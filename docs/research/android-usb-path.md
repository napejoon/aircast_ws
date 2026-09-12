# Android USB cast path: adb-based vs AOA vs USB tethering

Research asset for [#4](https://github.com/napejoon/aircast_ws/issues/4) (map [#1](https://github.com/napejoon/aircast_ws/issues/1)).
Date: 2026-09-10. Method: primary sources only — Android's own docs, AOSP source and licence files, libusb's own
wiki, and each project's own repo. The decision-critical claims were re-checked by an independent
fact-checker that fetched every page; several were corrected in the process and are marked below.

Carried in from [#2](https://github.com/napejoon/aircast_ws/issues/2) and
[#3](https://github.com/napejoon/aircast_ws/issues/3): the USB path is Android-only, carries a raw H.264
elementary stream over a plain TCP socket, and the receiver will be a GStreamer-based native app that can
consume it with `tcpclientsrc ! h264parse ! <decoder> ! <sink>` — the same decoder, renderer and recording
branch as the WebRTC path.

## 1. Why there are only two real candidates

`adb forward` / `adb reverse` carry **TCP only** — adb's `socket_spec.cpp` recognises `tcp:`, `vsock:`,
`acceptfd:` and the `local*` family, all created `SOCK_STREAM`, with no UDP scheme anywhere. (This is a
code-level fact; no Android page states the restriction in prose.) That is exactly why #2 chose a plain TCP
stream for USB rather than trying to push the WebRTC/ICE path down the cable.

**USB tethering is not a candidate at all**: `TETHERING_USB` is `@hide @SystemApi` and `startTethering`
requires `TETHER_PRIVILEGED`, so no normal app can turn it on. The user would have to enable it by hand
every session. It survives only as a *detection* signal — see §5.

## 2. adb path (the scrcpy shape)

**How it works**: `adb reverse localabstract:<name> tcp:<port>` makes the device listen and the desktop
connect; scrcpy uses exactly this by default, falling back to `adb forward` when `--force-adb forward` is set.

**What the user must do, and it is not small**:

- Enable **USB debugging**, which lives under Developer options — hidden by default since Android 4.2.
- Accept the **RSA host-key dialog**, which per Android's own wording means adb commands "cannot be executed
  unless you're able to unlock the device and acknowledge the dialog". ("Always allow from this computer"
  makes it a one-time cost per desktop.)
- On Windows, install an **OEM USB driver**.

For a consumer-facing product that is three pre-flight steps before any pixel moves. For an internal tool
used by a small team it is a one-time setup per machine, which is a different judgement entirely.

**Can we ship adb inside our desktop app?** This is the question the ticket actually turns on, and the answer
is a two-document answer:

- The **prebuilt binary** from Platform-Tools is governed by the Android SDK licence, which grants a
  "limited, worldwide, royalty-free, non-assignable, non-exclusive, and **non-sublicensable** license to use
  the SDK solely to develop applications for compatible implementations of Android" (§3.1) and states
  "you may not copy (except for backup purposes), modify, adapt, **redistribute**, decompile, reverse
  engineer, disassemble, or create derivative works of the SDK or any part of the SDK" (§3.4).
- But §3.5 carves out open-source components: "Use, reproduction and distribution of components of the SDK
  licensed under an open source software license are governed solely by the terms of that open source
  software license and not the License Agreement."
- And **adb's source is Apache-2.0** — `packages/modules/adb`'s `MODULE_LICENSE_APACHE2` and `NOTICE` carry
  the standard header ("Copyright (c) 2006-2009, The Android Open Source Project / Licensed under the Apache
  License, Version 2.0"), consistent with source.android.com's statement that Apache-2.0 "is the preferred
  license for AOSP, and the majority of Android software is licensed with Apache 2.0". Apache-2.0 §2 grants
  the right to "reproduce, prepare Derivative Works of, publicly display, publicly perform, sublicense, and
  **distribute** the Work … in Source or Object form", subject to attribution and NOTICE preservation.

**So: building adb from AOSP source and shipping that binary is permitted by the licence that governs the
source; redistributing Google's Platform-Tools zip verbatim is not.** No first-party page addresses the
bundling question directly — that is a genuine documentation gap, not a fetch failure, and the conclusion
above is assembled from the two licences rather than stated by anyone at Google.

**The server-collision problem, and its only documented fix**: adb is a client/server design where all
clients talk to one server over **TCP port 5037**; a client auto-starts a server if none is running, and
`ANDROID_ADB_SERVER_PORT` moves the port. Android's docs describe no multi-server negotiation, so a shipped
app running its own adb will fight Android Studio's on the default port. **Set
`ANDROID_ADB_SERVER_PORT` for our own server** — that is the whole mitigation, and it is first-party. Note
also that client/server version mismatches are documented as a failure source, which is a second argument
for shipping a known-version binary rather than shelling out to whatever adb is on `PATH`.

**Correction to a widely repeated claim**: scrcpy is often said to bundle `adb.exe` in its Windows release.
Its FAQ says the release "should work out-of-the-box", but the v4.1 release asset list contains no separate
adb asset and no description confirming it, and `doc/windows.md` mentions adb only under the package-manager
paths ("From WinGet: ADB and other dependencies will be installed alongside scrcpy"; "From Chocolatey /
Scoop: `choco install adb # if you don't have it yet`"). **Treat "scrcpy bundles adb" as unconfirmed.** The
settled fact is the other project: ws-scrcpy's README requires that "`adb` executable must be available in
the PATH environment variable" — explicitly not bundled. Smallest check to close this: `unzip -l` the
official `scrcpy-win64` zip.

## 3. AOA path (Android Open Accessory)

**How it works**: the desktop takes the accessory/host role — enumerate, send control request 51 (Get
Protocol; a nonzero version means AOA is supported), 52 (identifying strings: manufacturer / model /
description / version / URI / serial, each ≤256 bytes, null-terminated), then 53 (Start Accessory Mode) with
no payload. The device re-enumerates under **VID 0x18D1, PID 0x2D00 or 0x2D01**, after which you query the
descriptors and use the first bulk IN/OUT endpoints (issuing SET_CONFIGURATION for the two-interface 0x2D01
case). AOSP itself ships a working host-side implementation at
`frameworks/base/libs/usb/tests/accessorytest/usb.c`, so a Linux desktop can do this today. AOA v2 is still
documented; only its audio output was deprecated, in Android 8.0.

**On the device side it is an ordinary app API, not a debug feature**: `UsbManager` +`UsbAccessory`, with a
manifest `android.hardware.usb.action.USB_ACCESSORY_ATTACHED` intent-filter pointing at an
`res/xml/accessory_filter.xml` that whitelists manufacturer/model/version. **Nothing in Android's accessory
guide mentions Developer options, USB debugging or root** — which is AOA's whole selling point against the
adb path. (Stated honestly: the guide never says "no Developer options required" in so many words; the
conclusion rests on the absence of any such requirement across the documented API surface.)

**Does the permission persist? Yes — and this is the correction that matters.** The guide only describes the
two entry paths: "If your application uses an intent filter to discover accessories as they're connected, it
automatically receives permission if the user allows your application to handle the intent. If not, you must
request permission explicitly… The call to `requestPermission()` displays a dialog to the user asking for
permission to connect to the accessory." Persistence appears in the framework javadoc instead —
`hasPermission(UsbAccessory)` "Returns true if the caller has permission to access the accessory. Permission
might have been granted temporarily via `requestPermission(…)` **or by the user choosing the caller as the
default application for the accessory.**" So the "use by default" binding is an app-default, not a one-shot
grant, and survives reconnects. **Cite the javadoc for this, not the guide page** — and note that neither
source states whether it survives a full device reboot. Smallest check: grant the default, reboot, replug,
see whether the dialog returns.

**The desktop-side cost is where AOA gets expensive**:

- **Windows needs a driver.** Per libusb's own Windows wiki, a device not already HID- or WinUSB-bound needs
  WinUSB (recommended), libusbK or libusb-win32 before libusb can talk to it. Zadig is documented as "an
  Automated Driver Installer GUI application" — i.e. installable from an installer, but not zero-touch.
- **Linux needs a udev rule** for the mode-switched VID/PID (documented IDs; the udev consequence is
  inference, not an Android statement).
- **No first-party throughput figure exists.** Android's AOA pages state only packet/buffer sizes
  (16384-byte buffers; 64-byte full-speed / 512-byte high-speed packets) and no Mbit/s number anywhere. The
  480 Mbit/s figure in circulation is the USB 2.0 *bus* spec, not an AOA or libusb claim about achievable
  application throughput. **UNSOURCED — must be measured.**
- **No maintained desktop AOA media implementation to copy.** Beyond AOSP's bare test tool, none turned up.
  (Flagged as a search-coverage gap rather than proof of absence.)

## 4. Decision table

| | adb (`adb reverse` + TCP) | AOA (accessory mode) | USB tethering / RNDIS |
|---|---|---|---|
| Developer options required | **yes** (USB debugging) | **no**, per the accessory guide's silence on it | n/a |
| Device-side dialog | RSA host-key, once per desktop ("always allow") | accessory permission dialog, or auto-granted via intent-filter | n/a |
| Persists | yes | **yes**, via "default application for the accessory" (javadoc); reboot survival unverified | n/a |
| Windows driver | OEM adb driver | WinUSB/libusbK via Zadig | n/a |
| Linux | udev rules | udev rule for 0x18D1:0x2D00-2D01 (inferred) | n/a |
| Can we ship everything we need | **yes, if we build adb from AOSP source** (Apache-2.0); not by redistributing Platform-Tools | yes — no adb dependency at all | **no** — user must enable it by hand |
| Collides with the user's own tooling | **yes** — one adb server per port 5037; fix is `ANDROID_ADB_SERVER_PORT` | no | no |
| First-party throughput figure | none | none | none |
| Shipped by anyone in the wild | scrcpy, ws-scrcpy (both require/expect adb) | nothing maintained beyond AOSP's test tool | nothing |

## 5. What USB tethering is still good for

Only detection. `NetworkCapabilities` declares, in AOSP source with **no `@hide` or `@SystemApi`
annotation**, `public static final int TRANSPORT_USB = 8;` ("Indicates this network uses a USB transport") —
so a normal app can notice that a USB network link exists. The exact "Added in API level" could not be read
from a first-party page (the reference page is a JS-rendered SPA that returns only a nav shell, and the AOSP
source carries no `@since`); community knowledge says API 31, unverified. Android documents nothing
app-facing about the RNDIS link's addressing.

## 6. Recommendation

**Implement the adb path first; keep AOA as the escape hatch, and let the audience decide whether AOA ever
gets built.**

The reasoning is not that adb is nicer — it is that adb is the only one of the two with a proven shape to
copy (scrcpy's `adb reverse` + TCP socket, which is also exactly the stream shape #2 chose), and the only one
where the desktop side is a socket rather than a USB stack we write and validate ourselves. Its two real
problems both have concrete answers: the redistribution question is solved by building adb from Apache-2.0
AOSP source instead of shipping Google's zip, and the server collision is solved by one environment variable.

What would flip this to AOA: if the product turns out to be consumer-facing rather than internal, the
Developer-options requirement becomes the dominant cost and AOA's persisted accessory permission is worth
building a host stack for. The gate on that decision is a throughput measurement — no primary source
anywhere states what AOA bulk transfer actually sustains, and a 1080p H.264 mirror is a real bitrate.

## 7. Reliability of this document

One research pass plus one adversarial verification pass over the decision-critical claims. Corrections the
verification produced, all folded in above: AOA permission persistence **is** documented (in the framework
javadoc, not the guide page) — the first pass had marked it unsourced; `TRANSPORT_USB` is a normal public
constant, not `@hide`; and "scrcpy bundles adb.exe" does **not** hold up against the release assets and
`doc/windows.md`, so it is presented as unconfirmed rather than as fact. Still open by their own admission:
`TRANSPORT_USB`'s API level from a first-party page, whether the AOA default-app grant survives a reboot,
whether the official scrcpy Windows zip physically contains `adb.exe`, and any throughput figure for AOA.
