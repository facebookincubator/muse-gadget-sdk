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

# Resources/ holds copies of the Android app's assets (the tone loops and the
# UI demo pages): fails unless each copy is a regular file (no links) equal to
# the Android original, and ui/ has nothing missing or extra on either side.
# After changing an asset, copy it to both. ./tests/check_resources.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ios=$here/../Resources
android=$here/../../../android/musegadgetstest/assets
links=$(find "$ios" -type l)
if [ -n "$links" ]; then
    echo "check_resources: links in Resources/ (copy the files instead): $links" >&2
    exit 1
fi
for f in tone.lc3 tone.sbc; do
    cmp "$ios/$f" "$android/$f" || { echo "check_resources: Resources/$f differs from the Android asset" >&2; exit 1; }
done
diff -r "$ios/ui" "$android/ui" || { echo "check_resources: Resources/ui differs from the Android assets/ui" >&2; exit 1; }
echo "check_resources: Resources/ matches the Android assets"
