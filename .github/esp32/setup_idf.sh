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

# Install ESP-IDF natively for the build jobs, as the espressif/idf image does:
# the release's own checkout, its install.sh and its pinned cmake.
#   setup_idf.sh VERSION COMMIT TARGETS     e.g. setup_idf.sh v6.0.3 76f5dedd... esp32,esp32s3
# The checkout must be COMMIT (the release tag's commit, pinned in the
# workflow). IDF_PATH and IDF_TOOLS_PATH come from the environment. A restored
# cache (actions/cache) skips the clone and the install.
set -euo pipefail
version=${1:?version}; commit=${2:?commit}; targets=${3:?targets}
: "${IDF_PATH:?}" "${IDF_TOOLS_PATH:?}"
s=$(date +%s)
marker="$IDF_PATH/.ci-installed-$commit"
if [ -f "$marker" ] && [ "$(git -C "$IDF_PATH" rev-parse HEAD)" = "$commit" ]; then
    echo "ESP-IDF $version ($commit) from the cache"
else
    rm -rf "$IDF_PATH" "$IDF_TOOLS_PATH"
    # Each download has a time limit: a stalled one fails the job instead of using it up.
    timeout 900 git clone -q --depth 1 --branch "$version" --recurse-submodules --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
    got=$(git -C "$IDF_PATH" rev-parse HEAD)
    [ "$got" = "$commit" ] || { echo "::error::ESP-IDF $version is $got, not the pinned $commit"; exit 1; }
    # The image's cmake too (install.sh leaves it out on Linux).
    { timeout 1200 "$IDF_PATH/install.sh" "$targets" &&
      timeout 300 "$IDF_PATH/tools/idf_tools.py" --non-interactive install cmake; } \
        > /tmp/idf-install.log 2>&1 || { tail -40 /tmp/idf-install.log; exit 1; }
    rm -rf "$IDF_TOOLS_PATH/dist"   # the downloaded archives: installed already, not cached
    touch "$marker"
    echo "ESP-IDF $version ($commit) installed for $targets"
fi
# The Python packages the build uses (install.sh takes them from PyPI within ESP-IDF's constraints).
for py in "$IDF_TOOLS_PATH"/python_env/*/bin/python; do
    [ -x "$py" ] && "$py" -m pip freeze 2>/dev/null | grep -iE '^(esptool|esp-idf-|idf-component-manager)' || true
done
echo "setup: $(( $(date +%s) - s )) s"
