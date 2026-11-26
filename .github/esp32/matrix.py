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

"""Split the ESP32 workflow's board list into build groups.

Usage: matrix.py [--per-group N] < board list

The board list is the one in .github/workflows/esp32.yml's Plan job, one
entry per board:

    - board: NAME
      target: CHIP
      overlays: ";devices/sdkconfig.X;..."

Prints a GitHub Actions matrix, {"include": [{"group", "names", "boards"}]},
where "boards" holds each board's name, target and overlays. Each build job
builds one group's boards in turn, so it sets up ESP-IDF once for several
boards. Groups are consecutive runs of the list, so a board added at the end
changes only the last group and every other group keeps its build cache.
Every board in the list is in exactly one group.
"""

import argparse
import json
import re
import sys

FIELD = re.compile(r'^(-\s+)?(board|target|overlays):\s*(.*?)\s*$')


def parse(text):
    boards = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = FIELD.match(line.strip())
        if not m:
            sys.exit(f"board list line {n}: expected board, target or overlays: {line.strip()}")
        dash, key, value = m.groups()
        if value.startswith('"') and value.endswith('"') and len(value) >= 2:
            value = value[1:-1]
        if dash:
            if key != "board":
                sys.exit(f"board list line {n}: an entry starts with board")
            boards.append({"board": value})
        elif not boards or key in boards[-1]:
            sys.exit(f"board list line {n}: {key} outside a board entry, or given twice")
        else:
            boards[-1][key] = value
    return boards


def check(boards):
    if not boards:
        sys.exit("the board list is empty")
    names = [b["board"] for b in boards]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        sys.exit(f"the board list has a board twice: {', '.join(dupes)}")
    for b in boards:
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", b["board"]):
            sys.exit(f"board name {b['board']!r}: lowercase letters, digits and dashes only")
        if not re.fullmatch(r"esp32[a-z0-9]*", b.get("target", "")):
            sys.exit(f"{b['board']}: needs an esp32 target")
        if "overlays" not in b:
            sys.exit(f"{b['board']}: needs overlays (\"\" for none)")
        if not re.fullmatch(r"(;devices/sdkconfig\.[A-Za-z0-9._-]+)*", b["overlays"]):
            sys.exit(f"{b['board']}: overlays are ;devices/sdkconfig.<name> entries")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-group", type=int, default=3)
    args = ap.parse_args()
    if args.per_group < 1:
        sys.exit("--per-group must be at least 1")
    boards = parse(sys.stdin.read())
    check(boards)
    groups = []
    for i in range(0, len(boards), args.per_group):
        chunk = boards[i:i + args.per_group]
        groups.append({
            "group": f"g{len(groups) + 1:02d}",
            "names": " ".join(b["board"] for b in chunk),
            "boards": chunk,
        })
    print(json.dumps({"include": groups}, separators=(",", ":")))


if __name__ == "__main__":
    main()
