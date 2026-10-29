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

"""Turn an image into a file for `display.draw_url` or `display.draw_file` on a
Link with a display.

The image is scaled to cover the screen (170x320 by default), cropped to the
centre, and written as a baseline JPEG for a .jpg/.jpeg name, otherwise as raw
RGB565 with the high byte first.

Usage: image_for_display.py picture.png out.jpg [--width 480] [--height 480]
"""

import argparse
import struct

from PIL import Image, ImageOps


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image")
    parser.add_argument("out")
    parser.add_argument("--width", type=int, default=170)
    parser.add_argument("--height", type=int, default=320)
    args = parser.parse_args()

    img = Image.open(args.image).convert("RGB")
    img = ImageOps.fit(img, (args.width, args.height), Image.LANCZOS)

    if args.out.lower().endswith((".jpg", ".jpeg")):
        # The device's ROM decoder handles baseline JPEG only.
        img.save(args.out, "JPEG", quality=85, progressive=False)
        return
    px = img.load()
    out = bytearray()
    for y in range(args.height):
        for x in range(args.width):
            r, g, b = px[x, y]
            out += struct.pack(">H", (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3)
    with open(args.out, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
