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

# Non-destructive dual-boot helper for the WeatherXM WG1200 gateway:
#
# Preserves untouched:
#   - bootloader      (0x000000, 48 KB)
#   - esp_secure_cert (0x00D000, 8 KB)   - X.509 station identity
#   - nvs             (0x00F000, 16 KB)  - encrypted WeatherXM NVS
#   - phy_init        (0x015000, 4 KB)
#   - factory         (0x020000, 4 MB)   - WeatherXM factory firmware
#   - ota_0           (0x420000, 4 MB)   - WeatherXM active OTA firmware
#   - nvs_key         (0xC20000, 4 KB)   - WeatherXM NVS encryption key
#   - spiffs          (0xC21000, 3 MB)   - WeatherXM SPIFFS storage
#
# Writes only:
#   - partition-table (0x00C000, 4 KB)   - stock layout + muse_nvs at 0xF21000
#   - otadata         (0x013000, 8 KB)   - boot selector (ota_1 <-> ota_0/factory)
#   - ota_1           (0x820000, 4 MB)   - Muse Gadget firmware

set -euo pipefail

CMD=${1:-help}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-muse-wxm-wg1200"}
BACKUP_DIR=${BACKUP_DIR:-"$ROOT/../backups/wg1200_stock_backup"}
BAUD=${BAUD:-460800}
SIGNING_KEY=${WXM_SIGNING_KEY:-}

resolve_port() {
    if [ -n "${1:-}" ]; then
        echo "$1"
        return 0
    fi
    if [ -n "${PORT:-}" ]; then
        echo "$PORT"
        return 0
    fi
    python3 "$ROOT/tools/muse/ports.py" wg1200
}

activate_idf() {
    if ! command -v idf.py >/dev/null 2>&1; then
        for d in "$HOME/.espressif/esp-idf-v6.0.3" "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.3" "$HOME/esp/esp-idf-v6" "$HOME/esp/esp-idf" "$HOME/.platformio/packages/framework-espidf"; do
            if [ -n "$d" ] && [ -f "$d/export.sh" ]; then
                # shellcheck disable=SC1090
                . "$d/export.sh" >/dev/null 2>&1
                break
            fi
        done
    fi
    command -v idf.py >/dev/null 2>&1 || { echo "Error: idf.py not found in PATH; activate ESP-IDF v6.0.3 first." >&2; exit 1; }
}

make_otadata_slot() {
    local slot=$1
    local out=$2
    python3 - "$slot" "$out" << 'PY'
import sys, struct, binascii

slot = int(sys.argv[1])  # 0 for ota_0, 1 for ota_1, -1 for factory
out_path = sys.argv[2]

def make_entry(seq):
    seq_bytes = struct.pack("<I", seq)
    crc = binascii.crc32(seq_bytes, 0xFFFFFFFF) & 0xFFFFFFFF
    entry = struct.pack("<I20sII", seq, b"\xff" * 20, 0x00000002, crc)
    return entry + b"\xff" * (4096 - len(entry))

if slot < 0:
    data = b"\xff" * 8192
elif slot == 0:
    data = make_entry(3) + make_entry(2)
else:
    data = make_entry(1) + make_entry(2)

with open(out_path, "wb") as f:
    f.write(data)
PY
}

sign_if_key_set() {
    local src_bin="$BUILD_DIR/muse-gadget-unsigned.bin"
    [ -f "$src_bin" ] || src_bin="$BUILD_DIR/muse-gadget.bin"
    if [ -n "$SIGNING_KEY" ]; then
        [ -f "$SIGNING_KEY" ] || { echo "Error: WXM_SIGNING_KEY not found at $SIGNING_KEY" >&2; exit 1; }
        local signed_bin="$BUILD_DIR/muse-gadget-signed.bin"
        espsecure.py sign_data --version 2 --keyfile "$SIGNING_KEY" --output "$signed_bin" "$src_bin"
        espsecure.py verify_signature --version 2 --keyfile "$SIGNING_KEY" "$signed_bin"
        echo "$signed_bin"
    else
        echo "$BUILD_DIR/muse-gadget.bin"
    fi
}

case "$CMD" in
    backup)
        activate_idf
        TARGET_PORT=$(resolve_port "${2:-}")
        mkdir -p "$BACKUP_DIR"
        echo "Backing up WG1200 identity and boot partitions from $TARGET_PORT to $BACKUP_DIR..."
        python3 -m esptool --chip esp32s3 -p "$TARGET_PORT" -b "$BAUD" --no-stub read_flash \
            0x000000 0x16000 "$BACKUP_DIR/low_flash_0x0_to_0x16000.bin" \
            0x00C000 0x01000 "$BACKUP_DIR/partition-table.bin" \
            0x00D000 0x02000 "$BACKUP_DIR/esp_secure_cert.bin" \
            0x00F000 0x04000 "$BACKUP_DIR/nvs.bin" \
            0x013000 0x02000 "$BACKUP_DIR/otadata.bin" \
            0x015000 0x01000 "$BACKUP_DIR/phy_init.bin" \
            0xC20000 0x01000 "$BACKUP_DIR/nvs_key.bin"
        echo "Backup saved in $BACKUP_DIR"
        ;;

    build)
        "$ROOT/tools/muse/board.sh" build wg1200
        if [ -n "$SIGNING_KEY" ]; then
            activate_idf
            SIGNED=$(sign_if_key_set)
            echo "Signed WG1200 image written to $SIGNED"
        fi
        ;;

    install)
        activate_idf
        TARGET_PORT=$(resolve_port "${2:-}")
        if [ ! -f "$BUILD_DIR/muse-gadget.bin" ]; then
            echo "Error: $BUILD_DIR/muse-gadget.bin not found. Run '$0 build' (or 'tools/muse/board.sh build wg1200') first." >&2
            exit 1
        fi
        IMAGE_TO_FLASH=$(sign_if_key_set)
        OTADATA_OTA1="$BUILD_DIR/otadata_ota1.bin"
        make_otadata_slot 1 "$OTADATA_OTA1"
        echo "Installing dual-boot partition table (0xC000), otadata -> ota_1 (0x13000), and Muse -> ota_1 (0x820000) on $TARGET_PORT..."
        python3 -m esptool --chip esp32s3 -p "$TARGET_PORT" -b "$BAUD" --no-stub \
            --before default_reset --after hard_reset write_flash \
            0x00C000 "$BUILD_DIR/partition_table/partition-table.bin" \
            0x013000 "$OTADATA_OTA1" \
            0x820000 "$IMAGE_TO_FLASH"
        echo "Muse Gadget installed in ota_1 (0x820000); WeatherXM partitions (factory, ota_0, certs, NVS) untouched."
        ;;

    boot-weatherxm)
        activate_idf
        TARGET_PORT=$(resolve_port "${2:-}")
        mkdir -p "$BUILD_DIR"
        HDR_OTA0="$BUILD_DIR/ota0_hdr.bin"
        python3 -m esptool --chip esp32s3 -p "$TARGET_PORT" -b "$BAUD" --no-stub \
            read_flash 0x420000 0x120 "$HDR_OTA0" >/dev/null 2>&1 || true
        SLOT=$(python3 - "$HDR_OTA0" << 'PY'
import sys
try:
    b = open(sys.argv[1], "rb").read()
    proj = b[0x50:0x70].split(b"\x00", 1)[0].decode("ascii", "ignore").lower()
    print("0" if proj.startswith(("wg1", "weatherxm", "wxm")) else "-1")
except Exception:
    print("-1")
PY
)
        OTADATA_WXM="$BUILD_DIR/otadata_wxm.bin"
        make_otadata_slot "$SLOT" "$OTADATA_WXM"
        python3 -m esptool --chip esp32s3 -p "$TARGET_PORT" -b "$BAUD" --no-stub \
            --before default_reset --after hard_reset write_flash \
            0x013000 "$OTADATA_WXM"
        echo "WG1200 boot selector switched to WeatherXM ($([ "$SLOT" = "0" ] && echo ota_0 || echo factory))."
        ;;

    boot-muse)
        activate_idf
        TARGET_PORT=$(resolve_port "${2:-}")
        mkdir -p "$BUILD_DIR"
        OTADATA_OTA1="$BUILD_DIR/otadata_ota1.bin"
        make_otadata_slot 1 "$OTADATA_OTA1"
        python3 -m esptool --chip esp32s3 -p "$TARGET_PORT" -b "$BAUD" --no-stub \
            --before default_reset --after hard_reset write_flash \
            0x013000 "$OTADATA_OTA1"
        echo "WG1200 boot selector switched to Muse Gadget (ota_1)."
        ;;

    *)
        echo "Usage: $0 {backup|build|install|boot-weatherxm|boot-muse} [serial-port]"
        exit 1
        ;;
esac
