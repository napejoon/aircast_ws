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

# Debug builds are signed with the committed key, named in Gradle rather than
# left for Gradle to find.
#
# Every CI runner is a fresh machine, and Android refuses to update a package
# whose signature changed -- so the APK ci.yml calls "the one a maintainer hands
# someone to try" could only be installed by uninstalling the last one first:
# INSTALL_FAILED_UPDATE_INCOMPATIBLE. The first fix copied the committed key to
# ~/.android/debug.keystore, and CI logged the copy on every run -- and the APKs
# went on carrying different signatures anyway. Gradle does not look there on
# the runner: which debug.keystore the Android plugin opens depends on
# ANDROID_USER_HOME, ANDROID_SDK_HOME and the image the runner happens to be,
# and two builds of the same branch a week apart landed on different ones.
# Naming the file in signingConfigs takes the environment out of the answer.
#
# It is Android's own debug key, parameters and all -- alias androiddebugkey,
# password "android", the values every SDK generates -- so it is not a secret
# and is not treated as one. What keeps it away from a release is the
# signingConfig deletion above, which fails the build if Gradle still signs
# release builds at all; this block only modifies the debug config that
# already exists and assigns it to nothing.
sed -i 's|^android {$|android {\n    signingConfigs {\n        getByName("debug") {\n            storeFile = file("../debug.keystore")\n            storePassword = "android"\n            keyAlias = "androiddebugkey"\n            keyPassword = "android"\n        }\n    }|' build.gradle.kts
grep -qF 'storeFile = file("../debug.keystore")' build.gradle.kts || {
  echo "scaffold moved: 'android {' is no longer a line of its own in android/app/build.gradle.kts" >&2
  exit 1
}

grep -nE 'minSdk|compileSdk|targetSdk|debug.keystore' build.gradle.kts
