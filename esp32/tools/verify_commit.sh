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
#
# The full ESP32 host suite on one exact commit, in a clean worktree, with nothing skipped for
# want of setup: the simulator built, cJSON linked, ESP-IDF exported, and a Python that has
# smpclient, bleak, cryptography and ppk2-api. Run it before every push: it fails if setup
# fails or any test is skipped.
#
#   esp32/tools/verify_commit.sh <commit>
#
# Settings:
#   VERIFY_REPO     the git checkout to make the worktree from (default: this script's repo)
#   VERIFY_SCRATCH  where the worktree goes (default $TMPDIR, else /tmp)
#   VERIFY_PY       the Python (default: the active virtualenv's, else python3)
#   IDF_EXPORT      ESP-IDF v6.0.3's export.sh (default $IDF_PATH/export.sh)
# The commit (HEAD, a branch, a short sha) is resolved to its sha first: the worktree goes in a
# new private directory under $VERIFY_SCRATCH for each run (mktemp -d, verify-<sha12>.XXXXXX,
# removed when the run ends, however it ends) and the logs in another, so runs from other
# sessions or clones never share state, two runs of the same commit included. It prints every
# skip.
# A simulator that doesn't build fails the run before the suite starts (its tests would only
# skip).
set -u
ARG=${1:?usage: verify_commit.sh <commit>}
REPO=${VERIFY_REPO:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}
SCRATCH=${VERIFY_SCRATCH:-${TMPDIR:-/tmp}}
FULL=$(git -C "$REPO" rev-parse --verify --quiet "$ARG^{commit}") || { echo "verify_commit: no commit $ARG"; exit 1; }
SHA=${FULL:0:12}
mkdir -p "$SCRATCH"
PY=${VERIFY_PY:-${VIRTUAL_ENV:+$VIRTUAL_ENV/bin/python}}
[ -n "$PY" ] && [ -x "$PY" ] || PY=$(command -v python3)
IDF_EXPORT=${IDF_EXPORT:-${IDF_PATH:+$IDF_PATH/export.sh}}
[ -n "$IDF_EXPORT" ] && [ -f "$IDF_EXPORT" ] \
  || { echo "verify_commit: set IDF_EXPORT (or IDF_PATH) to ESP-IDF v6.0.3's export.sh"; exit 1; }
TIMEOUT=$(command -v timeout || command -v gtimeout || true)
PARENT=$(mktemp -d "$SCRATCH/verify-$SHA.XXXXXX") || exit 1
W=$PARENT/worktree
cleanup() { cd "$REPO" && git worktree remove --force "$W" 2> /dev/null; rm -rf "$PARENT"; }
trap cleanup EXIT
LOGS=$(mktemp -d "$SCRATCH/verify-$SHA-logs.XXXXXX") || exit 1
SIMLOG=$LOGS/simulator.log LOG=$LOGS/tests.log
cd "$REPO" && git worktree prune && git worktree add -q --detach "$W" "$FULL" || exit 1
fail() { echo "verify_commit: $1"; exit 1; }
cd "$W/esp32" || fail "no esp32/ in $FULL"
cmake -S simulator -B simulator/build -G Ninja -DCMAKE_BUILD_TYPE=Debug > "$SIMLOG" 2>&1 \
  && cmake --build simulator/build >> "$SIMLOG" 2>&1 \
  || { tail -5 "$SIMLOG"; fail "simulator build failed"; }
CJ=$(ls -d "$HOME"/Library/Caches/Espressif/ComponentManager/*/espressif__cjson* "$HOME"/.cache/Espressif/ComponentManager/*/espressif__cjson* 2>/dev/null | head -1)
[ -n "$CJ" ] || fail "no cJSON in the component manager's cache (run one idf.py build)"
mkdir -p managed_components && ln -sfn "$CJ" managed_components/espressif__cjson
(
  . "$IDF_EXPORT" > /dev/null 2>&1 || { echo "verify_commit: $IDF_EXPORT failed" > "$LOG"; exit 1; }
  export PATH="$(dirname "$PY"):$PATH"
  ${TIMEOUT:+$TIMEOUT 3000} "$PY" -m unittest discover -v -s tests -p 'test_*.py' > "$LOG" 2>&1
); rc=$?
echo "commit $(git log --oneline -1 | cut -c1-70): rc=$rc (logs in $LOGS)"
grep -a -E '^(Ran|OK|FAILED)' "$LOG" | tail -2
grep -a -E '^(FAIL|ERROR):|^verify_commit:' "$LOG" | head -10
grep -a -E '\.\.\. skipped' "$LOG" | sed -E 's/.*\(([^)]*)\).*skipped (.*)/  skip \1: \2/'
skips=$(grep -a -c -E '\.\.\. skipped' "$LOG")
if [ "$rc" -eq 0 ] && [ "$skips" -gt 0 ]; then
  echo "verify_commit: $skips skipped: not a full run"
  rc=1
fi
exit $rc
