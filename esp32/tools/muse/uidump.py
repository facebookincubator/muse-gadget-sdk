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
"""What a gadget shows of a UI stream, from its console's `ui.dump`.

    uidump.py CONSOLE_LOG OUT_PREFIX [--ref PHONE_PNG]

Reads the last "@uidump" block in the log (muse_video.c, dump_picture) and
writes OUT_PREFIX_panel.png (the RGB565 the panel got, as RGB888),
OUT_PREFIX_decoded.png (the decoded YUV through an exact BT.601 limited-range
conversion with bilinear chroma: what a perfect display of the picture would
show) and OUT_PREFIX_y.png (luma alone). With --ref (the phone's picture at
the same size, e.g. the app's ui snapshot), prints PSNR of each against it,
over the panel's circle, and writes OUT_PREFIX_compare.png (ref | decoded |
panel, side by side, and a 3x crop of the centre). Needs numpy and Pillow.
"""
import argparse
import base64
import sys

import numpy as np
from PIL import Image


MAX_LOG = 256 << 20   # a console log; a header past these limits is a broken or hostile one
MAX_SIDE = 4096       # the largest UI picture is 480 px
MAX_PIXELS = 4 << 20


def check_header(fields):
    """The block's width, height and hq, refused before anything is allocated."""
    if len(fields) < 3 or not all(f.isdigit() for f in fields[:3]):
        sys.exit("malformed @uidump header: " + " ".join(fields)[:80])
    w, h, hq = (int(x) for x in fields[:3])
    if not (0 < w <= MAX_SIDE and 0 < h <= MAX_SIDE and w * h <= MAX_PIXELS) or w % 2 or h % 2 or hq not in (0, 1):
        sys.exit(f"refusing a {w}x{h} (hq {hq}) dump: even sides up to {MAX_SIDE}, {MAX_PIXELS} pixels, hq 0 or 1")
    return w, h, hq


def parse(path):
    import os
    if os.path.getsize(path) > MAX_LOG:
        sys.exit(f"{path}: over {MAX_LOG >> 20} MB, not a console log")
    lines = open(path, "rb").read().decode("latin-1").splitlines()
    start = max((i for i, l in enumerate(lines) if l.startswith("@uidump ") and len(l.split()) > 1 and l.split()[1].isdigit()), default=-1)
    if start < 0:
        sys.exit("no @uidump block in " + path)
    w, h, hq = check_header(lines[start].split()[1:4])
    Y = np.zeros((h, w), np.uint8)
    U = np.zeros((h // 2, w // 2), np.uint8)
    V = np.zeros((h // 2, w // 2), np.uint8)
    P = np.zeros((h, w * 2), np.uint8)
    got = {"Y": 0, "U": 0, "V": 0, "P": 0}
    rows_ok = np.zeros(h, bool)
    for l in lines[start + 1:]:
        if l.startswith("@uidump end"):
            break
        parts = l.split(" ")
        if len(parts) != 3 or parts[0][1:] not in got or not parts[1].isdigit():
            continue   # a malformed row: skipped, as a lost one is
        tag, n = parts[0][1:], int(parts[1])
        dst = {"Y": Y, "U": U, "V": V, "P": P}[tag]
        try:
            data = base64.b64decode(parts[2].strip(), validate=True)
        except ValueError:
            continue
        if n < dst.shape[0] and len(data) == dst.shape[1]:
            dst[n] = np.frombuffer(data, np.uint8)
            got[tag] += 1
            if tag == "P":
                rows_ok[n] = True
    if got["Y"] < h or got["P"] < h:
        print("warning: incomplete dump", got, file=sys.stderr)
    return w, h, hq, Y, U, V, P, rows_ok


def rgb565_be(P, w, h):
    v = (P[:, 0::2].astype(np.uint16) << 8) | P[:, 1::2]
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.dstack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]).astype(np.uint8)


def up_bilinear(c):
    """Chroma at luma resolution, centred between each 2x2's luma (as the phones average it)."""
    c = c.astype(np.float64)
    p = np.pad(c, 1, mode="edge")
    h, w = c.shape
    out = np.zeros((2 * h, 2 * w))
    for dy, oy in ((0, -1), (1, 1)):
        for dx, ox in ((0, -1), (1, 1)):
            out[dy::2, dx::2] = (9 * p[1:-1, 1:-1] + 3 * p[1 + oy:h + 1 + oy, 1:-1] + 3 * p[1:-1, 1 + ox:w + 1 + ox]
                                 + p[1 + oy:h + 1 + oy, 1 + ox:w + 1 + ox]) / 16
    return out


def yuv_to_rgb(Y, U, V):
    y = (Y.astype(np.float64) - 16) * 1.164
    u = up_bilinear(U) - 128
    v = up_bilinear(V) - 128
    rgb = np.dstack([y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u])
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8)


def circle(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    u = (2 * xx + 1) / w - 1
    v = (2 * yy + 1) / h - 1
    return u * u + v * v <= 0.97 ** 2


def psnr(a, b, m):
    d = (a.astype(np.float64) - b.astype(np.float64))[m]
    mse = (d * d).mean()
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("out")
    ap.add_argument("--ref")
    ap.add_argument("--square", action="store_true", help="a square panel (ESP-Mosaico): compare every pixel, not the circle")
    a = ap.parse_args()
    w, h, hq, Y, U, V, P, rows_ok = parse(a.log)
    panel = rgb565_be(P, w, h)
    dec = yuv_to_rgb(Y, U, V)
    Image.fromarray(panel).save(a.out + "_panel.png")
    Image.fromarray(dec).save(a.out + "_decoded.png")
    Image.fromarray(Y).save(a.out + "_y.png")
    print(f"{w}x{h}, conversion {'fine (hq)' if hq else 'fast'}")
    if a.ref:
        ref = np.asarray(Image.open(a.ref).convert("RGB"))[:h, :w]
        m = (np.ones((h, w), bool) if a.square else circle(w, h)) & rows_ok[:, None]
        where = "the whole square" if a.square else "inside the circle"
        print(f"PSNR vs the phone's picture ({where}): decoded {psnr(dec, ref, m):.2f} dB, "
              f"panel {psnr(panel, ref, m):.2f} dB")
        c = h // 2 - 40, w // 2 - 60
        crop = lambda im: np.kron(im[c[0]:c[0] + 80, c[1]:c[1] + 120], np.ones((3, 3, 1), np.uint8))
        top = np.hstack([ref, dec, panel])
        bot = np.hstack([crop(ref), crop(dec), crop(panel)])
        pad = np.zeros((bot.shape[0], top.shape[1] - bot.shape[1], 3), np.uint8)
        Image.fromarray(np.vstack([top, np.hstack([bot, pad])])).save(a.out + "_compare.png")


if __name__ == "__main__":
    main()
