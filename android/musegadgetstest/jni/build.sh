#!/bin/sh
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

# Builds lib/arm64-v8a/libmgx264.so (the software encoder, X264.java) with the
# Android NDK: libx264 from source (X264_SRC, a checkout of
# https://code.videolan.org/videolan/x264), static, NEON, then the JNI wrapper.
#   NDK=/path/to/android-ndk-r27c X264_SRC=/path/to/x264 ./jni/build.sh
# build.sh packs lib/ into the APK when it exists. libx264 is GPL-2.0 (or a
# commercial license): an APK with it is under the GPL.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${NDK:?set NDK to the Android NDK}"
: "${X264_SRC:?set X264_SRC to an x264 checkout}"
host=$(ls "$NDK/toolchains/llvm/prebuilt")
tc="$NDK/toolchains/llvm/prebuilt/$host/bin"
api=26
work=${WORK:-/tmp/mgx264-build}
mkdir -p "$work/x264"
# A copy, with mv-cheap (decoder-friendly motion vectors, x264_mvcheap.py)
# unless MV_CHEAP=0. Without it the app's mv-cheap request is ignored.
rm -rf "$work/src"
cp -R "$X264_SRC" "$work/src"
if [ "${MV_CHEAP:-1}" != 0 ]; then
    python3 "$here/x264_mvcheap.py" "$work/src"
fi
(
    cd "$work/src"
    CC="$tc/aarch64-linux-android$api-clang" ./configure --host=aarch64-linux-android \
        --cross-prefix="$tc/llvm-" --sysroot="$NDK/toolchains/llvm/prebuilt/$host/sysroot" \
        --enable-static --enable-pic --disable-cli --disable-opencl --bit-depth=8 \
        --chroma-format=420 --disable-interlaced --prefix="$work/x264" >/dev/null
    make -j8 >/dev/null
    make install >/dev/null
)
out=${OUT_LIB:-$here/../lib}
mkdir -p "$out/arm64-v8a"
"$tc/aarch64-linux-android$api-clang" -O3 -shared -fPIC -Wall -I"$work/x264/include" \
    -o "$out/arm64-v8a/libmgx264.so" "$here/mgx264.c" "$work/x264/lib/libx264.a" \
    -llog -lm -Wl,--gc-sections -Wl,-z,max-page-size=16384 -s
echo "$out/arm64-v8a/libmgx264.so"
