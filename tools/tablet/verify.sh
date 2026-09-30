#!/usr/bin/env bash
# A receiver build against the real tablet: recast over a phone that died,
# record, and a fullscreen round trip from maximized. Needs the tablet on USB
# with adb authorised, and Git Bash on Windows.
#
#   tools/tablet/verify.sh                         the installed receiver
#   tools/tablet/verify.sh path/to/kagami.exe      a build extracted with msiexec /a
#
# An extracted build has no plugin registry until it has run once, and that
# first run is slow enough to look like a hang: `kagami.exe --prebuild-registry`
# first. Output and screenshots go to $OUT (default: a temp folder, printed).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="${1:-/c/Program Files/Kagami/bin/kagami.exe}"
OUT="${OUT:-$(mktemp -d)}"
ADB="${ADB:-$(ls -d "$(cygpath -u "$LOCALAPPDATA")"/Microsoft/WinGet/Packages/Google.PlatformTools_*/platform-tools 2>/dev/null | head -1)/adb.exe}"
export ADB
VID="$USERPROFILE/Videos"
LOG="$OUT/receiver.log"
run_ps() { powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/$1")" "${@:2}"; }
up() { grep -ac "jitter buffer" "$LOG"; }
wait_up() { for _ in $(seq 1 30); do [ "$(up)" -ge "$1" ] && return 0; sleep 1; done; return 1; }
echo "output in $OUT"

taskkill //IM kagami.exe //F >/dev/null 2>&1; sleep 1
# env -u HOME: Git Bash hands over HOME=/c/Users/..., which GLib believes.
(env -u HOME GST_DEBUG=webrtcbin:4 GST_DEBUG_NO_COLOR=1 "$EXE" --code 424242 > "$LOG" 2>&1 &)
sleep 8

echo "== 1: first cast"
"$HERE/cast.sh"; wait_up 1 && echo "up" || { echo "FIRST CAST DID NOT COME UP"; exit 1; }
sleep 5

echo "== 2: kill the phone without a bye, cast again"
"$HERE/cast.sh"; wait_up 2 && echo "recast up" || echo "RECAST DID NOT COME UP"
echo "   holding 30 s"; sleep 30
grep -aq "to failed" "$LOG" && echo "   FAILED during hold" || echo "   held"

echo "== 3: record 8 s, with the tablet's screen moving"
# A still screen sends almost no frames -- MediaProjection emits on change --
# so a recording of one can hold a single keyframe. The shade gives it motion.
before=$(ls -t "$VID"/kagami-*.mkv 2>/dev/null | head -1)
run_ps key.ps1 -Vk 0x52 -Scan 0x13
for _ in 1 2 3 4; do
  "$ADB" shell cmd statusbar expand-notifications; sleep 1
  "$ADB" shell cmd statusbar collapse; sleep 1
done
run_ps key.ps1 -Vk 0x52 -Scan 0x13; sleep 4
after=$(ls -t "$VID"/kagami-*.mkv 2>/dev/null | head -1)
if [ -n "$after" ] && [ "$after" != "$before" ]; then
  ls -l "$after"
  command -v ffprobe >/dev/null && echo "   packets: $(ffprobe -v quiet -show_packets -of csv=p=0 "$after" | wc -l)"
else
  echo "   NO NEW RECORDING"
fi

echo "== 4: fullscreen round trip from maximized"
powershell -NoProfile -Command "Add-Type -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool ShowWindow(IntPtr h,int n);' -Name Max -Namespace KagamiMax | Out-Null; \$h=(Get-Process kagami|Where-Object{\$_.MainWindowHandle -ne 0}|Select-Object -First 1).MainWindowHandle; [KagamiMax.Max]::ShowWindow(\$h,3)|Out-Null"
sleep 2
run_ps key.ps1 >/dev/null; sleep 3
run_ps key.ps1 >/dev/null; sleep 3
(cd "$OUT" && run_ps cap.ps1 -Out fullscreen-exit.png) | grep -E "window|maximized|below"
grep -a "left fullscreen" "$LOG" | sed 's/^.*Message: [0-9:.]*: //'
