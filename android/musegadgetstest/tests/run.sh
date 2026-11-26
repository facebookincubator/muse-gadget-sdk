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

# Host tests for the app's protocol code (capabilities, the token proof): compiles the tests
# with the app's sources against android.jar and runs them on the desktop JVM
# (they touch no Android API); argument 1 is the token proof vectors' JSON. ./tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
jar=$( (ls -d "$sdk"/platforms/android-* 2>/dev/null || true) | sort -V | tail -1)/android.jar
[ -f "$jar" ] || { echo "no android.jar under $sdk: set ANDROID_HOME" >&2; exit 1; }
if ! javac -version >/dev/null 2>&1; then   # macOS has a javac stub without a JDK
    export PATH="/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin:$PATH"
fi
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
tests=($(cd "$here" && find com -name '*Test.java'))
javac -source 17 -target 17 -nowarn -Xlint:-options -classpath "$jar" -sourcepath "$here/../src:$here" \
    -d "$out" "${tests[@]/#/$here/}"
for t in "${tests[@]}"; do
    java -cp "$out:$jar" "$(echo "${t%.java}" | tr / .)" "$here/../../../protocols/test-vectors/mg-token-proof-v1.json"
done
