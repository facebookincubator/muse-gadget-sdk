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

# Writes lib/codecs.xcconfig (git-ignored), which the Xcode project includes
# (Config.xcconfig): links whichever of LIB_DIR/libmgx264.a and
# LIB_DIR/libmgvpx.a exist into device builds and stubs the rest. The
# build_*.sh scripts run it; run it yourself after moving the libraries.
#   LIB_DIR=~/mgcam-ios-lib ./native/codecs_xcconfig.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
lib=$(cd "${LIB_DIR:-$here/../lib}" && pwd)
defines="" flags=""
for c in x264 vpx; do
    if [ -f "$lib/libmg$c.a" ]; then
        flags="$flags \"$lib/libmg$c.a\""
    else
        defines="$defines MG_STUB_$(echo $c | tr a-z A-Z)=1"
    fi
done
mkdir -p "$here/../lib"
cat > "$here/../lib/codecs.xcconfig" <<XC
// Written by native/codecs_xcconfig.sh from $lib
MG_CODEC_DEFINES[sdk=iphoneos*] =$defines
MG_CODEC_LDFLAGS[sdk=iphoneos*] =$flags
XC
echo "$here/../lib/codecs.xcconfig:${flags:- no encoders}"
