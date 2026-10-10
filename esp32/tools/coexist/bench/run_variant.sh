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
# Flash one bench build and stream its log detached.
# Usage: run_variant.sh BUILD_DIR PORT LOG [SECS]
set -u
cd "$(dirname "$0")/../../.."
. "${IDF_EXPORT:-$HOME/.espressif/esp-idf-v6.0.1/export.sh}" >/dev/null 2>&1
idf.py -B "$1" -p "$2" flash > "$3.flash" 2>&1 || { echo "flash failed: see $3.flash"; exit 1; }
nohup python -u tools/coexist/bench/serial_log.py "$2" "${4:-900}" > "$3" 2>&1 &
echo "flashed $1; logging to $3"
