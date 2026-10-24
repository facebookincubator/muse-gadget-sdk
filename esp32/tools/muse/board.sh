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
#   tools/muse/board.sh build|flash <s3|s3n|s3-216|mosaico|aipi|box3|c6|c6-206|watcher|sticks3|plus2|cardputer-adv|stopwatch|cores3|core2|fnk0104b|jc3248w535|lcd7|lcd128|vn183|ai-passport|echoear> [serial|port]
# Build log: /tmp/muse_build_<board>.log. flash finds the board's port by its
# USB device (tools/muse/ports.py); with several of a kind attached, pass the
# one's USB serial number (the MAC on native USB) or its port. Flashing from a
# sandboxed agent needs the sandbox disabled (serial port access).
# MUSE_BENCH=1 adds devices/sdkconfig.muse-bench (screenshots) and builds and
# flashes in build-muse-<profile>-bench/, so neither build's sdkconfig hides
# the other's.
# MUSE_VIDEO=1 adds devices/sdkconfig.muse-video (the phone camera over
# Wi-Fi, on boards whose panel driver has draw_frame) in build-muse-<profile>-video/.
# mosaico (Espressif ESP-Mosaico, ESP32-S31) builds with ESP-IDF master
# (IDF_EXPORT_S31) and Espressif's esp-mosaico-utils (MOSAICO_UTILS), and
# flashes through the board's Vibe Mode firmware with their mosaico.py: an
# ESP-Iris System Update bundle, never esptool (devices/README.md).
set -uo pipefail
cmd=${1:?build|flash}; board=${2:?s3|s3n|s3-216|185c|mosaico|aipi|box3|c6|c6-206|watcher|sticks3|plus2|cardputer-adv|stopwatch|cores3|core2|fnk0104b|jc3248w535|lcd7|lcd128|vn183|ai-passport|echoear}
root=$(cd "$(dirname "$0")/../.." && pwd)
case $board in
    s3)      profile=waveshare-s3-175c;    target=esp32s3 ;;
    s3n)     profile=waveshare-s3-175;     target=esp32s3 ;;
    s3-216)  profile=waveshare-s3-216;     target=esp32s3 ;;
    185c)    profile=waveshare-s3-185c;    target=esp32s3 ;;
    # ESP-Mosaico: ESP-IDF master (IDF_EXPORT_S31, default ~/.espressif/esp-idf-s31).
    mosaico) profile=esp-mosaico;          target=esp32s31 ;;
    aipi)    profile=aipi;                 target=esp32s3 ;;
    echoear) profile=ostb-echoear-2st; target=esp32s3 ;;
    box3)    profile=espressif-box-3;       target=esp32s3 ;;
    c6)      profile=waveshare-c6-18;      target=esp32c6 ;;
    c6-206)  profile=waveshare-c6-206;     target=esp32c6 ;;
    # Its CH342 bridge drops bytes when esptool sends a whole packet at once,
    # so pace the writes (paced_esptool.py) at the baud they were tested at.
    watcher) profile=sensecap-watcher;     target=esp32s3; baud=115200; paced=1 ;;
    cardputer-adv) profile=m5stack-cardputer-adv; target=esp32s3 ;;
    sticks3) profile=m5stack-sticks3;      target=esp32s3 ;;
    stopwatch) profile=m5stack-stopwatch;  target=esp32s3 ;;
    cores3)  profile=m5stack-cores3;       target=esp32s3 ;;
    fnk0104b) profile=fnk0104b;            target=esp32s3 ;;
    jc3248w535) profile=guition-jc3248w535; target=esp32s3 ;;
    lcd7) profile=waveshare-s3-lcd7; target=esp32s3 ;;
    lcd128) profile=waveshare-s3-lcd128; target=esp32s3 ;;
    lcd154) profile=waveshare-s3-lcd154; target=esp32s3 ;;
    vn183)   profile=vn-s3-183;            target=esp32s3 ;;
    # Its CH9102 USB-UART bridge drops out above 230400 baud.
    plus2)   profile=m5stack-stickc-plus2; target=esp32; baud=230400 ;;
    # The Core2's bridge is a CP2104 or a CH9102F: 230400 is safe on both.
    core2)   profile=m5stack-core2; target=esp32; baud=230400 ;;
    ai-passport) profile=ai-passport;      target=esp32c3 ;;
    *) echo "unknown board $board"; exit 2 ;;
esac
baud=${baud:-460800}
if [ "$target" = esp32s31 ]; then
    # The S31 isn't in v6.0.1: Espressif's ESP-Mosaico pins master at 7b9cc1ac.
    IDF_EXPORT=${IDF_EXPORT_S31:-$HOME/.espressif/esp-idf-s31/export.sh}
    preview=--preview
    # Espressif's esp-mosaico-utils: ESP-Iris, which the board's Vibe Mode
    # firmware installs apps with (devices/README.md, ESP-Mosaico).
    MOSAICO_UTILS=${MOSAICO_UTILS:-$HOME/mosaico/esp-mosaico-vibe/submodule/esp-mosaico-utils}
    [ -d "$MOSAICO_UTILS/ESP-Iris/components/esp_iris" ] || { echo "esp-mosaico-utils not found at $MOSAICO_UTILS (set MOSAICO_UTILS)" >&2; exit 1; }
    iris="-DMOSAICO_UTILS=$MOSAICO_UTILS"
fi
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
if [ -n "${MUSE_VIDEO:-}" ]; then
    B=$B-video; defaults="$defaults;devices/sdkconfig.muse-video"
fi
if [ -n "${MUSE_BENCH:-}" ]; then
    B=$B-bench; defaults="$defaults;devices/sdkconfig.muse-bench"
fi
[ -n "${iris:-}" ] && defaults="$defaults;devices/sdkconfig.muse-$profile-iris"
log=/tmp/muse_build_$board.log
# ESP-Mosaico: its Vibe Mode firmware installs an ESP-Iris System Update
# bundle (the app and this partition table; never the bootloader or the
# retained first 2 MB). Built with Espressif's tools, in a venv of their own.
mosaico_bundle() {
    local venv=${MOSAICO_IRIS_VENV:-$HOME/.cache/muse-mosaico-iris} tools="$MOSAICO_UTILS/ESP-Iris/components/esp_iris/tools"
    if [ ! -x "$venv/bin/python" ]; then
        python3 -m venv "$venv" && "$venv/bin/pip" -q install -r "$tools/requirements.lock" || return 1
    fi
    rm -rf "$B/system-update"
    "$venv/bin/python" "$MOSAICO_UTILS/mosaico-tools/tools/prepare_system_update.py" \
        --partition-csv partitions_mosaico.csv --partition-table $B/partition_table/partition-table.bin \
        --application $B/muse-gadget.bin --stage-dir $B/system-update --release 1.0.0 >> $log 2>&1 &&
    "$venv/bin/python" "$tools/esp_iris.py" bundle build $B/system-update/manifest.json \
        --component-root $B/system-update --output $B/muse-gadget-system-update.irisfw >> $log 2>&1 || { tail -5 $log; return 1; }
    echo "System Update bundle: $B/muse-gadget-system-update.irisfw"
}
if [ "$cmd" = build ]; then
    # Boards share managed_components/ with each other and with the Link build,
    # so build one at a time. Each resolves a different dependency set, and the
    # component manager fails partway through pruning the last one's (lvgl), so
    # start clean and leave nothing behind. Something on the host (an indexer)
    # can recreate files mid-delete, so retry.
    clean() { for _ in 1 2 3; do rm -rf managed_components dependencies.lock 2>/dev/null && return; sleep 1; done; }
    clean
    idf.py ${preview:-} -B $B -DIDF_TARGET=$target ${iris:-} -DSDKCONFIG=$B/sdkconfig \
        -DSDKCONFIG_DEFAULTS="$defaults" \
        build > $log 2>&1; rc=$?
    [ $rc -eq 0 ] && [ -n "${iris:-}" ] && { mosaico_bundle || rc=$?; }
    clean
    grep -E "error:|undefined reference" $log | head -20
    tail -3 $log
    exit $rc
fi
if [ -n "${iris:-}" ]; then
    # Through the board's Vibe Mode firmware, with Espressif's product command:
    # it switches the running app to Vibe Mode over ESP-Iris (or uses Vibe Mode
    # if the board is in it), installs the bundle into ota_0 and checks the app
    # comes up healthy. Never esptool: `factory` holds Vibe Mode.
    # mosaico.py runs a Gateway per application project in its workspace:
    # this directory is that project (projects/muse_gadget, a link here).
    ws=${MOSAICO_WORKSPACE:-$(cd "$MOSAICO_UTILS/../.." && pwd)}
    [ -e "$ws/projects/muse_gadget" ] || { mkdir -p "$ws/projects" && ln -s "$root" "$ws/projects/muse_gadget"; }
    cd "$ws" && exec python3 mosaico.py iris system-update --bundle "$root/$B/muse-gadget-system-update.irisfw" ${3:+--device-id $3}
fi
# Experimental LCD 1.54: the vendor publishes both UART and USJ console
# configurations. Never auto-select the only USB device and risk flashing a
# different attached S3. Require an explicit /dev/... port for this board.
if [ "$board" = lcd154 ]; then
    if [[ "${3:-}" != /dev/* ]]; then
        echo "lcd154 is experimental: pass its verified /dev/... serial port explicitly" >&2
        exit 2
    fi
fi
port=$(python "$root/tools/muse/ports.py" $board ${3:-}) || exit 1
esptool="-m esptool"
[ -n "${paced:-}" ] && esptool="$root/tools/muse/paced_esptool.py"
cd $B && python $esptool --chip $target -p $port -b $baud --before default-reset --after hard-reset write-flash "@flash_args" 2>&1 | tail -3
