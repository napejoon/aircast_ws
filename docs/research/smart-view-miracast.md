# Smart View with nothing installed on the phone

Not an option survey — that already happened and the answer was no. This records the verdict, the
one button it turned into, and the parts of it that are Windows' problem rather than ours.

Date: 2026-09-13. Method: the receiver notebook's own registry, the Connect app's own
`AppxManifest.xml`, and the Settings app's own URI table, read on the machine the receiver runs on
(Windows 11 Home Single Language, 10.0.26200). Two identifiers below could not be confirmed on this
machine and are marked where they appear; everything else was read, not recalled.

## 1. The verdict, kept short

Samsung Smart View is Miracast. A Miracast sink is Wi-Fi Direct, an RTSP handshake, HDCP and a WLAN
driver that agrees to be a sink — four dependencies with nothing to do with `webrtcbin`, three of
them below the level any GStreamer pipeline can reach. We are not writing one.

Windows already ships one: **Projecting to this PC**, with the Connect app
(`Microsoft.PPIProjection`) behind it. It is an optional feature rather than part of the image; its servicing package is on disk at
`C:\Windows\servicing\Packages\Microsoft-Windows-WirelessDisplay-FOD-Package~31bf3856ad364e35~amd64~~10.0.26100.9444.mum`.

Its capability name and the release it became optional in could not be read on this machine:
`Get-WindowsCapability` refuses without elevation, and so they are not recorded here.

So the shippable answer is a door, not a sink: one line on the receiver's idle card that opens that
page. The phone then casts to Windows, and aircast is not in the path.

## 2. The deep link, and why not GtkUriLauncher

`ms-settings:project` is the page, verified rather than remembered: the URI sits as UTF-16 in
`C:\Windows\ImmersiveControlPanel\SystemSettings.dll`, in the page table, immediately after the
internal page name `SettingsPageContinuum` — Continuum being what this page was called before it was
called Projecting to this PC.

The receiver already opens URLs with `GtkUriLauncher` (the Update button). That will not work here,
and the registry says why:

```
HKCR\ms-settings\Shell\Open\Command
    DelegateExecute = {4ed3a719-cea8-4bd9-910d-e252f997afc2}   ("Association Launch Execute Command")
    (default)       = absent
```

There is no command line. GIO's Win32 `GAppInfo` is built by parsing exactly that default value, so
the URI schemes Windows implements as COM delegates are precisely the ones
`g_app_info_launch_default_for_uri()` cannot open — and it fails by doing nothing, which is the worst
way for this particular button to fail. `ShellExecuteW` goes through the shell, which honours
`DelegateExecute`, and returns an `HINSTANCE` greater than 32 on success. That is the whole
implementation, in `on_wireless_display_clicked()` in `receiver/main.c`.

## 3. Detecting whether the feature is installed, and why we do not

| Probe | Needs admin | Verdict |
|---|---|---|
| `Get-WindowsCapability -Online -Name App.WirelessDisplay.Connect*` | **yes** — "The requested operation requires elevation" | authoritative, and therefore unusable from a normal app |
| `C:\Windows\SystemApps\Microsoft.PPIProjection_cw5n1h2txyewy\Receiver.exe` exists | no | present here (folder created 8 Sep 2026 on an OS installed Dec 2024, i.e. when the feature was added). Never observed on a machine *without* the feature, so the negative case is unproven |
| `HKCR\ms-projection` exists | no | cheapest positive signal — the Connect app registers `ms-projection` and `ms-playto-miracast` in its manifest, and both keys are present here, mirrored into `HKCU\Software\Classes`. Same unproven negative |
| `C:\Windows\servicing\Packages\*WirelessDisplay-FOD*` | no | useless: `.mum` files are present for packages the image merely knows about |
| `netsh wlan show driver` | no | answers a *different* question — whether the hardware could ever do it. Here: `Wireless Display Supported: Yes (Graphics Driver: Yes, Wi-Fi Driver: Yes)` |

**We ship no check.** The Settings page is the only thing on the machine that knows the answer
without elevation, and when the answer is no it already says to add the Wireless Display optional
feature and puts the button under it. A probe of ours could only produce a second opinion that
disagrees with the page we are about to open, and a false negative would hide the button on exactly
the machines that need it.

Two next steps, deliberately not taken:

- `ms-projection:` opens the Connect app directly, skipping Settings — and does nothing at all when
  the feature is absent, which is the one case where the user needs to be told something. The
  Settings page carries its own "launch the Connect app" link, so one door covers both states.
- The AUMID `Microsoft.PPIProjection_cw5n1h2txyewy!Microsoft.PPIProjection` (from the manifest:
  `<Application Id="Microsoft.PPIProjection" Executable="Receiver.exe">`) launches the same app via
  `explorer.exe shell:appsFolder\...`. Same objection, plus a shell-out.

## 4. End to end, as the user meets it

1. Receiver window, idle card: **No app on the phone? Use Windows Wireless Display**.
2. Settings opens on System > Projecting to this PC. If the feature is missing, that page says so and
   offers Optional features; adding it needs an administrator and a working Windows Update.
3. On the same page: "Available everywhere" (or "on secure networks"), and whether a PIN is required.
   Nothing is advertised to anything until this is set.
4. Phone: Quick panel > Smart View > the notebook appears > tap it. Windows prompts on the PC, or
   asks for the PIN.
5. The Connect app takes the screen. The aircast window is not involved and should be left alone.

## 5. What it costs, said before someone finds out the hard way

- **No recording, no bezel, no pairing code.** The picture never enters our pipeline, so the record
  button, `--record-dir` and the whole `tee` branch have nothing to work on.
- **The latency is Windows', not ours.** `--latency` tunes a jitter buffer this path does not
  contain. If it is bad, we cannot make it better, and we should not pretend otherwise.
- **The drivers decide.** `netsh wlan show driver` answers in one line; a No on either half of
  `Wireless Display Supported` ends it, and no software of ours changes that.
- **Wi-Fi must be on even on Ethernet**, because the transport is Wi-Fi Direct rather than the LAN.
  Managed and guest networks that block peer-to-peer traffic block this; Miracast over
  Infrastructure (both on the same network, the PC's hostname resolvable) is Windows' fallback, and
  plenty of networks block that too.
- **Windows 11 Home is fine.** Every fact in this file was read on Windows 11 Home Single Language
  10.0.26200: the optional feature, the settings page, the Connect app and the driver support are all
  present. Home changes nothing except that adding the feature still needs an administrator. No part
  of this design needs Pro, a domain or a policy.
- **Nothing here exists on Ubuntu.** The button is inside `#ifdef G_OS_WIN32`, and the Linux
  equivalent means `gnome-network-displays` and its own dependency chain — the sink we already
  refused to write.
- **The phone half was not tested for this note.** Everything above is the PC side. Whether a Galaxy
  Tab S10 FE's Smart View lists this particular notebook is a bring-up step, not a claim.
