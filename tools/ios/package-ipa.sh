#!/usr/bin/env bash
# Package the app `flutter build ios --no-codesign` produced as an IPA a
# sideloading tool can sign with a free Apple ID (sender/ios/README.md).
#
#   tools/ios/package-ipa.sh            -> sender/Kagami-sideload.ipa
#
# Ad hoc signed, not unsigned: a tool that re-signs an app takes the
# entitlements from the signature it finds, and a build with no signature has
# none -- so the App Group the extension and the app share would silently not
# be asked for, and the extension would capture into a socket nobody reads.
# The ad hoc signature carries the entitlements and nothing else; no device
# will run it as is.
#
# Then checks, from the built bundle rather than from the project, the things
# that each fail on the phone with nothing on screen to say why.
set -euo pipefail
cd "$(dirname "$0")/../../sender"

APP=build/ios/iphoneos/Runner.app
EXT="$APP/PlugIns/KagamiBroadcast.appex"
GROUP=group.io.kagami.sender
[ -d "$EXT" ] || { echo "no $EXT: the extension was not embedded" >&2; exit 1; }

# Inside out: what a bundle contains is signed before the bundle.
for f in "$APP"/Frameworks/*.framework "$APP"/Frameworks/*.dylib; do
  if [ -e "$f" ]; then codesign --force --sign - "$f"; fi
done
codesign --force --sign - --generate-entitlement-der \
  --entitlements ios/KagamiBroadcast/KagamiBroadcast.entitlements "$EXT"
codesign --force --sign - --generate-entitlement-der \
  --entitlements ios/Kagami/Runner.entitlements "$APP"

fail() { echo "package-ipa: $*" >&2; exit 1; }
key() { /usr/libexec/PlistBuddy -c "Print :$2" "$1/Info.plist"; }

for b in "$APP" "$EXT"; do
  codesign -d --entitlements - --xml "$b" 2>/dev/null | grep -q "$GROUP" \
    || fail "$b does not carry the App Group $GROUP"
done
app_id=$(key "$APP" CFBundleIdentifier)
ext_id=$(key "$EXT" CFBundleIdentifier)
[ "$ext_id" = "$app_id.broadcast" ] || fail "extension id $ext_id is not $app_id.broadcast"
[ "$(key "$APP" RTCScreenSharingExtension)" = "$ext_id" ] \
  || fail "the picker would preselect $(key "$APP" RTCScreenSharingExtension), not $ext_id"
[ "$(key "$APP" RTCAppGroupIdentifier)" = "$GROUP" ] || fail "app reads another App Group"
[ "$(key "$EXT" RTCAppGroupIdentifier)" = "$GROUP" ] || fail "extension reads another App Group"
[ "$(key "$EXT" NSExtension:NSExtensionPointIdentifier)" = com.apple.broadcast-services-upload ] \
  || fail "extension is not a broadcast upload extension"
for k in CFBundleShortVersionString CFBundleVersion MinimumOSVersion; do
  [ "$(key "$APP" "$k")" = "$(key "$EXT" "$k")" ] \
    || fail "$k differs: app $(key "$APP" "$k"), extension $(key "$EXT" "$k")"
done
key "$APP" UIBackgroundModes | grep -q audio || fail "no audio background mode (Kagami/KagamiKeepAlive.m)"
key "$APP" NSCameraUsageDescription >/dev/null || fail "no camera string: the scanner would crash the app"

rm -rf Payload Kagami-sideload.ipa
mkdir Payload && cp -R "$APP" Payload/
zip -qry Kagami-sideload.ipa Payload && rm -rf Payload
echo "Kagami-sideload.ipa: $app_id + $ext_id, iOS $(key "$APP" MinimumOSVersion)+, version $(key "$APP" CFBundleShortVersionString) ($(key "$APP" CFBundleVersion))"
