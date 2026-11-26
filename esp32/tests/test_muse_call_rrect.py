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

"""The call controls drawn shape by shape (muse_call_controls.h):
muse_call_rrect gives the same pixels as blending muse_call_rr_cover into every pixel, for circles, pills and
rounded rectangles cut into pieces anywhere, and the blend's weights stay within one step of an exact one."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_call_controls.h"
int main(void)
{
    static unsigned char a[64 * 400 * 2], b[64 * 400 * 2];
    int bad = 0, cases = 0;
    srand(5);
    for (int t = 0; t < 4000; t++) {
        int w = 1 + rand() % 400, rows = 1 + rand() % 64, px = rand() % 300 - 100, py = rand() % 200 - 60;
        int x0 = rand() % 300, y0 = rand() % 160, ww = 1 + rand() % 200, hh = 1 + rand() % 80;
        int rad = rand() % 4 == 0 ? 0 : rand() % ((ww < hh ? ww : hh) / 2 + 1);
        unsigned long rgb = (unsigned long)rand() << 8 ^ (unsigned long)rand();
        rgb &= 0xffffff;
        for (size_t i = 0; i < sizeof(a); i++) a[i] = b[i] = (unsigned char)rand();
        int stride = w * 2;
        muse_call_rrect(a, stride, px, py, w, rows, x0, y0, x0 + ww, y0 + hh, rad, rgb);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < w; c++) {
                muse_call_blend_rgb(b + r * stride + c * 2, rgb,
                                    muse_call_rr_cover(px + c, py + r, x0, y0, x0 + ww, y0 + hh, rad));
            }
        }
        if (memcmp(a, b, (size_t)rows * stride)) {
            if (bad++ < 5) printf("case %d differs: piece %d,%d %dx%d rect %d,%d %dx%d r %d\n", t, px, py, w, rows,
                                  x0, y0, ww, hh, rad);
        }
        cases++;
    }
    /* The blend: within one 565 step of the exact (c a + d (255 - a)) / 255 per channel. */
    for (unsigned av = 0; av < 256; av++) {
        for (unsigned dv = 0; dv < 32; dv++) {
            unsigned char d[2] = { (unsigned char)((dv << 11) >> 8), (unsigned char)(dv << 11) };
            muse_call_blend_rgb(d, 0xf80000, av);   /* red 31 over red dv */
            unsigned got = ((unsigned)d[0] << 8 | d[1]) >> 11, want = (31 * av + dv * (255 - av) + 127) / 255;
            if ((got > want ? got - want : want - got) > 1 && bad++ < 10) printf("blend a %u d %u: %u vs %u\n", av, dv, got, want);
        }
    }
    printf("%d cases, %s\n", cases, bad ? "BAD" : "ok");
    return bad != 0;
}
'''


class MuseCallRrectTest(unittest.TestCase):
    def test_rrect(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "h.c"), Path(t, "h")
            src.write_text(HARNESS)
            r = subprocess.run([*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                                "-I", str(MUSE), str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)

if __name__ == "__main__":
    unittest.main()
