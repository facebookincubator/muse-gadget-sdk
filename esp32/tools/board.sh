#!/usr/bin/env bash
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

# Build, flash or monitor one of the bench boards, each in its own build-<board>
# directory with its own sdkconfig.
#
# Usage: board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]
#
#   devkit     ESP32-C5 DevKitC-1 (status LED, no display)
#   ideaspark  ideaspark ESP32 with a 1.9 inch ST7789 display
#   sensecap-indicator
#              Seeed SenseCAP Indicator (ESP32-S3) with a 4 inch display
#   home-assistant-voice
#              Home Assistant Voice PE (ESP32-S3), push-to-talk voice chat
#   seeed-respeaker-lite
#              reSpeaker Lite with XIAO ESP32-S3 (experimental voice profile)
#   reterminal-e1001
#              Seeed reTerminal E1001 (ESP32-S3) with a 7.5 inch e-paper
#   reterminal-e1002
#              Seeed reTerminal E1002 (ESP32-S3) with a 7.3 inch colour e-paper
#   c6-nopsram ESP32-C6 devkit without PSRAM (status LED on GPIO8, no display)
#   espressif-s3-devkitc-1
#              ESP32-S3-DevKitC-1 v1.1 N8R8 (status LED, no display)
#   waveshare-c6-lcd-147
#              Waveshare ESP32-C6-LCD-1.47 with a 172x320 status screen
#   waveshare-s3-epaper-154
#              Waveshare ESP32-S3-1.54inch-ePaper V2 with a 200x200 e-paper
#   waveshare-s3-epaper-154g
#              Waveshare ESP32-S3-ePaper-1.54G with a 1.54 inch colour e-paper
#
# The action defaults to build. Without PORT, flash and monitor use the only
# matching serial port, if there is exactly one.
set -euo pipefail

usage() { sed -n '16,42p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

[ $# -ge 1 ] || usage 2
case "$1" in -h|--help) usage ;; esac
BOARD="$1"
ACTION="${2:-build}"
PORT="${3:-}"

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULTS="sdkconfig.defaults"

# Board: target, overlay (loaded last) and the ports its USB bridge shows up as.
case "$BOARD" in
  devkit)
    TARGET=esp32c5
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  c6-nopsram)
    TARGET=esp32c6
    DEFAULTS="$DEFAULTS;devices/sdkconfig.c6-nopsram"
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  ideaspark|sensecap-indicator)
    [ "$BOARD" = ideaspark ] && TARGET=esp32 || TARGET=esp32s3
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    # CH340 bridge. The SenseCAP Indicator's RP2040 shows up as a usbmodem, not here.
    PORTS="/dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/ttyUSB*"
    ;;
  reterminal-e1001|reterminal-e1002)
    TARGET=esp32s3
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    PORTS="/dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/ttyUSB*"
    ;;
  waveshare-s3-epaper-154g)
    TARGET=esp32s3
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    # The S3's own USB-Serial-JTAG, which looks like a DevKitC-1's; check
    # which board is on the port before flashing.
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  home-assistant-voice|seeed-respeaker-lite)
    TARGET=esp32s3
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    # The S3's own USB-Serial-JTAG, which looks like a DevKitC-1's; check
    # which board is on the port before flashing.
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  waveshare-c6-lcd-147)
    TARGET=esp32c6
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    # The C6's own USB-Serial-JTAG.
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  espressif-s3-devkitc-1)
    TARGET=esp32s3
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    # The "ESP32-S3 USB Port", the chip's own USB-Serial-JTAG. The
    # USB-to-UART port works too; pass it explicitly.
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  waveshare-s3-epaper-154)
    TARGET=esp32s3
    DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"
    # The S3's own USB-Serial-JTAG.
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
  *) echo "error: unknown board '$BOARD'" >&2; usage 2 ;;
esac

case "$ACTION" in
  build)         IDF_ACTIONS=(build) ;;
  flash)         IDF_ACTIONS=(flash) ;;
  monitor)       IDF_ACTIONS=(monitor) ;;
  flash-monitor) IDF_ACTIONS=(flash monitor) ;;
  *) echo "error: unknown action '$ACTION'" >&2; usage 2 ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
  for d in "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.1" "$HOME/esp/esp-idf-v6" "$HOME/esp/esp-idf"; do
    if [ -n "$d" ] && [ -f "$d/export.sh" ]; then
      # shellcheck disable=SC1091
      . "$d/export.sh" >/dev/null 2>&1
      break
    fi
  done
  command -v idf.py >/dev/null 2>&1 || {
    echo "error: idf.py not found; activate ESP-IDF v6 first" >&2; exit 1; }
fi

PORT_ARGS=()
if [ "$ACTION" != build ]; then
  if [ -z "$PORT" ]; then
    # shellcheck disable=SC2086
    FOUND=($(ls $PORTS 2>/dev/null || true))
    if [ "${#FOUND[@]}" -ne 1 ]; then
      echo "error: pass the port; found ${#FOUND[@]} candidate(s): ${FOUND[*]:-none}" >&2
      exit 1
    fi
    PORT="${FOUND[0]}"
  fi
  echo ">> $BOARD on $PORT"
  PORT_ARGS=(-p "$PORT")
fi

BUILD_DIR="$PROJECT/build-$BOARD"
cd "$PROJECT"
exec idf.py -B "$BUILD_DIR" \
  -DIDF_TARGET="$TARGET" \
  -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$DEFAULTS" \
  ${PORT_ARGS[@]+"${PORT_ARGS[@]}"} "${IDF_ACTIONS[@]}"
