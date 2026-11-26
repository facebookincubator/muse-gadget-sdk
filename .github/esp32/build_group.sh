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

# Build one group of boards (matrix.py's "boards", as JSON in $BOARDS), one
# after another, in esp32/ with ESP-IDF already exported. Each build fails if
# the app no longer fits its partition; web flasher boards are packaged to
# out/<board>/. Every board is built even when an earlier one fails, from the
# same clean state a fresh checkout has; the exit status says whether all of
# them built. BUILD_TIMEOUT_S caps each board's build.
set -uo pipefail
: "${BOARDS:?}"
limit=${BUILD_TIMEOUT_S:-900}
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
list=$(python3 -c 'import json, os
for b in json.loads(os.environ["BOARDS"]):
    print(b["board"], b["target"], b["overlays"], sep="\t")') && [ -n "$list" ] || { echo "::error::no boards in \$BOARDS"; exit 1; }
failed=()
echo "| Board | Target | Result | Build s |" >> "$summary"
echo "|---|---|---|---|" >> "$summary"
while IFS=$'\t' read -r -u 3 board target overlays; do   # fd 3: the builds may read stdin
    echo "::group::$board ($target)"
    # The component manager fails while pruning another board's components:
    # each board starts without them, as in a fresh checkout.
    rm -rf build managed_components dependencies.lock
    s=$(date +%s)
    timeout "$limit" idf.py -B build -DIDF_TARGET="$target" -DSDKCONFIG=build/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults$overlays" build
    rc=$?
    t=$(( $(date +%s) - s ))
    if [ $rc = 0 ]; then
        if NAME=$(BOARD="$board" python3 -c 'import json, os; print(next((b["name"] for b in json.load(open("../web-flasher/boards.json")) if b["board"] == os.environ["BOARD"]), ""))'); then
            if [ -n "$NAME" ]; then
                # The manifest's version is the image's own, what the gadget reports.
                python3 ../web-flasher/make_manifest.py build "out/$board" "Muse Gadget ($NAME)" || rc=$?
            fi
        else
            echo "::error::$board: could not read web-flasher/boards.json"
            rc=1
        fi
    fi
    echo "::endgroup::"
    if [ $rc = 0 ]; then
        echo "$board: built in $t s"
        echo "| $board | $target | ok | $t |" >> "$summary"
    else
        if [ $rc = 124 ]; then echo "::error::$board: build took over $limit s"; else echo "::error::$board: build failed ($rc)"; fi
        echo "| $board | $target | FAILED ($rc) | $t |" >> "$summary"
        failed+=("$board")
    fi
done 3<<< "$list"
rm -rf build managed_components dependencies.lock
command -v ccache > /dev/null && ccache -s
if [ ${#failed[@]} -gt 0 ]; then
    echo "::error::failed: ${failed[*]}"
    exit 1
fi
