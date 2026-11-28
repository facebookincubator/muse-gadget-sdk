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
"""tools/muse/shot.py and uidump.py read a frame's
size from a gadget's console (or a saved log) and allocated for it before
checking anything: `@shot 65536 65536` asked for two 8 GiB buffers,
`@uidump 65536 65536 0` ~14 GiB of image planes. Both must refuse such a
header before allocating (watched: tracemalloc for shot.py, a numpy stand-in
that records every allocation for uidump.py, which needs numpy and Pillow),
and still read real dumps."""
import base64
import contextlib
import io
import sys
import tempfile
import tracemalloc
import types
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools/muse"
sys.path.insert(0, str(TOOLS))


def shot_text(w, h, fill=0x12):
    data = bytes([fill]) * (w * h * 2)
    lines = [f"@shot {w} {h}"]
    for off in range(0, len(data), 48):
        lines.append(f"@s {off:x} {base64.b64encode(data[off:off + 48]).decode()}")
    return "\n".join(lines + ["@shot end", ""])


class Shot(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        import shot
        cls.shot = shot

    def refuses(self, text):
        tracemalloc.start()
        try:
            with self.assertRaises(SystemExit) as e:
                self.shot.parse(text)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 1 << 20, "allocated before refusing")
        return str(e.exception.code)

    def test_report_header_refused_before_allocating(self):
        self.assertIn("refusing a 65536x65536", self.refuses("@shot 65536 65536\n@shot end\n"))

    def test_zero_and_tall(self):
        for w, h in ((0, 16), (16, 0), (1, 4097), (4096, 4096)):
            with self.subTest(w=w, h=h):
                self.assertIn("refusing", self.refuses(f"@shot {w} {h}\n@shot end\n"))

    def test_real_shot_and_stray_offset(self):
        text = shot_text(8, 4).replace("@shot end", "@s ffffffff AAAA\n@shot end")   # a line past the frame
        w, h, data, got = self.shot.parse(text)
        self.assertEqual((w, h, len(data), all(got)), (8, 4, 64, True))
        self.assertEqual(bytes(data), bytes([0x12]) * 64)


class FakeArray:
    def __init__(self, shape):
        self.shape = shape

    def __setitem__(self, k, v):
        pass


class UiDump(unittest.TestCase):
    """uidump.py needs numpy and Pillow: a stand-in numpy records what's allocated."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.allocs = []
        np = types.ModuleType("numpy")
        np.uint8, np.uint16, np.float64 = "uint8", "uint16", "float64"
        np.bool_ = bool

        def zeros(shape, dtype=None):
            cls.allocs.append(shape)
            return FakeArray(shape if isinstance(shape, tuple) else (shape,))
        np.zeros = zeros
        np.frombuffer = lambda b, dtype=None: b
        pil = types.ModuleType("PIL")
        pil.Image = types.ModuleType("PIL.Image")
        saved = {k: sys.modules.get(k) for k in ("numpy", "PIL", "PIL.Image", "uidump")}
        sys.modules.update({"numpy": np, "PIL": pil, "PIL.Image": pil.Image})
        sys.modules.pop("uidump", None)
        try:
            import uidump
            cls.uidump = uidump
        finally:
            for k, v in saved.items():
                if v is None:
                    sys.modules.pop(k, None)
                else:
                    sys.modules[k] = v

    def setUp(self):
        self.allocs.clear()
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def log(self, text):
        p = Path(self.tmp.name) / "con.log"
        p.write_text(text)
        return str(p)

    def refuses(self, text):
        with self.assertRaises(SystemExit) as e, contextlib.redirect_stderr(io.StringIO()):
            self.uidump.parse(self.log(text))
        self.assertEqual(self.allocs, [], "allocated before refusing")
        return str(e.exception.code)

    def test_report_header_refused_before_allocating(self):
        self.assertIn("refusing a 65536x65536", self.refuses("@uidump 65536 65536 0\n@uidump end\n"))

    def test_odd_zero_and_hq(self):
        for hdr in ("@uidump 465 466 0", "@uidump 466 465 0", "@uidump 0 466 0", "@uidump 466 466 7",
                    "@uidump 466 466"):
            with self.subTest(hdr=hdr):
                self.refuses(hdr + "\n@uidump end\n")

    def test_bad_rows_skipped(self):
        b64 = base64.b64encode(bytes(4)).decode()
        text = "\n".join(["@uidump 4 2 1", f"@Y 0 {b64}", "@Y x " + b64, "@Y 99 " + b64, "@U 0 !!notbase64",
                          f"@P 0 {base64.b64encode(bytes(8)).decode()}", "@uidump end", ""])
        with contextlib.redirect_stderr(io.StringIO()):
            w, h, hq, *_ = self.uidump.parse(self.log(text))
        self.assertEqual((w, h, hq), (4, 2, 1))
        self.assertEqual(self.allocs[:4], [(2, 4), (1, 2), (1, 2), (2, 8)])   # Y, U, V, panel at the header's size


if __name__ == "__main__":
    unittest.main()
