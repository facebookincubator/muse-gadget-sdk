#!/usr/bin/env python3
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

"""Copy one board's flash images out of an ESP-IDF build directory and write
an ESP Web Tools manifest for them.

Usage: make_manifest.py BUILD_DIR OUT_DIR NAME VERSION

The images are flashed one by one at their own offsets, exactly like
`idf.py flash`, rather than as a single merged image: a merged image would
pad the gap where NVS lives with 0xFF and wipe the device's pairing and Wi-Fi
settings on every update.
"""

import json
import shutil
import struct
import sys
from pathlib import Path

# ESP-IDF target -> ESP Web Tools chipFamily.
CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c2": "ESP32-C2",
    "esp32c3": "ESP32-C3",
    "esp32c5": "ESP32-C5",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
    "esp32p4": "ESP32-P4",
}


def partitions_end(table):
    """Where the last partition in a binary partition table ends: the flash
    the build needs."""
    end = 0
    for i in range(0, len(table) - 31, 32):
        magic, _type, _subtype, offset, size = struct.unpack_from("<HBBII", table, i)
        if magic != 0x50AA:   # 0xEBEB is the table's MD5, 0xFFFF the end
            break
        end = max(end, offset + size)
    return end


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    build, out, name, version = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]

    args = json.loads((build / "flasher_args.json").read_text())
    chip = args["extra_esptool_args"]["chip"]

    out.mkdir(parents=True, exist_ok=True)
    parts = []
    for offset, file in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        dest = Path(file).name
        shutil.copyfile(build / file, out / dest)
        parts.append({"path": dest, "offset": int(offset, 16)})

    table = (build / args["partition-table"]["file"]).read_bytes()
    manifest = {
        "name": name,
        "version": version,
        # Offer to erase on first install; updates keep pairing and Wi-Fi.
        "new_install_prompt_erase": True,
        # Not ESP Web Tools': the page refuses boards with less flash.
        "min_flash_bytes": partitions_end(table),
        "builds": [{"chipFamily": CHIP_FAMILIES[chip], "parts": parts}],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
