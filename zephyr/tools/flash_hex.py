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
"""One hex to flash a XIAO nRF54L15 build (MCUboot + the signed app) safely.

    python3 tools/flash_hex.py BUILD_DIR -o mg-xiao.hex [--fresh]

BUILD_DIR is a sysbuild directory (west build --sysbuild). The XIAO's RRAM
is written 16 bytes at a time, and OpenOCD's nrf54l-load writes the image
through the memory bus: a final partial 16-byte word (the signed image ends
on an odd length) can be left unwritten, and whatever an earlier firmware
left in the primary slot's trailer stays there for MCUboot to misread. This
merges both images, pads every segment out to whole 16-byte words with 0xff,
and blanks (writes 0xff, RRAM's erased value) the last 16 KB of slot0, where
MCUboot keeps its swap state.

--fresh also blanks slot1's first and last 16 KB (an old image's header and
trailer) and the whole settings partition: a clean first flash, without a
chip erase. It drops pairings, settings and device tokens.

Partition offsets come from the build's own devicetree. Needs intelhex (in
the Zephyr venv: pip install intelhex).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

WORD = 16            # RRAM write unit
TRAILER = 16 * 1024  # MCUboot swap state at a slot's end (8.6 KB here), whole sectors


def partitions(dts: str) -> dict[str, tuple[int, int]]:
    """{label: (offset, size)} for the fixed partitions in a zephyr.dts."""
    out = {}
    for m in re.finditer(r"(\w+_partition): partition@[0-9a-f]+ \{[^}]*?reg = < (0x[0-9a-f]+) (0x[0-9a-f]+) >",
                         dts, re.S):
        out[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return out


def pad_words(ih, fill: int = 0xFF) -> None:
    """Every 16-byte word that holds image bytes gets written whole: the
    bytes around them that no image sets become 0xff."""
    have = ih.todict()
    for start, end in ih.segments():
        for a in range(start - start % WORD, -(-end // WORD) * WORD):
            if a not in have:
                ih[a] = fill


def blank(ih, start: int, size: int) -> None:
    for a in range(start, start + size):
        ih[a] = 0xFF


def build_hex(build: Path, fresh: bool):
    from intelhex import IntelHex

    boot = build / "mcuboot/zephyr/zephyr.hex"
    app = build / "zephyr/zephyr/zephyr.signed.hex"
    for f in (boot, app):
        if not f.exists():
            raise SystemExit(f"{f} missing: build with --sysbuild")
    parts = partitions((build / "zephyr/zephyr/zephyr.dts").read_text())
    s0, s1, st = parts["slot0_partition"], parts["slot1_partition"], parts["storage_partition"]
    ih = IntelHex()
    ih.merge(IntelHex(str(boot)), overlap="error")
    ih.merge(IntelHex(str(app)), overlap="error")
    if ih.maxaddr() >= s0[0] + s0[1] - TRAILER:
        raise SystemExit("the app reaches into slot0's trailer: too big for the slot")
    pad_words(ih)
    blank(ih, s0[0] + s0[1] - TRAILER, TRAILER)
    if fresh:
        blank(ih, s1[0], TRAILER)
        blank(ih, s1[0] + s1[1] - TRAILER, TRAILER)
        blank(ih, st[0], st[1])
    for start, end in ih.segments():
        assert start % WORD == 0 and end % WORD == 0, (hex(start), hex(end))
    return ih


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path, help="a sysbuild build directory")
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--fresh", action="store_true",
                    help="also blank slot1's header and trailer and the settings (drops pairings and tokens)")
    a = ap.parse_args(argv)
    ih = build_hex(a.build, a.fresh)
    # The image holds the SDK token when the build did: 0600.
    fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as f:
        ih.write_hex_file(f)
    print(f"{a.out}: " + ", ".join(f"{s:#x}-{e:#x}" for s, e in ih.segments()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
