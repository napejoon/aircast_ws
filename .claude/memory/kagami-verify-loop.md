---
name: kagami-verify-loop
description: "How to build, install and see Kagami on this machine — the traps that cost time in the Windows/adb verify loop"
metadata:
  node_type: memory
  type: project
  originSessionId: 59247566-5c97-4867-850c-23b2c5c46583
  modified: 2026-09-25T10:50:00.000Z
---

Verifying a Kagami change means: push → CI builds → pull the artifact → install → look at it.
Six bugs in one session were invisible to the compiler and only showed up in a screenshot or
a live cast, so the loop is the work, not overhead.

The traps, each of which cost a cycle:

- **msiexec from Git Bash** needs `MSYS2_ARG_CONV_EXCL='*'` or MSYS rewrites `/i` into a path
  and the installer answers with its usage dialog.
- **Use `/qb`, never `/qn`.** The package is `Scope="perMachine"`, so a silent install cannot
  raise the UAC prompt and dies with `1730 → 1603`. `/qb` shows a progress bar and lets Windows ask.
- **Screenshots without stealing focus:** `PrintWindow(hwnd, hdc, 2)`. `SetForegroundWindow`
  is refused for a process that is not already in front, so AppActivate captures the wrong window.
- **CI debug APKs are signed with the committed `sender/android/debug.keystore`** since PR #62
  (2026-09-25), so `adb install -r` updates in place. Release APKs are unsigned and do not install.
- **Test a receiver build without installing it:** `msiexec /a <msi> /qn TARGETDIR=<dir>` extracts
  it (no UAC), then run `<dir>/PFiles64/Kagami/bin/kagami.exe --prebuild-registry` once — the
  extracted copy has no registry and its first launch is slow enough to look like a hang.
- **Launch the receiver with `env -u HOME`** — Git Bash's `HOME=/c/Users/...` reaches GLib.
- **A sender left casting from the last test reconnects to the next receiver** on the same code,
  and `cast.sh` then force-stops it: that looks like the cast dropping. Force-stop the sender
  between runs.
- **Letter keys:** the keyboard layout here is Thai, so an injected R arrives as พ. Use PostMessage
  WM_KEYDOWN with the scan code in lParam (`kagami-work/key3.ps1`); `keybd_event` +
  SetForegroundWindow is unreliable. F11 via PostMessage always worked.
- **A static phone screen sends almost no frames** (MediaProjection only emits on change), so an
  8 s recording of it can hold a single keyframe. Animate the tablet during a record test:
  `adb shell cmd statusbar expand-notifications` / `collapse`. Check with `ffprobe -show_packets`.
- **adb** is at `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Google.PlatformTools_*\platform-tools\adb.exe`
  (installed with `winget install Google.PlatformTools`); it is not on PATH in this shell.
- **`gh run download` of a 70 MB MSI or a 200 MB APK exceeds the 120 s tool timeout** — run it
  with `run_in_background: true`.
- **Merging waits on the PR-triggered run.** Opening a PR starts a second CI run and
  `mergeStateStatus` stays `BLOCKED` until it finishes. Check the merge result before tagging:
  a chained `merge && tag` once put a tag on the wrong commit.
- `keytool -printcert -jarfile` cannot read an APK signed only with v2/v3, so it cannot be used
  to compare signing certs.

See [[user-thai-terse-reports]] for how to report what the loop found.
