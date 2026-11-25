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
"""Worst-case stack per entry point from GCC's -fcallgraph-info=su files.

  stack_report.py FILE.ci... -- ENTRY...

Every function's frame comes from GCC (static sizes; a dynamic frame is
flagged). Calls through pointers (the platform callbacks) and library calls
(memcpy, ...) count 0 here: add the deepest platform callback yourself.
Recursion would be reported as an error (there is none).
"""

import re
import sys

NODE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
SIZE = re.compile(r"\\n(\d+) bytes \(([a-z,]+)\)")


def main() -> int:
    args = sys.argv[1:]
    split = args.index("--")
    files, entries = args[:split], args[split + 1:]
    frame: dict[str, int] = {}
    kind: dict[str, str] = {}
    calls: dict[str, set] = {}
    for f in files:
        for line in open(f, encoding="utf-8", errors="replace"):
            m = NODE.search(line)
            if m:
                title, label = m.groups()
                s = SIZE.search(label)
                if s:
                    frame[title] = int(s.group(1))
                    kind[title] = s.group(2)
                else:
                    frame.setdefault(title, 0)
                continue
            m = EDGE.search(line)
            if m:
                calls.setdefault(m.group(1), set()).add(m.group(2))
    memo: dict[str, tuple[int, list]] = {}

    def worst(fn: str, stack: tuple) -> tuple[int, list]:
        if fn in stack:
            raise SystemExit(f"recursion through {fn}")
        if fn in memo:
            return memo[fn]
        best = (0, [])
        for callee in calls.get(fn, ()):
            d, path = worst(callee, stack + (fn,))
            if d > best[0]:
                best = (d, path)
        r = (frame.get(fn, 0) + best[0], [fn] + best[1])
        memo[fn] = r
        return r

    dynamic = sorted(t for t, k in kind.items() if "dynamic" in k)
    for e in entries:
        depth, path = worst(e, ())
        short = " > ".join(p.split(":")[-1] + f"({frame.get(p, 0)})" for p in path)
        print(f"  {e:20s} {depth:5d} B  {short}")
    if dynamic:
        print("  dynamic frames: " + ", ".join(d.split(':')[-1] for d in dynamic))
    return 0


if __name__ == "__main__":
    sys.exit(main())
