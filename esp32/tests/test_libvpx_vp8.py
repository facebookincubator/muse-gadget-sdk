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

"""components/libvpx_vp8 on the host: the trimmed, sped-up decoder against
stock libvpx (MD5s of its output in tests/vectors/vp8/md5.txt, from libvpx
v1.16.0's vpxdec) on streams covering versions 0 to 3, the normal, simple and
no loop filter, segmentation, four token partitions, error resilience, an
alt-ref (hidden) frame and repeated keyframes; and its row progress hook
(tests/libvpx_vp8_harness.c). tests/vectors/vp8/make_vectors.py makes the
streams from a synthetic picture."""

from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "components/libvpx_vp8/src"
VEC = ROOT / "tests/vectors/vp8"


class LibvpxVp8Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name, "vp8dec")
        cc = os.environ.get("CC", "cc")
        srcs = sorted(str(p) for p in SRC.rglob("*.c"))
        build = subprocess.run(
            [cc, "-O2", "-w", f"-I{SRC}", "-o", str(cls.exe), str(ROOT / "tests/libvpx_vp8_harness.c"), *srcs, "-lm"],
            capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_bit_exact_with_libvpx(self) -> None:
        want = dict(line.split() for line in (VEC / "md5.txt").read_text().splitlines() if line.strip())
        self.assertGreaterEqual(len(want), 7)
        for name, md5 in want.items():
            with self.subTest(stream=name):
                out = Path(self.tmp.name, name + ".yuv")
                run = subprocess.run([str(self.exe), str(VEC / name), str(out)], capture_output=True, text=True,
                                     timeout=120)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("progress ok", run.stdout)
                self.assertEqual(hashlib.md5(out.read_bytes()).hexdigest(), md5)


if __name__ == "__main__":
    unittest.main()
