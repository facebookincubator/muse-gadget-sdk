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
"""Changed-only redraw of call video (components/muse/muse_vdirty.c, muse_video.c's vd_* and the boards'
draw_windows). A window left out leaves a stale part of the picture on the
panel until it changes again, with nobody watching, so:

- the digests: a macroblock called unchanged is the last picture's byte for
  byte, and every changed one is caught, on real tinyh264 output (VideoToolbox
  captures, deblocked x264, UI streams), with and without the decoder's own map;
- the cells: every output pixel lies in a cell that reads its source
  macroblock, through the C6's fill scaler, on the panel's alignment;
- the windows: they cover every dirty cell, stay inside the picture, and give
  way to the whole picture when they'd cost more.
"""

from __future__ import annotations

import ctypes
import json
import os
import random
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
TINY = ROOT.parent / "xplat/tinyh264/src"
VEC = ROOT / "tests/vectors/h264"
MAX_CELLS = 32
WINDOWS, NONE, FULL_AREA, FULL_MANY = range(4)


class Axis(ctypes.Structure):
    _fields_ = [("n", ctypes.c_int), ("b", ctypes.c_int16 * (MAX_CELLS + 1)), ("m0", ctypes.c_uint8 * MAX_CELLS),
                ("m1", ctypes.c_uint8 * MAX_CELLS)]


class Cfg(ctypes.Structure):
    _fields_ = [("win_cost", ctypes.c_int), ("full_pct", ctypes.c_int), ("max_windows", ctypes.c_int)]


class Win(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int16), ("y", ctypes.c_int16), ("w", ctypes.c_int16), ("h", ctypes.c_int16)]


class Stats(ctypes.Structure):
    _fields_ = [("res", ctypes.c_int), ("dirty", ctypes.c_int), ("cells", ctypes.c_int), ("area", ctypes.c_int),
                ("total", ctypes.c_int)]


class Hash(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint32)]


def cc() -> list[str]:
    return shlex.split(os.environ.get("CC", "cc"))


def fill_src(o: int, crop: int, num: int, den: int, n: int) -> int:
    """muse_video_fill_src (components/muse/muse_video_fit.c), C's truncating division."""
    s = ((o + crop) * 2 + 1) * den
    d = 2 * num
    q = abs(s) // d * (1 if s >= 0 else -1)
    return max(0, min(n - 1, q))


class Lib(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name, "libvdirty.so")
        r = subprocess.run([*cc(), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Werror=vla", "-shared", "-fPIC",
                            f"-I{MUSE}", "-o", str(lib), str(MUSE / "muse_vdirty.c")], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.muse_vdirty_axis.restype = ctypes.c_bool
        cls.lib.muse_vdirty_plan.restype = ctypes.c_int
        cls.lib.muse_vdirty_digest.restype = ctypes.c_int
        cls.lib.muse_vdirty_model_rows.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def axis(self, out_n, src_n, num, den, crop, align) -> Axis:
        ax = Axis()
        ok = self.lib.muse_vdirty_axis(ctypes.byref(ax), out_n, src_n, num, den, crop, align)
        self.assertTrue(ok, (out_n, src_n, num, den, crop, align))
        return ax

    # ---- cells ----
    GEOMS = [
        # (out, src, num, den, crop, align): the C6's fill both ways, odd crops, the Watcher's 4, one to one, 2x.
        (368, 128, 368, 128, 0, 2), (448, 158, 368, 128, 3, 2), (368, 120, 368, 120, 0, 2),
        (448, 150, 448, 150, 0, 2), (368, 112, 448, 144, 3, 2), (448, 144, 448, 144, 1, 2),
        (412, 208, 2, 1, 2, 4), (466, 240, 2, 1, 7, 2), (480, 480, 1, 1, 0, 2), (466, 232, 2, 1, -1, 2),
        (480, 240, 2, 1, 0, 4), (448, 224, 2, 1, 0, 4),
    ]

    def test_cells_partition_the_output_and_read_every_source_macroblock(self) -> None:
        for out_n, src_n, num, den, crop, align in self.GEOMS:
            ax = self.axis(out_n, src_n, num, den, crop, align)
            b = list(ax.b[: ax.n + 1])
            self.assertEqual(b[0], 0)
            self.assertEqual(b[-1], out_n)
            self.assertEqual(b, sorted(set(b)), "cells are disjoint, in order")
            for cut in b[1:-1]:
                self.assertEqual(cut % align, 0, (out_n, src_n, cut))
            for i in range(ax.n):
                for o in range(b[i], b[i + 1]):
                    mb = fill_src(o, crop, num, den, src_n) // 16
                    self.assertTrue(ax.m0[i] <= mb <= ax.m1[i], (out_n, src_n, i, o, mb))
                self.assertLessEqual(ax.m1[i] - ax.m0[i], 1, "a cell reads at most two macroblocks")

    def test_a_changed_macroblock_marks_every_cell_that_shows_it(self) -> None:
        rng = random.Random(5)
        for out_w, src_w, num, den, cx, al in [(368, 128, 368, 128, 0, 2), (368, 120, 368, 120, 0, 2)]:
            out_h, src_h, cy = (448, 158, 3) if src_w == 128 else (448, 150, 0)
            ax = self.axis(out_w, src_w, num, den, cx, al)
            ay = self.axis(out_h, src_h, num, den, cy, 2)
            cols, rows = (src_w + 15) // 16, (src_h + 15) // 16
            for _ in range(60):
                mb = [1 if rng.random() < 0.15 else 0 for _ in range(cols * rows)]
                cells = (ctypes.c_uint8 * (ax.n * ay.n))()
                buf = (ctypes.c_uint8 * len(mb))(*mb)
                self.lib.muse_vdirty_cells(buf, cols, rows, ctypes.byref(ax), ctypes.byref(ay), cells)
                for cyi in range(ay.n):
                    for cxi in range(ax.n):
                        want = any(mb[r * cols + c] for r in range(ay.m0[cyi], ay.m1[cyi] + 1)
                                   for c in range(ax.m0[cxi], ax.m1[cxi] + 1))
                        self.assertEqual(bool(cells[cyi * ax.n + cxi]), want)

    def test_axis_refuses_what_it_cannot_cut(self) -> None:
        ax = Axis()
        self.assertFalse(self.lib.muse_vdirty_axis(ctypes.byref(ax), 0, 128, 1, 1, 0, 2))
        self.assertFalse(self.lib.muse_vdirty_axis(ctypes.byref(ax), 368, 128, 368, 128, 0, 3))   # not a power of 2
        self.assertFalse(self.lib.muse_vdirty_axis(ctypes.byref(ax), 1024, 1024, 1, 1, 0, 2))     # > 32 cells
        self.assertEqual(ax.n, 0)

    # ---- windows ----
    def plan(self, cells: list[list[int]], ax: Axis, ay: Axis, cost=2, pct=100, maxw=24):
        rows, cols = len(cells), len(cells[0])
        self.assertEqual((rows, cols), (ay.n, ax.n))
        buf = (ctypes.c_uint8 * (rows * cols))(*[v for r in cells for v in r])
        out = (Win * 32)()
        st = Stats()
        n = self.lib.muse_vdirty_plan(buf, ctypes.byref(ax), ctypes.byref(ay), ctypes.byref(Cfg(cost, pct, maxw)), out,
                                      32, ctypes.byref(st))
        return [(w.x, w.y, w.w, w.h) for w in out[:n]], st

    def test_windows_cover_every_dirty_cell_inside_the_picture(self) -> None:
        rng = random.Random(7)
        ax = self.axis(368, 128, 368, 128, 0, 2)
        ay = self.axis(448, 158, 368, 128, 3, 2)
        for trial in range(300):
            p = rng.choice([0.03, 0.1, 0.3])
            cells = [[1 if rng.random() < p else 0 for _ in range(ax.n)] for _ in range(ay.n)]
            wins, st = self.plan(cells, ax, ay, cost=rng.choice([0, 2, 6]))
            if st.res != WINDOWS:
                self.assertIn(st.res, (NONE, FULL_MANY), (trial, st.res))
                if st.res == NONE:
                    self.assertFalse(any(map(any, cells)))
                continue
            for x, y, w, h in wins:
                self.assertTrue(0 <= x and x + w <= 368 and 0 <= y and y + h <= 448, (x, y, w, h))
                self.assertEqual(x % 2, 0)
                self.assertEqual(w % 2, 0)
                self.assertEqual(y % 2, 0)
                self.assertIn(x, list(ax.b[: ax.n + 1]), "windows start on a cell")
                self.assertIn(y, list(ay.b[: ay.n + 1]))
            for cy in range(ay.n):
                for cx in range(ax.n):
                    if cells[cy][cx]:
                        x0, x1, y0, y1 = ax.b[cx], ax.b[cx + 1], ay.b[cy], ay.b[cy + 1]
                        self.assertTrue(any(x <= x0 and x1 <= x + w and y <= y0 and y1 <= y + h
                                            for x, y, w, h in wins), (trial, cx, cy))
            self.assertEqual(st.area, sum(w * h for _, _, w, h in wins))

    def test_whole_when_windows_cost_more(self) -> None:
        ax = self.axis(368, 128, 368, 128, 0, 2)
        ay = self.axis(448, 158, 368, 128, 3, 2)
        full = [[1] * ax.n for _ in range(ay.n)]
        _, st = self.plan(full, ax, ay, pct=80)
        self.assertEqual(st.res, FULL_AREA)
        apart = [[1 if r % 2 == 0 and c == 0 else 0 for c in range(ax.n)] for r in range(ay.n)]   # one per window
        _, st = self.plan(apart, ax, ay, cost=0, pct=100, maxw=3)
        self.assertEqual(st.res, FULL_MANY)
        none = [[0] * ax.n for _ in range(ay.n)]
        wins, st = self.plan(none, ax, ay)
        self.assertEqual((wins, st.res), ([], NONE))

    def test_mark_takes_every_cell_a_rectangle_touches(self) -> None:
        ax = self.axis(368, 128, 368, 128, 0, 2)
        ay = self.axis(448, 158, 368, 128, 3, 2)
        for x, y, w, h in [(34, 150, 300, 156), (0, 0, 1, 1), (367, 447, 5, 5), (-10, -10, 20, 20), (100, 100, 0, 9)]:
            cells = (ctypes.c_uint8 * (ax.n * ay.n))()
            self.lib.muse_vdirty_mark(cells, ctypes.byref(ax), ctypes.byref(ay), x, y, w, h)
            for cy in range(ay.n):
                for cx in range(ax.n):
                    hit = (w > 0 and h > 0 and ax.b[cx] < x + w and x < ax.b[cx + 1] and ay.b[cy] < y + h and
                           y < ay.b[cy + 1])
                    self.assertEqual(bool(cells[cy * ax.n + cx]), hit, (x, y, w, h, cx, cy))

    # ---- digests ----
    def digest(self, y: bytes, u: bytes, v: bytes, w: int, h: int, dg, prev_ok: bool):
        cols, rows = (w + 15) // 16, (h + 15) // 16
        chg = (ctypes.c_uint8 * (cols * rows))()
        n = self.lib.muse_vdirty_digest(y, u, v, w, w // 2, w, h, dg, prev_ok, None, 0, chg)
        return n, list(chg)

    def test_digest_catches_any_one_byte_and_chroma_alone(self) -> None:
        rng = random.Random(11)
        w, h = 120, 150   # partial macroblocks on the right and the bottom
        cols, rows = (w + 15) // 16, (h + 15) // 16
        y = bytearray(rng.randbytes(w * h))
        u = bytearray(rng.randbytes(w * h // 4))
        v = bytearray(rng.randbytes(w * h // 4))
        dg = (Hash * (cols * rows))()
        self.digest(bytes(y), bytes(u), bytes(v), w, h, dg, False)
        n, chg = self.digest(bytes(y), bytes(u), bytes(v), w, h, dg, True)
        self.assertEqual(n, 0, "the same picture: nothing changed")
        for _ in range(300):
            plane = rng.choice("yuv")
            if plane == "y":
                px, py = rng.randrange(w), rng.randrange(h)
                y[py * w + px] ^= 1 << rng.randrange(8)
                mb = (py // 16) * cols + px // 16
            else:
                px, py = rng.randrange(w // 2), rng.randrange((h + 1) // 2)
                (u if plane == "u" else v)[py * (w // 2) + px] ^= 1 << rng.randrange(8)
                mb = (py // 8) * cols + px // 8
            n, chg = self.digest(bytes(y), bytes(u), bytes(v), w, h, dg, True)
            self.assertEqual(n, 1, plane)
            self.assertEqual(chg[mb], 1, plane)

    def test_one_word_differences_never_collide(self) -> None:
        # Each lane is a bijection of the word mixed in: inputs differing in one 32-bit word always differ.
        rng = random.Random(3)
        for _ in range(2000):
            base = bytearray(rng.randbytes(64))
            other = bytearray(base)
            k = rng.randrange(16) * 4
            other[k:k + 4] = rng.randbytes(4)
            if other == base:
                continue
            h1, h2 = Hash(), Hash()
            for h, d in ((h1, base), (h2, other)):
                self.lib.muse_vdirty_hash_init(ctypes.byref(h))
                self.lib.muse_vdirty_hash_bytes(ctypes.byref(h), bytes(d), len(d))
            self.assertNotEqual((h1.a, h1.b), (h2.a, h2.b))


class RealStreams(unittest.TestCase):
    """The digests against what tinyh264 decodes (tests/muse_vdirty_h264_harness.c).

    ui_clock_466.264 and ui_edit_466.264 are this firmware's own 466 x 466 UI
    (the StopWatch's clock screen and an edit screen) coded as H.264 baseline:
    firmware screens only, no personal content.
    """

    STREAMS = ["vt_112_crop.264", "vt_80_crop.264", "testsrc2_208.264", "syn_on.264", "syn_off.264", "syn_ref2.264",
               "ui_clock_466.264", "ui_edit_466.264"]

    @classmethod
    def setUpClass(cls) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "esp_log.h").write_text("#include <stdio.h>\n#define ESP_LOGI(tag, fmt, ...) ((void)0)\n"
                                             "#define ESP_LOGW(tag, fmt, ...) ((void)0)\n")
            exe = Path(tmp, "h264")
            r = subprocess.run([*cc(), "-O1", "-w", f"-I{tmp}", f"-I{TINY}", f"-I{MUSE}", "-o", str(exe),
                                str(ROOT / "tests/muse_vdirty_h264_harness.c"), str(MUSE / "muse_vdec_same.c"),
                                str(MUSE / "muse_vdirty.c"), *sorted(str(p) for p in TINY.glob("*.c")), "-lm"],
                               capture_output=True, text=True)
            if r.returncode:
                raise AssertionError(r.stdout + r.stderr)
            run = subprocess.run([str(exe), *(str(VEC / s) for s in cls.STREAMS)], capture_output=True, text=True,
                                 timeout=300)
            if run.returncode:
                raise AssertionError(run.stdout + run.stderr)
        cls.res = {d["file"]: d for d in map(json.loads, run.stdout.strip().splitlines())}

    def test_exact_on_every_stream(self) -> None:
        for name in self.STREAMS:
            r = self.res[name]
            self.assertGreater(r["pictures"], 5, name)
            self.assertEqual(r["digest_false_same"], 0, f"{name}: a changed macroblock called unchanged: {r}")
            self.assertEqual(r["digest_false_changed"], 0, f"{name}: {r}")
            self.assertEqual(r["with_map_wrong"], 0, f"{name}: with the decoder's map as the skip list: {r}")


if __name__ == "__main__":
    unittest.main()
