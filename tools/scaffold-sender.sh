#!/usr/bin/env bash
# Regenerate the Flutter scaffold this repo deliberately does not commit, and
# re-apply the patches every Android build depends on.
#
#   tools/scaffold-sender.sh          # from anywhere; operates on sender/
#
# `flutter create` never overwrites a file that is already there, so this is
# safe to re-run. It lives here rather than in YAML because ci.yml and
# release.yml both need it, and a patch that drifts between the two means the
# APK that ships is not the APK that was tested.
set -euo pipefail

cd "$(dirname "$0")/../sender"

flutter create --platforms=android,ios --org io.kagami --project-name kagami_sender .
# The scaffold drops in a widget test for a MyApp that is not ours.
rm -f test/widget_test.dart

cd android/app

# `sed -i` exits 0 when it matches nothing, so an unchecked patch does not fail
# the build — it silently ships the scaffold's default instead.
pin() {
  sed -i "s/$1/$2/" build.gradle.kts
  grep -qF -- "$2" build.gradle.kts || {
    echo "scaffold moved: '$1' is no longer in android/app/build.gradle.kts" >&2
    exit 1
  }
}

# 26, not Flutter's 24: UsbCastPlugin calls startForegroundService, and
# UsbCastService calls createNotificationChannel and Notification.Builder(Context,
# String) — all API 26, none of them guarded. Too low a floor does not fail a
# build, it ships an APK that installs on phones it crashes on. flutter_webrtc's
# own floor is 21, so it was never the reason for the 23 that used to be here.
pin 'minSdk = flutter.minSdkVersion' 'minSdk = 26'
# Pinned rather than followed: left alone these track whatever Flutter the runner
# happened to install, and every mediaProjection rule the cast path obeys is keyed
# to targetSdk. An upstream release should not be able to retarget a shipped app.
pin 'compileSdk = flutter.compileSdkVersion' 'compileSdk = 36'
pin 'targetSdk = flutter.targetSdkVersion' 'targetSdk = 36'

# The scaffold signs release builds with the debug keystore so `flutter run
# --release` works. Shipping that is worse than shipping nothing: the debug key
# is on every machine with an Android SDK, so anyone could mint an update for it.
# With no signingConfig, Gradle emits app-release-unsigned.apk instead — the file
# the release ceremony signs offline.
sed -i '/signingConfig = signingConfigs.getByName("debug")/d' build.gradle.kts
if grep -qE 'signingConfig[[:space:]]*=' build.gradle.kts; then
  echo "Gradle still signs release builds — refusing to produce a debug-keyed APK" >&2
  exit 1
fi

grep -nE 'minSdk|compileSdk|targetSdk' build.gradle.kts
