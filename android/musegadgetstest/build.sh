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

# Builds Muse Gadgets Test without Gradle (Android Studio: open this directory,
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
# An x264 library from before the UI streams lacks their JNI functions (the app would fail when one
# starts): stopped here.
x264so="${LIB_DIR:-$here/lib}/arm64-v8a/libmgx264.so"
if [ -f "$x264so" ]; then
    for f in openUi encodeQp reconI420 rgbaConvert; do
        LC_ALL=C grep -qaF "Java_com_musegadgets_test_X264_$f" "$x264so" || {
            echo "$x264so is an older build without X264.$f: rebuild it with jni/build.sh" >&2
            exit 1
        }
    done
fi
out=${OUT:-/tmp/musegadgetstest-build}
app_id=${APP_ID:-com.musegadgets.test}
rm -rf "$out" && mkdir -p "$out/classes" "$out/dex"
srcs=($(find "$here/src" -name '*.java'))
javac -source 17 -target 17 -nowarn -Xlint:-options -classpath "$jar" -d "$out/classes" "${srcs[@]}"
"$bt/d8" --min-api 33 --lib "$jar" --output "$out/dex" $(find "$out/classes" -name '*.class')
assets=()
[ -d "$here/assets" ] && assets=(-A "$here/assets" -0 mp4 -0 264 -0 ivf)
# The same package, versions and SDK levels as build.gradle.kts; debuggable
# (a test build, like Gradle's debug variant).
# APP_ID (and APP_LABEL) install a second copy beside the usual one, e.g.
# APP_ID=com.musegadgets.test.ui APP_LABEL="MG Test (UI)".
sed -e 's|<manifest xmlns:android="http://schemas.android.com/apk/res/android">|<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.musegadgets.test">|' \
    -e "s|android:label=\"Muse Gadgets Test\"|android:label=\"${APP_LABEL:-Muse Gadgets Test}\"|" \
    "$here/AndroidManifest.xml" > "$out/AndroidManifest.xml"
"$bt/aapt2" compile --dir "$here/res" -o "$out/res.zip"
# APP_ID installs beside the usual build (e.g. com.musegadgets.test.ui); the classes keep their package.
rename=()
[ "$app_id" != com.musegadgets.test ] && rename=(--rename-manifest-package "$app_id")
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$out/AndroidManifest.xml" \
    --min-sdk-version 33 --target-sdk-version 34 --version-code 1 --version-name 0.1 --debug-mode \
    ${rename[@]+"${rename[@]}"} ${assets[@]+"${assets[@]}"} "$out/res.zip"
(cd "$out/dex" && zip -q "$out/base.apk" classes.dex)
# the native libraries: the encoders (jni/build*.sh) when built
rm -rf "$out/native" && mkdir -p "$out/native/lib/arm64-v8a"
if [ -d "${LIB_DIR:-$here/lib}" ]; then
    cp -R "${LIB_DIR:-$here/lib}/." "$out/native/lib/"
fi
if [ -n "$(ls -A "$out/native/lib/arm64-v8a")" ]; then
    (cd "$out/native" && zip -q -r -0 "$out/base.apk" lib)
fi
"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android \
    --out "$out/MuseGadgetsTest.apk" "$out/aligned.apk"
echo "$out/MuseGadgetsTest.apk"
if [ "${1:-}" = install ]; then
    adb install -r "$out/MuseGadgetsTest.apk"
    for p in CAMERA BLUETOOTH_SCAN BLUETOOTH_CONNECT RECORD_AUDIO; do
        adb shell pm grant "$app_id" android.permission.$p
    done
fi
