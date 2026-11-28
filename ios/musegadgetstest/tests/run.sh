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

# Host tests for the app's protocol code (capabilities, the token proof, the panel scaler),
# after tests/check_resources.sh (Resources/ matches the Android assets):
# builds the Foundation-only sources with the tests as a macOS executable and
# runs it against the repo's token proof vectors. ./tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
"$here/check_resources.sh"
src=$here/../Sources
out=$(mktemp -d "${TMPDIR:-/tmp}/musegadgetstest-iostests.XXXXXX")
trap 'rm -rf -- "$out"' EXIT
trap 'exit 1' HUP INT TERM
clang -O1 -c -o "$out/mgscale.o" "$here/../native/mgscale.c"
swiftc -parse-as-library -import-objc-header "$here/../native/mgcodecs.h" -o "$out/tests" \
    "$src/GadgetLink.swift" "$src/WifiLink.swift" "$src/VideoLink.swift" "$src/VideoFrame.swift" \
    "$src/LatencyMeter.swift" "$src/Util.swift" "$src/TokenProof.swift" "$src/PanelScaler.swift" \
    "$here"/*.swift "$out/mgscale.o"
"$out/tests" "$here/../../../protocols/test-vectors/mg-token-proof-v1.json"
