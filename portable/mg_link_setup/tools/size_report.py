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
"""Sizes per object from a GNU ld map file (after --gc-sections).

  size_report.py MAP

Sums the retained input sections of each object into text (.text,
.rodata, .srodata) and RAM (.data, .sdata, .bss, .sbss), so libc and libgcc
(which the firmware already has) are reported separately.
"""

import re
import sys
from collections import defaultdict

SEC = re.compile(r"^\s*(\.[A-Za-z0-9_.$]+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o\)?|\S+\.a\(\S+\))\s*$")


def classify(name: str) -> str | None:
    if name.startswith((".text", ".rodata", ".srodata")):
        return "text"
    if name.startswith((".data", ".sdata", ".bss", ".sbss")):
        return "ram"
    return None


def main() -> int:
    sizes: dict = defaultdict(lambda: {"text": 0, "ram": 0})
    in_discarded = True
    pending = None
    for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
        if line.startswith("Linker script and memory map"):
            in_discarded = False
            continue
        if in_discarded:
            continue
        m = SEC.match(line)
        if m and m.group(1) is None and pending:
            name = pending
        elif m:
            name = m.group(1)
        else:
            s = line.strip()
            pending = s if s.startswith(".") and " " not in s else None
            continue
        pending = None
        cls = classify(name)
        size = int(m.group(3), 16)
        if cls is None or size == 0 or int(m.group(2), 16) == 0:
            continue
        obj = m.group(4).split("/")[-1]
        if ".a(" in obj:
            obj = "libc/libgcc: " + obj
        sizes[obj][cls] += size
    total = {"text": 0, "ram": 0}
    for obj in sorted(sizes, key=lambda o: -sizes[o]["text"]):
        t, r = sizes[obj]["text"], sizes[obj]["ram"]
        print(f"  {obj:44s} text {t:6d}  ram {r:4d}")
        if not obj.startswith("libc/libgcc") and obj != "size_main.o":
            total["text"] += t
            total["ram"] += r
    print(f"  {'TOTAL (ours, without size_main and libc)':44s} text {total['text']:6d}  "
          f"ram {total['ram']:4d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
