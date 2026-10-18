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

# Builds Muse Gadgets Camera for iPhone without an Xcode project: swiftc and
# clang from Xcode's command-line tools, then signs it with your own Apple
# development certificate (a free Personal Team works; see signing.py, which
# finds the profile Xcode downloaded for it). ./build.sh [install]
#   BUNDLE_ID the app's bundle ID (default com.musegadgets.camstream; with a
#             free team pick your own, e.g. com.yourname.musegadgetscamera)
#   DEVICE    the iPhone: xcrun devicectl list devices (needed for install,
#             and picks a profile that lists it)
#   PROFILE   a development .mobileprovision to use instead of looking
#   IDENTITY  the signing identity to use instead of the profile's
#   LIB_DIR   where native/build_x264.sh and native/build_vpx.sh put
#             libmgx264.a and libmgvpx.a (default lib/); each is optional
#   SIM=1     build for the iOS Simulator instead (no signing, no x264 or
#             VP8, no camera: Bars and the clips work); install puts it on the
#             booted simulator
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sim=${SIM:-}
bundle=${BUNDLE_ID:-com.musegadgets.camstream}
lib=${LIB_DIR:-$here/lib}
out=${OUT:-/tmp/mgcam-ios-build}
app="$out/MuseGadgetsCamera.app"
plat=iphoneos
target=arm64-apple-ios17.0
if [ -n "$sim" ]; then
    plat=iphonesimulator
    target=arm64-apple-ios17.0-simulator
    lib=/nonexistent   # LIB_DIR's libraries are built for the device
fi
sdk=$(xcrun -sdk $plat --show-sdk-path)
rm -rf "$out" && mkdir -p "$app" "$out/obj"

# The C side: the panel scaler, and each encoder from LIB_DIR or its stub.
cflags=(-target "$target" -isysroot "$sdk" -O2 -Wall)
xcrun -sdk $plat clang "${cflags[@]}" -c "$here/native/mgscale.c" -o "$out/obj/mgscale.o"
objs=("$out/obj/mgscale.o")
stubs=()
libs=()
if [ -f "$lib/libmgx264.a" ]; then libs+=("$lib/libmgx264.a"); else stubs+=(-DMG_STUB_X264); fi
if [ -f "$lib/libmgvpx.a" ]; then libs+=("$lib/libmgvpx.a"); else stubs+=(-DMG_STUB_VPX); fi
if [ ${#stubs[@]} -gt 0 ]; then
    xcrun -sdk $plat clang "${cflags[@]}" "${stubs[@]}" -c "$here/native/stubs.c" -o "$out/obj/stubs.o"
    objs+=("$out/obj/stubs.o")
fi

xcrun -sdk $plat swiftc -O -swift-version 5 -target "$target" -sdk "$sdk" \
    -import-objc-header "$here/native/Bridging.h" -I "$here/native" \
    "$here"/Sources/*.swift "${objs[@]}" ${libs[@]+"${libs[@]}"} -lc++ \
    -Xlinker -rpath -Xlinker @executable_path/Frameworks -o "$app/MuseGadgetsCamera"

sed "s/@BUNDLE_ID@/$bundle/" "$here/Info.plist" > "$app/Info.plist"
plutil -convert binary1 "$app/Info.plist"
if [ -n "$sim" ]; then
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
identity=${IDENTITY:-$sha}
codesign -f -s "$identity" --entitlements "$out/app.entitlements" --timestamp=none "$app" >/dev/null 2>&1
codesign -v "$app"
echo "$app (x264: $([ -f "$lib/libmgx264.a" ] && echo yes || echo no), VP8: $([ -f "$lib/libmgvpx.a" ] && echo yes || echo no))"
if [ "${1:-}" = install ]; then
    : "${DEVICE:?set DEVICE (xcrun devicectl list devices)}"
    xcrun devicectl device install app --device "$DEVICE" "$app" | tail -3
fi
