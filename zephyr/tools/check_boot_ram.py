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

"""The boot record's RAM against the MCUboot that was actually built.

The app's build checks its retained block (mg_retained) against
CONFIG_MG_BOOT_RAM_END, a number the board sets by hand. This checks that
number against the bootloader the same sysbuild built: MCUboot's
_image_ram_end (its map) must not pass it, or the bootloader could clear the
boot record at every boot. A board that leaves CONFIG_MG_BOOT_RAM_END at 0
has no block to check.
"""

import argparse
import re
import sys


def config_value(path, name):
    pat = re.compile(r"^CONFIG_%s=(\S+)$" % re.escape(name))
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = pat.match(line.strip())
            if m:
                return int(m.group(1), 0)
    return 0


def image_ram_end(path):
    pat = re.compile(r"^\s*0x([0-9a-fA-F]+)\s+_image_ram_end\s*=")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if m:
                return int(m.group(1), 16)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--app-config", required=True, help="the app's zephyr/.config")
    ap.add_argument("--mcuboot-map", required=True, help="MCUboot's zephyr/zephyr.map")
    a = ap.parse_args()

    limit = config_value(a.app_config, "MG_BOOT_RAM_END")
    if limit == 0:
        return 0
    end = image_ram_end(a.mcuboot_map)
    if end is None:
        print("check_boot_ram: no _image_ram_end in %s" % a.mcuboot_map, file=sys.stderr)
        return 1
    if end > limit:
        print(
            "check_boot_ram: the MCUboot built here uses RAM up to 0x%08x, above "
            "CONFIG_MG_BOOT_RAM_END 0x%08x: it could clear the boot record. Raise "
            "CONFIG_MG_BOOT_RAM_END and move mg_retained above it." % (end, limit),
            file=sys.stderr,
        )
        return 1
    print("check_boot_ram: MCUboot's RAM ends at 0x%08x, under 0x%08x" % (end, limit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
