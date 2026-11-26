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

# Fails unless each libmgx264.a given has the functions this app calls: one
# built before the UI streams lacks mgx264_encode_qp and mgx264_recon_i420,
# and the app's link would fail with only "undefined symbol".
#   native/check_x264.sh LIB_DIR/libmgx264.a ...
set -eu
for a in "$@"; do
    syms=$(nm -gU "$a" 2>/dev/null) || { echo "$a: nm can't read it: rebuild it with native/build_x264.sh" >&2; exit 1; }
    for s in mgx264_encode_qp mgx264_recon_i420; do
        if ! grep -qE "[[:space:]]_$s\$" <<<"$syms"; then
            echo "$a is an older build without $s: rebuild it with native/build_x264.sh" >&2
            exit 1
        fi
    done
done
