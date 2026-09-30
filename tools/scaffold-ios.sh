#!/usr/bin/env bash
# Regenerate the iOS scaffold and add what whole-screen mirroring needs on top
# of it: the Broadcast Upload Extension, the App Group on both targets, the
# keep-alive, the Info.plist keys and the icon. macOS only (PlistBuddy, Xcode,
# the xcodeproj gem) -- CI's macOS runner, since no Mac maintains this project.
#
#   tools/scaffold-ios.sh
#
# Safe to re-run: `flutter create` never overwrites, the extension script skips
# a target that exists, and every other step sets rather than appends.
set -euo pipefail

cd "$(dirname "$0")/../sender"
flutter create --platforms=ios --org io.kagami --project-name kagami_sender .
rm -f test/widget_test.dart
cd ios

# The four upstream files of the extension, from LiveKit's client-sdk-flutter
# example (Apache-2.0) at a pinned commit, each checked against the hash it had
# when this was written. Fetched rather than committed so the pin is the one
# place their version lives (sender/ios/README.md); checked so a moved tag or a
# rewritten file fails here instead of shipping.
LIVEKIT=0e121fce1b7bcd99cd712ad4dcdbe3ecb8d3727c
while read -r sum file; do
  curl -fsSL "https://raw.githubusercontent.com/livekit/client-sdk-flutter/$LIVEKIT/example/ios/LiveKit%20Broadcast%20Extension/$file" \
    -o "KagamiBroadcast/$file"
  echo "$sum  KagamiBroadcast/$file" | shasum -a 256 -c -
done <<'EOF'
687416a31fd45556b09c7d5592f0f002a84b5fb367efc59714714bd65d84f27d SampleUploader.swift
27f1cef4ca283b2eadcae9508b46394680c088366ac9189d92fddf6cae9e4606 SocketConnection.swift
b3bc86bdb7021c3c56564da90c9e10595e7f93c2c22bc46972e10c287727b05c DarwinNotificationCenter.swift
a82f18f6a0648acfa416b0fd5addb44d40bb0fcf881a4c384d3498ac1b9ac295 Atomic.swift
EOF
# Two log lines use os_log's interpolating overload, which is iOS 14 and would
# set the floor by itself. Same messages, format-string overload.
perl -0pi -e 's/"client stream error occurred: \\\(String\(describing: aStream\.streamError\)\)"\)/"client stream error occurred: %{public}\@", String(describing: aStream.streamError))/; s/"failure: \\\(status\)"\)/"failure: %d", status)/' \
  KagamiBroadcast/SocketConnection.swift
if grep -nE 'os_log\(.*\\\(' KagamiBroadcast/*.swift; then
  echo "an interpolating os_log is left (iOS 14 only)" >&2
  exit 1
fi

# The floor. Flutter's own, from the project it just generated, and no lower
# than 13 -- flutter_webrtc's podspec (WebRTC-SDK) and mobile_scanner's (12)
# are the other two floors, and 13 is the higher.
flutter_floor=$(grep -m1 -oE 'IPHONEOS_DEPLOYMENT_TARGET = [0-9.]+' Runner.xcodeproj/project.pbxproj | grep -oE '[0-9.]+$')
floor=$(printf '%s\n13.0\n' "$flutter_floor" | sort -t. -k1,1n -k2,2n | tail -1)
echo "iOS floor: $floor (Flutter's: $flutter_floor)"
sed -i '' -E "s/IPHONEOS_DEPLOYMENT_TARGET = [0-9.]+;/IPHONEOS_DEPLOYMENT_TARGET = $floor;/" Runner.xcodeproj/project.pbxproj
# No Podfile edit: CocoaPods takes a missing platform from the target it
# integrates, which is the floor just set, and fails `pod install` for a pod
# that needs more -- rather than building for a phone it cannot run on.

# Ours, copied into the ignored scaffold: the app's entitlements, the
# keep-alive, and the icon (branding/make-icons.py) over the Flutter logo.
cp Kagami/Runner.entitlements Kagami/KagamiKeepAlive.m Runner/
rm -rf Runner/Assets.xcassets/AppIcon.appiconset
cp -R Kagami/AppIcon.appiconset Runner/Assets.xcassets/

# Info.plist: set, not added, so a re-run is the same file.
plist=Runner/Info.plist
set_key() { /usr/libexec/PlistBuddy -c "Delete :$1" "$plist" 2>/dev/null || true
            /usr/libexec/PlistBuddy -c "Add :$1 $2 $3" "$plist"; }
set_key CFBundleDisplayName string Kagami
# flutter_webrtc reads the group to find the extension's socket
# (RTCScreenSharingExtension, the extension's bundle id, is set below).
set_key RTCAppGroupIdentifier string group.io.kagami.sender
set_key NSCameraUsageDescription string "Kagami scans the pairing code shown on the desktop."
set_key NSLocalNetworkUsageDescription string "Kagami connects straight to the desktop when both are on the same network."
# Runner/KagamiKeepAlive.m says why: the frames pass through this app, and a
# suspended app stops reading them.
/usr/libexec/PlistBuddy -c "Delete :UIBackgroundModes" "$plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :UIBackgroundModes array" -c "Add :UIBackgroundModes:0 string audio" "$plist"

# The extension target. The script reads the app's bundle id from the Runner
# target and prints it last; the picker preselects the extension by it.
host_id=$(ruby ../../tools/ios/add-broadcast-extension.rb Runner.xcodeproj "$floor" | tail -1)
set_key RTCScreenSharingExtension string "$host_id.broadcast"

echo "host $host_id, extension $host_id.broadcast, floor iOS $floor"
