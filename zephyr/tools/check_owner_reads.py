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
"""Build check: the owner-content reads are listed and gated.

  check_owner_reads.py <src dir>

src/mg_owner_reads.h's MG_OWNER_READS names every wireless read that returns
the owner's content; each must have an MG_OWNER_READ("name") site in src/*.c,
and every site must be listed. Exit 1, naming the read, otherwise.
"""
import pathlib
import re
import sys


def main():
    src = pathlib.Path(sys.argv[1])
    head = (src / "mg_owner_reads.h").read_text()
    m = re.search(r'#define MG_OWNER_READS "([^"]*)"', head)
    if not m:
        sys.exit("check_owner_reads: no MG_OWNER_READS list in mg_owner_reads.h")
    listed = set(m.group(1).split())
    sites = {}
    for f in sorted(src.glob("*.c")):
        for name in re.findall(r'MG_OWNER_READ\("([a-z_]+)"\)', f.read_text()):
            sites.setdefault(name, []).append(f.name)
    bad = [f"'{n}' is listed but no MG_OWNER_READ(\"{n}\") site gates it" for n in sorted(listed - sites.keys())]
    bad += [f"'{n}' ({', '.join(sites[n])}) is gated but not in MG_OWNER_READS" for n in sorted(sites.keys() - listed)]
    if bad:
        sys.exit("check_owner_reads: " + "; ".join(bad))
    print(f"check_owner_reads: {len(listed)} owner-content reads, each gated")


if __name__ == "__main__":
    main()
