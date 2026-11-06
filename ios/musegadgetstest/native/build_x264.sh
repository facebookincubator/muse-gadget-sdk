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

# Builds LIB_DIR/libmgx264.a (the software H.264 encoder, X264.swift): libx264
# from your checkout (X264_SRC, https://code.videolan.org/videolan/x264) with
# android/musegadgetstest/jni/x264_mvcheap.py's mv-cheap (unless MV_CHEAP=0), for
# iPhone (arm64, NEON), and native/mgx264.c, in one static library.
#   X264_SRC=~/x264 LIB_DIR=~/mgcam-ios-lib ./native/build_x264.sh
# libx264 is GPL-2.0 (or a commercial license): an app with it is under the GPL.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${X264_SRC:?set X264_SRC to an x264 checkout}"
out=${LIB_DIR:-$here/../lib}
work=${WORK:-/tmp/mgx264-ios}
rm -rf "$work/src" "$work/x264" && mkdir -p "$work" "$out"
cp -R "$X264_SRC" "$work/src"
if [ "${MV_CHEAP:-1}" != 0 ]; then
    python3 "$here/../../../android/musegadgetstest/jni/x264_mvcheap.py" "$work/src"
fi
sdk=$(xcrun -sdk iphoneos --show-sdk-path)
cc="$(xcrun -sdk iphoneos -f clang)"
flags="-arch arm64 -miphoneos-version-min=17.0 -isysroot $sdk"
(
    cd "$work/src"
    CC="$cc" ./configure --host=aarch64-apple-darwin --sysroot="$sdk" --enable-static --enable-pic \
        --disable-cli --disable-opencl --bit-depth=8 --chroma-format=420 --disable-interlaced \
        --extra-cflags="$flags" --extra-asflags="$flags" --extra-ldflags="$flags" --prefix="$work/x264" >/dev/null
    make -j8 >/dev/null
    make install >/dev/null
)
"$cc" $flags -O2 -Wall -I"$work/x264/include" -c "$here/mgx264.c" -o "$work/mgx264.o"
xcrun libtool -static -o "$out/libmgx264.a" "$work/x264/lib/libx264.a" "$work/mgx264.o" 2>/dev/null
echo "$out/libmgx264.a"
LIB_DIR="$out" "$here/codecs_xcconfig.sh"
