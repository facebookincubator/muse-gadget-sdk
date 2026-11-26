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

# Compares the Kotlin codec with liblc3 v1.1.3 (scalar, no FMA contraction) on any 16 kHz s16le recording:
# encoder bitstreams (LTPF off) and decoder PCM (streams from both encoders, liblc3's with LTPF on too, with and
# without lost frames) must be byte-identical, at 7.5 and 10 ms and 20..400 bytes a frame.
#   tools/compare_liblc3.sh SPEECH.raw      (needs ./build.sh bench and tools/make_vectors.sh's liblc3 build)
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
A=$1; O=$here/build/compare; mkdir -p "$O"
R=$here/build/vectors/lc3ref
[ -x "$R" ] || { echo "run tools/make_vectors.sh first (it builds build/vectors/lc3ref)"; exit 1; }
K="java -jar $here/build/lc3-bench.jar"
# Runs a codec command on a fresh output file (the first argument) and stops at its first failure.
run() { local o=$1; shift; rm -f "$o"; "$@" || { echo "FAILED: $*" >&2; exit 1; }; }
fail=0
for dt in 10000 7500; do for nb in 20 30 40 60 80 100 160 400; do
    t=d${dt}_b${nb}; r=""
    run "$O/c_$t.lc3" $R enc "$A" "$O/c_$t.lc3" -d $dt -b $nb -l
    run "$O/k_$t.lc3" $K enc "$A" "$O/k_$t.lc3" -d $dt -b $nb >/dev/null
    cmp -s "$O/c_$t.lc3" "$O/k_$t.lc3" && r="enc ok" || { r="enc FAIL"; fail=1; }
    run "$O/cl_$t.lc3" $R enc "$A" "$O/cl_$t.lc3" -d $dt -b $nb
    for s in k_$t cl_$t; do for P in 0 9; do
        run "$O/c.raw" $R dec "$O/$s.lc3" "$O/c.raw" -d $dt -b $nb -P $P
        run "$O/k.raw" $K dec "$O/$s.lc3" "$O/k.raw" -d $dt -b $nb -P $P >/dev/null
        cmp -s "$O/c.raw" "$O/k.raw" && r="$r, dec ok" || { r="$r, dec FAIL($s P$P)"; fail=1; }
    done; done
    echo "$t: $r"
done; done
[ $fail = 0 ] && echo "IDENTICAL to liblc3" || { echo "MISMATCH"; exit 1; }
