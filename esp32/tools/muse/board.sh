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
#   tools/muse/board.sh build|flash <k10|s3|s3-gadget|s3-video|s3n|s3-18|s3-216|s3-206|185b|185c|143c|143|146|lilygo175|mosaico|mosaico-video|aipi|atk-box|atk-box2|box3|c6|c6-206|c6ble|watcher|watcher-gadget|watcher-video|sticks3|crowpanel-43|plus2|cardputer-adv|atoms3|stopwatch|stopwatch-video|triviapod|xiao147|cores3|cores3lite|stackchan|core2|fnk0104b|jc3248w535|lcd7|lcd128|lcd349|spotpear|vn183|ai-passport|echoear|p4lcd35|rorolee|lcd19|smartring-plus|s3lcd35> [serial|port]
# Build log: /tmp/muse_build_<board>.log. flash finds the board's port by its
# USB device (tools/muse/ports.py); with several of a kind attached, pass the
# one's USB serial number (the MAC on native USB) or its port. Flashing from a
# sandboxed agent needs the sandbox disabled (serial port access).
# MUSE_BENCH=1 adds devices/sdkconfig.muse-bench (screenshots) and builds and
# flashes in build-muse-<profile>-bench/, so neither build's sdkconfig hides
# the other's.
# MUSE_HID=1 adds devices/sdkconfig.muse-hid (HID over GATT beside musegadgets,
# with bonding; BLE video builds) in build-muse-<profile>-ble-hid/.
# MUSE_VIDEO=1 adds devices/sdkconfig.muse-video (the phone camera over
# Wi-Fi, on boards whose panel driver has draw_frame) in build-muse-<profile>-video/.
# MUSE_CAMERA=1 adds devices/sdkconfig.muse-<profile>-camera (camera.capture,
# for a board that has one) in build-muse-<profile>-camera/.
# MUSE_WAKEWORD=1 adds devices/sdkconfig.muse-wakeword (the "Hey Muse" wake
# word) in build-muse-<profile>-wakeword/.
# MUSE_POWER_TEST=1 adds the board's diagnostic overlay and a -power-test suffix.
# MUSE_APPS=1 adds devices/sdkconfig.muse-apps (WebAssembly apps: app.run) in
# build-muse-<profile>-apps/.
# MUSE_MIRROR=1 adds the cable mirror in build-muse-<profile>-mirror/.
# mosaico (Espressif ESP-Mosaico, ESP32-S31) builds with ESP-IDF master
# (IDF_EXPORT_S31) and Espressif's esp-mosaico-utils (MOSAICO_UTILS), and
# flashes through the board's Vibe Mode firmware with their mosaico.py: an
# ESP-Iris System Update bundle, never esptool (devices/README.md).
set -uo pipefail
umask 077   # build outputs, logs and images hold the SDK token: private
cmd=${1:?build|flash}; board=${2:?k10|s3|s3-gadget|s3-video|s3n|s3-18|s3-216|s3-206|185b|185c|143c|143|146|lilygo175|mosaico|mosaico-video|aipi|atk-box|atk-box2|box3|c6|c6-206|c6ble|watcher|watcher-gadget|watcher-video|sticks3|crowpanel-43|plus2|cardputer-adv|atoms3|stopwatch|stopwatch-video|triviapod|xiao147|cores3|cores3lite|stackchan|core2|fnk0104b|jc3248w535|lcd7|lcd128|lcd349|spotpear|vn183|ai-passport|echoear|p4lcd35|rorolee|lcd19|smartring-plus|s3lcd35}
root=$(cd "$(dirname "$0")/../.." && pwd)
case $board in
    s3)      profile=waveshare-s3-175c;    target=esp32s3 ;;
    # A board's Muse Gadget profile: devices/sdkconfig.muse-<profile>-gadget after its own.
    s3-gadget) profile=waveshare-s3-175c; target=esp32s3; gadget=1 ;;
    s3-video) profile=waveshare-s3-175c;  target=esp32s3; gadget=1; extra=video-ble ;;   # musegadgets BLE + phone camera over BLE
    s3n)     profile=waveshare-s3-175;     target=esp32s3 ;;
    s3-18)   profile=waveshare-s3-18;      target=esp32s3 ;;
    s3-216)  profile=waveshare-s3-216;     target=esp32s3 ;;
    s3-206)  profile=waveshare-s3-206;     target=esp32s3 ;;
    185b)    profile=waveshare-s3-185b;    target=esp32s3 ;;
    185c)    profile=waveshare-s3-185c;    target=esp32s3 ;;
    143c)    profile=waveshare-s3-143c;    target=esp32s3 ;;
    143)     profile=waveshare-s3-143;     target=esp32s3 ;;
    lilygo175) profile=lilygo-s3-amoled-175; target=esp32s3 ;;
    146)     profile=waveshare-s3-146;     target=esp32s3 ;;
    # ESP-Mosaico: ESP-IDF master (IDF_EXPORT_S31, default ~/.espressif/esp-idf-s31).
    mosaico) profile=esp-mosaico;          target=esp32s31 ;;
    mosaico-video) profile=esp-mosaico;    target=esp32s31; extra=video-ble ;;   # musegadgets BLE + phone camera over BLE
    k10)     profile=dfrobot-unihiker-k10; target=esp32s3 ;;
    aipi)    profile=aipi;                 target=esp32s3 ;;
    atk-box) profile=atk-dnesp32s3-box;     target=esp32s3 ;;
    atk-box2) profile=atk-dnesp32s3-box2;   target=esp32s3 ;;
    echoear) profile=ostb-echoear-2st; target=esp32s3 ;;
    box3)    profile=espressif-box-3;       target=esp32s3 ;;
    c6)      profile=waveshare-c6-18;      target=esp32c6 ;;
    c6-206)  profile=waveshare-c6-206;     target=esp32c6 ;;
    # The C6 as a BLE-only musegadgets gadget: its overlay loads after the C6's.
    c6ble)   profile=waveshare-c6-18;      target=esp32c6; extra=waveshare-c6-18-ble ;;
    # Its CH342 bridge drops bytes when esptool sends a whole packet at once,
    # so pace the writes (paced_esptool.py) at the baud they were tested at.
    watcher) profile=sensecap-watcher;     target=esp32s3; baud=115200; paced=1 ;;
    watcher-gadget) profile=sensecap-watcher; target=esp32s3; baud=115200; paced=1; gadget=1 ;;
    watcher-video) profile=sensecap-watcher; target=esp32s3; baud=115200; paced=1; gadget=1; extra=video-ble ;;   # musegadgets BLE + phone camera and UI streams over BLE
    cardputer-adv) profile=m5stack-cardputer-adv; target=esp32s3 ;;
    atoms3) profile=m5stack-atoms3; target=esp32s3 ;;
    sticks3) profile=m5stack-sticks3;      target=esp32s3 ;;
    crowpanel-43) profile=crowpanel-advance-43; target=esp32s3 ;;
    stopwatch) profile=m5stack-stopwatch;  target=esp32s3 ;;
    triviapod) profile=m5stack-triviapod; target=esp32s3 ;;
    stopwatch-video) profile=m5stack-stopwatch; target=esp32s3; extra=video-ble ;;   # musegadgets BLE + phone camera over BLE
    xiao147) profile=seeed-xiao-s3-147;    target=esp32s3 ;;
    cores3)  profile=m5stack-cores3;       target=esp32s3 ;;
    cores3lite) profile=m5stack-cores3lite; target=esp32s3 ;;
    stackchan) profile=m5stack-stackchan;  target=esp32s3 ;;
    fnk0104b) profile=fnk0104b;            target=esp32s3 ;;
    jc3248w535) profile=guition-jc3248w535; target=esp32s3 ;;
    lcd7) profile=waveshare-s3-lcd7; target=esp32s3 ;;
    spotpear) profile=spotpear-esp32-ai-128; target=esp32s3 ;;
    lcd128) profile=waveshare-s3-lcd128; target=esp32s3 ;;
    lcd349) profile=waveshare-s3-lcd349; target=esp32s3 ;;
    vn183)   profile=vn-s3-183;            target=esp32s3 ;;
    p4lcd35) profile=waveshare-p4-lcd35;   target=esp32p4 ;;
    rorolee) profile=deotaland-rorolee;    target=esp32s3 ;;
    smartring-plus) profile=viewe-smartring-plus; target=esp32s3 ;;
    lcd19)   profile=waveshare-s3-lcd19;   target=esp32s3 ;;
    s3lcd35) profile=waveshare-s3-lcd-35;  target=esp32s3 ;;
    # Its CH9102 USB-UART bridge drops out above 230400 baud.
    plus2)   profile=m5stack-stickc-plus2; target=esp32; baud=230400 ;;
    # The Core2's bridge is a CP2104 or a CH9102F: 230400 is safe on both.
    core2)   profile=m5stack-core2; target=esp32; baud=230400 ;;
    ai-passport) profile=ai-passport;      target=esp32c3 ;;
    *) echo "unknown board $board"; exit 2 ;;
esac
baud=${baud:-460800}
if [ "$target" = esp32s31 ]; then
    # The S31 isn't in v6.0.3: Espressif's ESP-Mosaico pins master at 7b9cc1ac.
    IDF_EXPORT=${IDF_EXPORT_S31:-$HOME/.espressif/esp-idf-s31/export.sh}
    preview=--preview
    # Espressif's esp-mosaico-utils: ESP-Iris, which the board's Vibe Mode
    # firmware installs apps with (devices/README.md, ESP-Mosaico).
    MOSAICO_UTILS=${MOSAICO_UTILS:-$HOME/mosaico/esp-mosaico-vibe/submodule/esp-mosaico-utils}
    [ -d "$MOSAICO_UTILS/ESP-Iris/components/esp_iris" ] || { echo "esp-mosaico-utils not found at $MOSAICO_UTILS (set MOSAICO_UTILS)" >&2; exit 1; }
    iris="-DMOSAICO_UTILS=$MOSAICO_UTILS"
    # Vibe Mode 0.2 tools talk only to 0.2 boards, whose layout keeps Vibe Mode
    # in `vibe_mode` (app/test) instead of `factory`: build for the checkout's
    # generation, in a directory of its own.
    [ -d "$MOSAICO_UTILS/esp-mosaico-vibe-mode" ] && vibe2=0.2
    echo "ESP-Mosaico: building for Vibe Mode ${vibe2:-0.1} (utils at $MOSAICO_UTILS)" >&2
fi
if [ -n "${IDF_EXPORT:-}" ]; then
    . "$IDF_EXPORT" >/dev/null 2>&1
elif ! command -v idf.py >/dev/null 2>&1; then
    for d in "$HOME/.espressif/esp-idf-v6.0.3" "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.3" "$HOME/esp/esp-idf-v6" "$HOME/esp/esp-idf"; do
        [ -n "$d" ] && [ -f "$d/export.sh" ] && { . "$d/export.sh" >/dev/null 2>&1; break; }
    done
fi
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found; activate ESP-IDF v6.0.3 first" >&2; exit 1; }
cd "$root"
if [ -n "${extra:-}" ]; then
    B=build-muse-$profile-ble
else
    B=build-muse-$profile${gadget:+-gadget}
fi
defaults="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$profile${gadget:+;devices/sdkconfig.muse-$profile-gadget}${extra:+;devices/sdkconfig.muse-$extra}"
# A board's own settings for one extra profile, when it has them
# (devices/sdkconfig.muse-sensecap-watcher-video-ble: watcher-video only).
[ -n "${extra:-}" ] && [ -f "devices/sdkconfig.muse-$profile-$extra" ] && defaults="$defaults;devices/sdkconfig.muse-$profile-$extra"
if [ -n "${MUSE_VIDEO:-}" ]; then
    # Wi-Fi video and BLE audio don't fit the S3's internal RAM together.
    [ -z "${gadget:-}" ] || { echo "MUSE_VIDEO is video over Wi-Fi: build $board's Wi-Fi profile (video over BLE: the -video alias)" >&2; exit 2; }
    B=$B-video; defaults="$defaults;devices/sdkconfig.muse-video"
fi
if [ -n "${MUSE_CAMERA:-}" ]; then
    [ -f "devices/sdkconfig.muse-$profile-camera" ] || { echo "no camera overlay for $board" >&2; exit 1; }
    B=$B-camera; defaults="$defaults;devices/sdkconfig.muse-$profile-camera"
fi
if [ -n "${MUSE_WAKEWORD:-}" ]; then
    B=$B-wakeword; defaults="$defaults;devices/sdkconfig.muse-wakeword"
fi
if [ -n "${MUSE_HID:-}" ]; then
    B=$B-hid; defaults="$defaults;devices/sdkconfig.muse-hid"
fi
if [ -n "${MUSE_APPS:-}" ]; then
    B=$B-apps; defaults="$defaults;devices/sdkconfig.muse-apps"
fi
if [ -n "${MUSE_MIRROR:-}" ]; then
    B=$B-mirror; defaults="$defaults;devices/sdkconfig.muse-mirror"
fi
if [ "${MUSE_BENCH:-}" = 1 ]; then   # only 1: MUSE_BENCH=0 is no bench
    B=$B-bench; defaults="$defaults;devices/sdkconfig.muse-bench"
fi
[ -n "${iris:-}" ] && defaults="$defaults;devices/sdkconfig.muse-$profile-iris"
if [ -n "${vibe2:-}" ]; then
    B=$B-vibe2; defaults="$defaults;devices/sdkconfig.muse-$profile-vibe2"
fi
if [ -n "${MUSE_POWER_TEST:-}" ]; then
    [ -z "${MUSE_VIDEO:-}" ] && [ "${MUSE_BENCH:-}" != 1 ] || { echo "power-test cannot be combined with video/bench" >&2; exit 2; }
    power_profile=$profile
    case $board in watcher|watcher-gadget) power_profile=watcher ;; esac
    [ -f "devices/sdkconfig.muse-$power_profile-power-test" ] || { echo "no power-test overlay for $board" >&2; exit 2; }
    B=$B-power-test; defaults="$defaults;devices/sdkconfig.muse-$power_profile-power-test"
fi
log=/tmp/muse_build_$board.log
# ESP-Mosaico: its Vibe Mode firmware installs an ESP-Iris System Update
# bundle (the app and its partition table; never the bootloader or the
# retained first 2 MB). Built with Espressif's tools, in a venv of their own.
# The app goes into ota_0, the only slot Muse ever writes: ESP-IDF's size check
# also measures it against `factory` (Vibe Mode's 1.75 MB slot, never written
# here) and only warns, so check the slot that matters and fail if it's full.
# The partition table the build used: partitions_mosaico.csv, or the one the
# vibe2 overlay names.
mosaico_parts() { sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\(.*\)"$/\1/p' $B/sdkconfig; }
mosaico_fits() {
    local app size free parts
    parts=$(mosaico_parts)
    app=$(wc -c < $B/muse-gadget.bin) || return 1
    # The app slot by type and subtype (Vibe Mode 0.2's table names it main_app).
    size=$(awk -F, '/^ *#/ { next } { for (i = 2; i <= 5; i++) gsub(/[ \t]/, "", $i) }
        $2 == "app" && $3 == "ota_0" { print $5 }' $parts)
    [ -n "$size" ] || { echo "no app,ota_0 partition in $parts"; return 1; }
    [ "$(printf '%s\n' "$size" | wc -l)" -eq 1 ] || { echo "more than one app,ota_0 partition in $parts"; return 1; }
    size=$((size)); free=$((size - app))
    if [ $free -lt 0 ]; then
        echo "muse-gadget.bin ($app bytes) is too big for ota_0 in $parts ($size bytes)"; return 1
    fi
    echo "muse-gadget.bin $app bytes, ota_0 $size bytes: $((free * 100 / size))% free" \
        "(ESP-IDF's warning about 'factory' is Vibe Mode's slot, which Muse never writes)"
}
mosaico_bundle() {
    local venv=${MOSAICO_IRIS_VENV:-$HOME/.cache/muse-mosaico-iris} tools="$MOSAICO_UTILS/ESP-Iris/components/esp_iris/tools"
    if [ ! -x "$venv/bin/python" ]; then
        python3 -m venv "$venv" && "$venv/bin/pip" -q install -r "$tools/requirements.lock" || return 1
    fi
    local parts; parts=$(mosaico_parts)
    rm -rf "$B/system-update"
    "$venv/bin/python" "$MOSAICO_UTILS/mosaico-tools/tools/prepare_system_update.py" \
        --partition-csv "$parts" --partition-table $B/partition_table/partition-table.bin \
        --application $B/muse-gadget.bin --stage-dir $B/system-update --release "$(tr -d '[:space:]' < version.txt)" >> $log 2>&1 &&
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
    [ $rc -eq 0 ] && [ -n "${iris:-}" ] && { mosaico_fits && mosaico_bundle || rc=1; }
    # BLE builds that take updates over BLE (MCUmgr SMP): the same image in an
    # MCUboot envelope, for the Muse app or tools/mg_ble_client.py dfu.
    if [ $rc -eq 0 ] && grep -q '^CONFIG_MUSE_GADGET_BLE_SMP=y' $B/sdkconfig; then
        python3 tools/muse/mcuboot_wrap.py $B/muse-gadget.bin || rc=1
    fi
    clean
    grep -E "error:|undefined reference" $log | head -20
    tail -3 $log
    exit $rc
fi
if [ -n "${iris:-}" ]; then
    # Through the board's Vibe Mode firmware, with Espressif's product command:
    # it switches the running app to Vibe Mode over ESP-Iris (or uses Vibe Mode
    # if the board is in it), installs the bundle into the app slot and checks
    # the app comes up healthy. Never esptool: the first 2 MB hold Vibe Mode.
    # mosaico.py runs a Gateway per application project in its workspace:
    # this directory is that project (projects/muse_gadget, a link here).
    ws=${MOSAICO_WORKSPACE:-$(cd "$MOSAICO_UTILS/../.." && pwd)}
    [ -e "$ws/projects/muse_gadget" ] || { mkdir -p "$ws/projects" && ln -s "$root" "$ws/projects/muse_gadget"; }
    cd "$ws" && exec python3 mosaico.py iris system-update --bundle "$root/$B/muse-gadget-system-update.irisfw" ${3:+--device-id $3}
fi
port=$(python "$root/tools/muse/ports.py" $board ${3:-}) || exit 1
esptool="-m esptool"
[ -n "${paced:-}" ] && esptool="$root/tools/muse/paced_esptool.py"
# Flash safety: the write never resets the chip by
# itself (esptool resets in its teardown even after a failed write, which
# would boot a half-written image); every region must come back "Hash of data
# verified" (esptool checks each one's MD5 on the chip) and esptool must exit
# 0; only then reset, then expect a boot within 60 s. The whole log is kept.
flog=$(dirname "$log")/muse_flash_$board.log
regions=$(awk 'NR>1 && NF>=2 {n++} END {print n+0}' "$B/flash_args")
cd $B && python $esptool --chip $target -p $port -b $baud --before default-reset --after no-reset write-flash "@flash_args" > "$flog" 2>&1
rc=$?
verified=$(grep -c "Hash of data verified" "$flog")
tail -3 "$flog"
if [ $rc -ne 0 ] || [ "$verified" -ne "$regions" ]; then
    echo "FLASH FAILED (esptool exit $rc, $verified of $regions regions verified): the chip was NOT reset." >&2
    echo "Don't power-cycle it; the ROM loader still answers: retry the flash. Log: $flog" >&2
    exit 1
fi
# The reset that boots the new image. On the S3 it's esptool's watchdog reset:
# over the chip's own USB-Serial-JTAG a hard reset re-enters the ROM download
# mode it was just in, and through the Watcher's CH342 the control lines held
# BOOT low across it. Measured on the W175, the Watcher and the StopWatch:
# that reset boots the app as a power-on (raw 0x01), so the boot guard
# starts fresh and counts no crash (test_muse_bootguard: a flasher's reset).
# The boot check below fails if the boot ever reads as a crash.
# Elsewhere it's esptool's hard reset from a fresh connection (default-reset first):
# on the C6's USB-Serial-JTAG a hard reset from --before no-reset left it in
# download mode, while default-reset then hard-reset boots the app.
case $target in esp32s3) before=no-reset; after=watchdog-reset ;; *) before=default-reset; after=hard-reset ;; esac
# If that connection finds no ROM (the Watcher through its CH342: "No serial
# data received" after "Staying in bootloader"), connect once more with the control
# lines' reset into the ROM, then the same watchdog reset out of it: still a
# power-on boot, still no crash counted.
python $esptool --chip $target -p $port -b $baud --before $before --after $after read-mac >> "$flog" 2>&1 ||
    python $esptool --chip $target -p $port -b $baud --before default-reset --after $after read-mac >> "$flog" 2>&1 ||
    { echo "flashed and verified, but the reset failed (log: $flog): reset it by hand" >&2; exit 1; }
# The boot: the console says "link.main: <product> starting" within 60 s, read without
# touching DTR/RTS (no second reset).
python3 - "$port" <<'PY'
import os, re, sys, termios, time
port = sys.argv[1]
started = False
heartbeat_at = None
deadline = time.monotonic() + 60
buf = b""
fd = None
while time.monotonic() < deadline:
    try:
        if fd is None:
            fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(fd)
            a[2] &= ~termios.HUPCL
            a[4] = a[5] = termios.B115200   # a bridge board's console UART; ignored over USB-Serial-JTAG
            termios.tcsetattr(fd, termios.TCSANOW, a)
        c = os.read(fd, 4096)
        if c:
            buf = (buf + c)[-8192:]
            if b"link.main: " in buf and b" starting" in buf.split(b"link.main: ", 1)[1][:80]:
                started = True
            # The C6 prints "starting" before the host has the re-enumerated port open
            # again; its heartbeat (every 5 s, then 30 s) proves the app runs too.
            if b"link.heartbeat: hb t=" in buf:
                started = True
                heartbeat_at = heartbeat_at or time.monotonic()
            # How the boot began (muse_restart): a flash must never read as a crash,
            # which the boot guard would count toward turning Muse off.
            m = re.search(rb"link\.main: boot: ([A-Za-z -]+);", buf)
            if started and m:
                why = m.group(1).decode()
                if why in ("panic", "interrupt watchdog", "task watchdog", "watchdog"):
                    print(f"booted, but the boot reads as a crash ({why}): the boot guard counts it")
                    sys.exit(2)
                print(f"booted: link.main ... starting ({why})")
                sys.exit(0)
        else:
            time.sleep(0.05)
        # Running (its heartbeat) but the one-shot boot line went by before the
        # port was open again: booted, the reason unknown.
        if heartbeat_at and time.monotonic() - heartbeat_at > 10:
            print("booted (its heartbeat), but the boot reason line wasn't seen")
            sys.exit(3)
    except BlockingIOError:
        time.sleep(0.05)
    except OSError:
        # The USB-Serial-JTAG port goes away across the reset: open it again.
        if fd is not None:
            os.close(fd)
        fd = None
        time.sleep(0.2)
if heartbeat_at:
    print("booted (its heartbeat), but the boot reason line wasn't seen")
    sys.exit(3)
sys.exit(1)
PY
boot_rc=$?
case $boot_rc in
    0) ;;
    3) echo "booted, boot reason not seen (log: $flog): check that it wasn't a crash" >&2; exit 3 ;;
    *) echo "NO BOOT within 60 s after a verified flash: stop and report. Log: $flog" >&2; exit 1 ;;
esac
