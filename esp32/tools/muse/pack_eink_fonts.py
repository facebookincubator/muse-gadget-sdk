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

"""Pack the e-ink UI's fonts into the image flashed to its `fonts` partition.

    pack_eink_fonts.py OUT.bin FONT...

The layout, which components/muse/eink/muse_eink_fonts.cpp reads in place:
"MFP1", a little-endian u32 count, then per font a 16-byte NUL-padded name
(the file name without its extension), a u32 offset from the start and a u32
size; the fonts follow, each 4-byte aligned.
"""

import os
import struct
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, paths = sys.argv[1], sys.argv[2:]
    fonts = []
    for path in paths:
        name = os.path.splitext(os.path.basename(path))[0].encode()
        if len(name) > 15:
            sys.exit(f"{path}: name longer than 15 bytes")
        fonts.append((name, open(path, "rb").read()))
    offset = 8 + 24 * len(fonts)
    header, body = bytearray(b"MFP1" + struct.pack("<I", len(fonts))), bytearray()
    for name, data in fonts:
        pad = -(offset + len(body)) % 4
        body += b"\0" * pad
        header += name.ljust(16, b"\0") + struct.pack("<II", offset + len(body), len(data))
        body += data
    with open(out, "wb") as f:
        f.write(header + body)


if __name__ == "__main__":
    main()
