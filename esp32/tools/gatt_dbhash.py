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
"""The GATT Database Hash (Core Vol 3 Part G 7.3) from a gadget's table walk.

An independent reference for main/gatt_dbhash_core.c: give it the log lines
'>gatt.dump' prints ("attr <handle> <type hex> <value hex>", then
"hash <hex>") and it serializes the table the spec's way, takes the AES-CMAC
with an all-zero key, and checks the gadget's hash (and, with --read HEX, what
a phone read from 0x2B2A).

    python3 tools/gatt_dbhash.py LOG [--read HEX]

Needs `cryptography`.
"""
import argparse
import re
import sys

WITH_VALUE = {0x2800, 0x2801, 0x2802, 0x2803, 0x2900}
WITHOUT_VALUE = {0x2901, 0x2902, 0x2903, 0x2904, 0x2905}


def serialize(attrs):
    """attrs: [(handle, type, value bytes)] in any order."""
    out = bytearray()
    for handle, typ, value in sorted(attrs):
        if typ in WITH_VALUE:
            out += handle.to_bytes(2, "little") + typ.to_bytes(2, "little") + bytes(value)
        elif typ in WITHOUT_VALUE:
            out += handle.to_bytes(2, "little") + typ.to_bytes(2, "little")
    return bytes(out)


def cmac(key, msg):
    from cryptography.hazmat.primitives.cmac import CMAC
    from cryptography.hazmat.primitives.ciphers import algorithms
    c = CMAC(algorithms.AES(key))
    c.update(msg)
    return c.finalize()


def database_hash(attrs):
    """As the characteristic carries it: least significant octet first."""
    return cmac(bytes(16), serialize(attrs))[::-1]


def parse(text):
    attrs, device = [], None
    for line in text.splitlines():
        m = re.search(r"attr (\d+) ([0-9a-f]{4}) ([0-9a-f]*)\s*$", line)
        if m:
            attrs.append((int(m.group(1)), int(m.group(2), 16), bytes.fromhex(m.group(3))))
        m = re.search(r"\bhash ([0-9a-f]{32})\s*$", line)
        if m:
            device = bytes.fromhex(m.group(1))
    return attrs, device


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--read", help="what a client read from 0x2B2A, hex")
    a = ap.parse_args()
    attrs, device = parse(open(a.log, errors="replace").read())
    if not attrs:
        sys.exit("no 'attr' lines")
    want = database_hash(attrs)
    print(f"{len(attrs)} attributes, {len(serialize(attrs))} bytes hashed: {want.hex()}")
    ok = True
    if device is not None:
        print(f"gadget's hash: {device.hex()} {'match' if device == want else 'MISMATCH'}")
        ok &= device == want
    if a.read:
        read = bytes.fromhex(a.read)
        print(f"read over the air: {read.hex()} {'match' if read == want else 'MISMATCH'}")
        ok &= read == want
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
