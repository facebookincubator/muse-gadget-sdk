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

# Builds Muse Gadgets Test from the command line: the Xcode project, unsigned,
# then signed with your own Apple development certificate and a profile Xcode
# has downloaded for it (signing.py; a free Personal Team works), and
# optionally installed. Xcode itself needs none of this: open
# MuseGadgetsTest.xcodeproj. ./build.sh [install]
#   BUNDLE_ID the app's bundle ID (default: Local.xcconfig's, else
#             Config.xcconfig's, com.musegadgets.test; with a free team
#             pick your own, e.g. com.yourname.musegadgetstest)
#   DEVICE    the iPhone: xcrun devicectl list devices (needed for install,
#             and picks a profile that lists it)
#   PROFILE   a development .mobileprovision to use instead of looking
#   IDENTITY  the signing identity to use instead of the profile's
#   LIB_DIR   where native/build_x264.sh and native/build_vpx.sh put
#             libmgx264.a and libmgvpx.a: rewrites lib/codecs.xcconfig
#             (native/codecs_xcconfig.sh) before building
#   SIM=1     build for the iOS Simulator instead (no signing, no x264 or
#             VP8, no camera: Bars and the clips work); install puts it on the
#             booted simulator
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
setting() {   # the last value of an xcconfig setting, Local.xcconfig first
    for f in "$here/Local.xcconfig" "$here/Config.xcconfig"; do
        [ -f "$f" ] || continue
        v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*/\1/p" "$f" | tail -1)
        [ -n "$v" ] && { echo "$v"; return; }
    done
}
bundle=${BUNDLE_ID:-$(setting PRODUCT_BUNDLE_IDENTIFIER)}
out=${OUT:-/tmp/mgcam-ios-build}
if [ -n "${LIB_DIR:-}" ]; then
    LIB_DIR="$LIB_DIR" "$here/native/codecs_xcconfig.sh" >/dev/null
fi
sdk=iphoneos
[ -n "${SIM:-}" ] && sdk=iphonesimulator
xcodebuild -project "$here/MuseGadgetsTest.xcodeproj" -scheme MuseGadgetsTest -configuration Release \
    -sdk $sdk -derivedDataPath "$out" PRODUCT_BUNDLE_IDENTIFIER="$bundle" CODE_SIGNING_ALLOWED=NO \
    build > "$out.log" 2>&1 || { grep -E "error:" "$out.log" | sort -u >&2; echo "build failed: $out.log" >&2; exit 1; }
app="$out/Build/Products/Release-$sdk/MuseGadgetsTest.app"
if [ -n "${SIM:-}" ]; then
    codesign -f -s - "$app" 2>/dev/null
    echo "$app (simulator)"
    [ "${1:-}" = install ] && xcrun simctl install booted "$app"
    exit 0
fi
# The device's UDID, for a profile that lists it (DEVICE may be a name or an identifier).
udid=""
if [ -n "${DEVICE:-}" ]; then
    udid=$(xcrun devicectl list devices 2>/dev/null | grep -F "$DEVICE" | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}' | head -1 || true)
fi
sha=$(python3 "$here/signing.py" "$bundle" "$app" "${PROFILE:-}" "$udid")
mv "$app/app.entitlements" "$out/app.entitlements"
codesign -f -s "${IDENTITY:-$sha}" --entitlements "$out/app.entitlements" --timestamp=none "$app" >/dev/null 2>&1
codesign -v "$app"
codecs=$(grep -oE 'libmg(x264|vpx)\.a' "$here/lib/codecs.xcconfig" 2>/dev/null | tr '\n' ' ' || true)
echo "$app (encoders: ${codecs:-none})"
if [ "${1:-}" = install ]; then
    : "${DEVICE:?set DEVICE (xcrun devicectl list devices)}"
    xcrun devicectl device install app --device "$DEVICE" "$app" | tail -3
fi
