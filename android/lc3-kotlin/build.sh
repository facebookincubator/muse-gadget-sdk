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

# Builds lc3-kotlin without Gradle (./gradlew test jar does the same through Gradle), with kotlinc and a JDK.
#   ./build.sh          build/lc3-kotlin.jar (the library: no runtime dependencies)
#   ./build.sh test     + run the conformance checks (Lc3Checks) against the liblc3 reference vectors
#   ./build.sh bench    + build/lc3-bench.jar, the file to file encoder and decoder tools/compare_liblc3.sh runs
# KOTLINC overrides the kotlinc on PATH.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$here/build
kotlinc=${KOTLINC:-kotlinc}
warnings='^warning|^[^ ]+\.kts?:[0-9]+:[0-9]+: warning:'
flags="-Xno-param-assertions -Xno-call-assertions -Xno-receiver-assertions -jvm-target 17"
# Runs a tool with its warning lines ("warning: ..." and "File.kt:L:C: warning: ...") filtered out, keeping its
# exit status.
quiet() {
    local skip=$1 log rc=0
    shift
    log=$("$@" 2>&1) || rc=$?
    grep -v -E "$skip" <<<"$log" || true
    return $rc
}
rm -rf "$out/sh-classes"; mkdir -p "$out/sh-classes"
quiet "$warnings" "$kotlinc" $flags "$here/src/main/kotlin" -d "$out/sh-classes"
cp -R "$here/src/main/resources/." "$out/sh-classes/"
cp "$here/LICENSE" "$here/NOTICE" "$out/sh-classes/META-INF/"
(cd "$out/sh-classes" && jar cf "$out/lc3-kotlin.jar" .)
echo "built $out/lc3-kotlin.jar"
case "${1:-}" in
test)
    quiet "$warnings" "$kotlinc" -cp "$out/lc3-kotlin.jar" $(ls "$here"/src/test/kotlin/com/musegadgets/lc3/*.kt | grep -v ConformanceTest) \
        -include-runtime -d "$out/lc3-tests.jar"
    java -cp "$out/lc3-tests.jar:$out/lc3-kotlin.jar:$here/src/test/resources" com.musegadgets.lc3.RunTests
    ;;
bench)
    quiet "$warnings" "$kotlinc" -cp "$out/lc3-kotlin.jar" "$here/bench/src" -include-runtime -d "$out/lc3-bench-only.jar"
    rm -rf "$out/bench-merge" && mkdir -p "$out/bench-merge"
    (cd "$out/bench-merge" && unzip -q -o "$out/lc3-bench-only.jar" && unzip -q -o "$out/lc3-kotlin.jar" && jar cfe "$out/lc3-bench.jar" com.musegadgets.lc3.bench.BenchMain .)
    echo "built $out/lc3-bench.jar (java -jar build/lc3-bench.jar enc IN.raw OUT.lc3 ...)"
    ;;
esac
