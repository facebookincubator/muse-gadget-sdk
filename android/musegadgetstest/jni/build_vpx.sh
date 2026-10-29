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

# Builds lib/arm64-v8a/libmgvpx.so (the VP8 encoder, Vpx.java) with the
# Android NDK: libvpx's VP8 encoder from source (VPX_SRC, a checkout of
# https://chromium.googlesource.com/webm/libvpx at v1.16.0) with
# jni/libvpx_mg.py's switches, static, NEON, then the JNI wrapper.
#   NDK=/path/to/android-ndk-r27c VPX_SRC=/path/to/libvpx ./jni/build_vpx.sh
# libvpx is BSD-3-Clause with a patent grant (its LICENSE and PATENTS).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${NDK:?set NDK to the Android NDK}"
: "${VPX_SRC:?set VPX_SRC to a libvpx checkout}"
host=$(ls "$NDK/toolchains/llvm/prebuilt")
tc="$NDK/toolchains/llvm/prebuilt/$host/bin"
api=26
work=${WORK:-/tmp/mgvpx-build}
rm -rf "$work/src" "$work/build" && mkdir -p "$work/build"
cp -R "$VPX_SRC" "$work/src"
python3 "$here/libvpx_mg.py" "$work/src"
cc="$tc/aarch64-linux-android$api-clang"
(
    cd "$work/build"
    CC="$cc" CXX="$cc++" AR="$tc/llvm-ar" AS="$cc" LD="$cc" STRIP="$tc/llvm-strip" NM="$tc/llvm-nm" \
    "$work/src/configure" --target=armv8-linux-gcc --enable-pic --enable-static --disable-shared \
        --enable-vp8-encoder --disable-vp8-decoder --disable-vp9 --disable-examples --disable-tools \
        --disable-docs --disable-unit-tests --disable-runtime-cpu-detect --enable-multithread \
        --extra-cflags=-O3 >/dev/null
    make -j8 libvpx.a >/dev/null
)
out=${OUT_LIB:-$here/../lib}
mkdir -p "$out/arm64-v8a"
"$cc" -O2 -shared -fPIC -Wall -I"$work/src" -I"$work/build" \
    -o "$out/arm64-v8a/libmgvpx.so" "$here/mgvpx.c" "$work/build/libvpx.a" \
    -llog -lm -Wl,--gc-sections -Wl,-z,max-page-size=16384 -s
echo "$out/arm64-v8a/libmgvpx.so"
