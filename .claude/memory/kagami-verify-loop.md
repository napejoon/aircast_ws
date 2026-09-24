---
name: kagami-verify-loop
description: "How to build, install and see Kagami on this machine — the traps that cost time in the Windows/adb verify loop"
metadata:
  node_type: memory
  type: project
  originSessionId: 59247566-5c97-4867-850c-23b2c5c46583
  modified: 2026-09-24T10:20:41.226Z
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
- **CI debug APKs change signature every build** (fresh runner, fresh `~/.android/debug.keystore`),
  so `adb install -r` fails `INSTALL_FAILED_UPDATE_INCOMPATIBLE` — uninstall first. The committed
  keystore meant to fix this is in the repo but does not work yet; see HANDOFF.md.
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
