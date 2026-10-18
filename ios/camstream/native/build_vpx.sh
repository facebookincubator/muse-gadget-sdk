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

# Builds LIB_DIR/libmgvpx.a (VP8 for Muse Gadgets Camera, Vpx.swift): libvpx's
# VP8 encoder from your checkout (VPX_SRC, https://chromium.googlesource.com/webm/libvpx
# at v1.16.0) with android/camstream/jni/libvpx_mg.py's two switches, for
# iPhone (arm64, NEON), and native/mgvpx.c, in one static library.
#   VPX_SRC=~/libvpx LIB_DIR=~/mgcam-ios-lib ./native/build_vpx.sh
# libvpx is BSD-3-Clause with a patent grant (its LICENSE and PATENTS).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${VPX_SRC:?set VPX_SRC to a libvpx checkout}"
out=${LIB_DIR:-$here/../lib}
work=${WORK:-/tmp/mgvpx-ios}
rm -rf "$work/src" "$work/build" && mkdir -p "$work/build" "$out"
cp -R "$VPX_SRC" "$work/src"
python3 "$here/../../../android/camstream/jni/libvpx_mg.py" "$work/src"
(
    cd "$work/build"
    "$work/src/configure" --target=arm64-darwin-gcc --enable-static --disable-shared \
        --enable-vp8-encoder --disable-vp8-decoder --disable-vp9 --disable-examples --disable-tools \
        --disable-docs --disable-unit-tests --disable-runtime-cpu-detect --enable-multithread \
        --extra-cflags="-O3 -miphoneos-version-min=17.0" >/dev/null
    make -j8 libvpx.a >/dev/null
)
sdk=$(xcrun -sdk iphoneos --show-sdk-path)
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -O2 -Wall -isysroot "$sdk" \
    -I"$work/src" -I"$work/build" -c "$here/mgvpx.c" -o "$work/mgvpx.o"
xcrun libtool -static -o "$out/libmgvpx.a" "$work/build/libvpx.a" "$work/mgvpx.o" 2>/dev/null
echo "$out/libmgvpx.a"
