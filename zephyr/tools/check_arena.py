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
"""Fails the build when the malloc arena is too small for the device session and SMP together.

With CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1 the arena is the RAM from the image's end (`_end`) to
the end of zephyr,sram. The device session takes about 30 KB of it from a hello to the connection's
end, and SMP (the BLE recovery tier) about 10 KB while an update runs (src/mg_smp_mem.c): --min-kb is
both with a margin. A way back never depends on a pool that other users can fill first."""
import argparse
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--ram-end", required=True)
    ap.add_argument("--min-kb", type=int, required=True)
    a = ap.parse_args()
    out = subprocess.run([a.nm, a.elf], capture_output=True, text=True, check=True).stdout
    end = next((int(line.split()[0], 16) for line in out.splitlines()
                if line.split()[-1:] == ["_end"]), None)
    if end is None:
        sys.exit("check_arena: no _end symbol in the image")
    arena = int(a.ram_end, 16) - end
    if arena < a.min_kb * 1024:
        sys.exit(f"check_arena: the malloc arena is {arena // 1024} KB, below {a.min_kb} KB: the device "
                 "session and an SMP update can't both have their memory")
    print(f"check_arena: malloc arena {arena // 1024} KB (at least {a.min_kb} KB for the session and SMP)")


if __name__ == "__main__":
    main()
