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

# Build or flash Home Link for one board:
#   tools/muse/board.sh build|flash <s3|s3n|aipi|box3|c6|watcher|sticks3|plus2|cardputer-adv|stopwatch|cores3|jc3248w535> [serial|port]
# Build log: /tmp/muse_build_<board>.log. flash finds the board's port by its
# USB device (tools/muse/ports.py); with several of a kind attached, pass the
# one's USB serial number (the MAC on native USB) or its port. Flashing from a
# sandboxed agent needs the sandbox disabled (serial port access).
# MUSE_BENCH=1 adds devices/sdkconfig.muse-bench (screenshots) and builds and
# flashes in build-muse-<profile>-bench/, so neither build's sdkconfig hides
# the other's.
set -uo pipefail
cmd=${1:?build|flash}; board=${2:?s3|s3n|aipi|box3|c6|watcher|sticks3|plus2|cardputer-adv|stopwatch|cores3|jc3248w535}
root=$(cd "$(dirname "$0")/../.." && pwd)
case $board in
    s3)      profile=waveshare-s3-175c;    target=esp32s3 ;;
    s3n)     profile=waveshare-s3-175;     target=esp32s3 ;;
    aipi)    profile=aipi;                 target=esp32s3 ;;
    box3)    profile=espressif-box-3;       target=esp32s3 ;;
    c6)      profile=waveshare-c6-18;      target=esp32c6 ;;
    # Its CH342 bridge drops bytes when esptool sends a whole packet at once,
    # so pace the writes (paced_esptool.py) at the baud they were tested at.
    watcher) profile=sensecap-watcher;     target=esp32s3; baud=115200; paced=1 ;;
    cardputer-adv) profile=m5stack-cardputer-adv; target=esp32s3 ;;
    sticks3) profile=m5stack-sticks3;      target=esp32s3 ;;
    stopwatch) profile=m5stack-stopwatch;  target=esp32s3 ;;
    cores3)  profile=m5stack-cores3;       target=esp32s3 ;;
    jc3248w535) profile=guition-jc3248w535; target=esp32s3 ;;
    # Its CH9102 USB-UART bridge drops out above 230400 baud.
    plus2)   profile=m5stack-stickc-plus2; target=esp32; baud=230400 ;;
    *) echo "unknown board $board"; exit 2 ;;
esac
baud=${baud:-460800}
if [ -n "${IDF_EXPORT:-}" ]; then
    . "$IDF_EXPORT" >/dev/null 2>&1
elif ! command -v idf.py >/dev/null 2>&1; then
    for d in "$HOME/.espressif/esp-idf-v6.0.1" "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.1" "$HOME/esp/esp-idf-v6" "$HOME/esp/esp-idf"; do
        [ -n "$d" ] && [ -f "$d/export.sh" ] && { . "$d/export.sh" >/dev/null 2>&1; break; }
    done
fi
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found; activate ESP-IDF v6.0.1 first" >&2; exit 1; }
cd "$root"
B=build-muse-$profile
defaults="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$profile"
if [ -n "${MUSE_BENCH:-}" ]; then
    B=$B-bench; defaults="$defaults;devices/sdkconfig.muse-bench"
fi
log=/tmp/muse_build_$board.log
if [ "$cmd" = build ]; then
    # Boards share managed_components/ with each other and with the Link build,
    # so build one at a time. Each resolves a different dependency set, and the
    # component manager fails partway through pruning the last one's (lvgl), so
    # start clean and leave nothing behind. Something on the host (an indexer)
    # can recreate files mid-delete, so retry.
    clean() { for _ in 1 2 3; do rm -rf managed_components dependencies.lock 2>/dev/null && return; sleep 1; done; }
    clean
    idf.py -B $B -DIDF_TARGET=$target -DSDKCONFIG=$B/sdkconfig \
        -DSDKCONFIG_DEFAULTS="$defaults" \
        build > $log 2>&1; rc=$?
    clean
    grep -E "error:|undefined reference" $log | head -20
    tail -3 $log
    exit $rc
fi
port=$(python "$root/tools/muse/ports.py" $board ${3:-}) || exit 1
esptool="-m esptool"
[ -n "${paced:-}" ] && esptool="$root/tools/muse/paced_esptool.py"
cd $B && python $esptool --chip $target -p $port -b $baud --before default-reset --after hard-reset write-flash "@flash_args" 2>&1 | tail -3
