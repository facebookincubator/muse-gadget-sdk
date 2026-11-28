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
"""Move a codec's hot functions and tables into the sections a port runs from RAM.

    hot_place.py --hot-list hot.txt --hot-text .ram_text [--hot-rodata .ram_rodata] obj.o...

Objects built with -ffunction-sections -fdata-sections keep each function in
.text.<name> and each table in .rodata.<name> (or .srodata.<name>). For every
object named in the hot list, this renames those sections to
<hot-text>.<object>.<name> and <hot-rodata>.<object>.<name> with objcopy, in
place, so the port's linker script places them with `*(<hot-text>.*)` and
--gc-sections still drops what is unused. The section names are the port's.
A section an earlier run already moved counts as a match, so a rerun over the
same objects (an incremental build) succeeds. An entry that matches nothing
(no such object, function or table, an object with no section left to move,
or a table without --hot-rodata) is reported and the exit status is 1.

The hot list has one entry a line ('#' starts a comment):
    mdct              every function of mdct.o (and, with --hot-rodata, its tables)
    lc3:lc3_encode    one function, or one table (tables:mdct_win_10m_16k)

OBJCOPY and OBJDUMP pick the tools (default: objcopy, objdump).
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


def entries(path):
    whole, names = set(), set()
    for line in Path(path).read_text().splitlines():
        e = line.split("#", 1)[0].strip()
        if e:
            (names.add(tuple(e.split(":", 1))) if ":" in e else whole.add(e))
    return whole, names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--hot-list", required=True, help="the hot objects and functions, one a line")
    ap.add_argument("--hot-text", required=True, help="section name prefix for hot code")
    ap.add_argument("--hot-rodata", help="section name prefix for hot tables (default: tables stay)")
    ap.add_argument("objects", nargs="+")
    a = ap.parse_args()
    whole, names = entries(a.hot_list)
    objcopy = os.environ.get("OBJCOPY", "objcopy")
    objdump = os.environ.get("OBJDUMP", "objdump")
    moved = 0
    whole_hit, hit = set(), set()
    for obj in a.objects:
        stem = Path(obj).name.split(".")[0]
        if stem not in whole and not any(o == stem for o, _ in names):
            continue
        heads = subprocess.run([objdump, "-h", obj], capture_output=True, text=True, check=True).stdout
        args = []
        placed = [f"{t}.{stem}." for t in (a.hot_text, a.hot_rodata) if t]
        for sec, size in re.findall(r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)", heads, re.M):
            if not int(size, 16):
                continue   # every object has an empty .text: moving it is no hit
            # moved by an earlier run: a hit, nothing to rename
            done = next((sec[len(p):] for p in placed if sec.startswith(p)), None)
            if done is not None:
                if stem in whole or (stem, done) in names:
                    hit.add((stem, done))
                    if stem in whole:
                        whole_hit.add(stem)
                continue
            m = re.fullmatch(r"\.(text|s?rodata)(?:\.(.+))?", sec)
            if not m:
                continue
            kind = m.group(1)
            name = m.group(2) or kind
            to = a.hot_text if kind == "text" else a.hot_rodata
            if to and (stem in whole or (stem, name) in names):
                args += ["--rename-section", f"{sec}={to}.{stem}.{name}"]
                hit.add((stem, name))
                if stem in whole:
                    whole_hit.add(stem)
        if args:
            subprocess.run([objcopy, *args, obj], check=True)
            moved += len(args) // 2
    print(f"hot_place: {moved} sections moved")
    # A typo or a renamed function would otherwise leave it in flash unnoticed
    missed = sorted(e for e in whole if e not in whole_hit) + \
        sorted(f"{o}:{n}" for o, n in names if (o, n) not in hit)
    for e in missed:
        print(f"hot_place: {e} matched nothing", file=sys.stderr)
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
