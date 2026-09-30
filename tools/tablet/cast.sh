#!/usr/bin/env bash
# Start a cast from the tablet to a receiver waiting on code 424242.
#
# The taps are screen coordinates on the 2304x1440 tablet this was written
# against (landscape): the code field, the capture-scope dropdown, "Share
# entire screen", then "Share screen". Another device needs its own numbers --
# `adb shell uiautomator dump` and read the bounds.
#
# Force-stops the sender first. A sender left casting from the previous run
# reconnects to the next receiver on the same code by itself, and killing it
# here is then what makes a cast look like it dropped.
ADB="${ADB:-$(ls -d "$(cygpath -u "$LOCALAPPDATA")"/Microsoft/WinGet/Packages/Google.PlatformTools_*/platform-tools 2>/dev/null | head -1)/adb.exe}"
"$ADB" shell am force-stop io.kagami.kagami_sender
"$ADB" shell am start -n io.kagami.kagami_sender/io.kagami.sender.MainActivity >/dev/null 2>&1
sleep 4
"$ADB" shell input tap 1152 625          # the code field
sleep 1
"$ADB" shell input text 424242
sleep 1
"$ADB" shell input keyevent 66           # Enter submits the code
sleep 4
"$ADB" shell input tap 1152 642          # scope dropdown
sleep 2
"$ADB" shell input tap 1030 768          # Share entire screen
sleep 2
"$ADB" shell input tap 1348 1005         # Share screen
