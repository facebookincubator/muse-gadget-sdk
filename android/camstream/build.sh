#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Builds Muse Gadgets Camera without Gradle (Android Studio: open this directory,
# or ./gradlew installDebug): javac, d8, aapt2, zipalign, apksigner
# from the Android SDK (ANDROID_HOME, or ~/Library/Android/sdk), signed with
# the debug key. ./build.sh [install]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
bt=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
jar=$(ls -d "$sdk"/platforms/android-* | sort -V | tail -1)/android.jar
if ! javac -version >/dev/null 2>&1; then   # macOS has a javac stub without a JDK
    export PATH="/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin:$PATH"
fi
out=${OUT:-/tmp/camstream-build}
rm -rf "$out" && mkdir -p "$out/classes" "$out/dex"
javac -source 17 -target 17 -nowarn -Xlint:-options -classpath "$jar" -d "$out/classes" \
    $(find "$here/src" -name '*.java')
"$bt/d8" --min-api 33 --lib "$jar" --output "$out/dex" $(find "$out/classes" -name '*.class')
assets=()
[ -d "$here/assets" ] && assets=(-A "$here/assets" -0 mp4 -0 264)
# The same package, versions and SDK levels as build.gradle.kts; debuggable
# (a test build, like Gradle's debug variant).
sed 's|<manifest xmlns:android="http://schemas.android.com/apk/res/android">|<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.musegadgets.camstream">|' \
    "$here/AndroidManifest.xml" > "$out/AndroidManifest.xml"
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$out/AndroidManifest.xml" \
    --min-sdk-version 33 --target-sdk-version 34 --version-code 1 --version-name 0.1 --debug-mode \
    ${assets[@]+"${assets[@]}"}
(cd "$out/dex" && zip -q "$out/base.apk" classes.dex)
# the software encoder's native library (jni/build.sh), when built
if [ -d "${LIB_DIR:-$here/lib}" ]; then
    rm -rf "$out/native" && mkdir -p "$out/native" && cp -R "${LIB_DIR:-$here/lib}" "$out/native/lib"
    (cd "$out/native" && zip -q -r -0 "$out/base.apk" lib)
fi
"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android \
    --out "$out/camstream.apk" "$out/aligned.apk"
echo "$out/camstream.apk"
if [ "${1:-}" = install ]; then
    adb install -r "$out/camstream.apk"
    adb shell pm grant com.musegadgets.camstream android.permission.CAMERA
fi
