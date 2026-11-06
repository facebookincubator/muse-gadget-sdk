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
alt-ref (hidden) frame and repeated keyframes; its row progress hook
(tests/libvpx_vp8_harness.c); and several decoders at once, one thread each
(tests/libvpx_vp8_threads_harness.c), as the gadget's two bands run. tests/vectors/vp8/make_vectors.py makes the
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


    def test_decoders_side_by_side(self) -> None:
        """Every stream decoded on its own thread at once, as the gadget runs two bands
        on two cores: each must match decoding it alone (no state shared between
        decoder instances; tests/libvpx_vp8_threads_harness.c)."""
        exe = Path(self.tmp.name, "vp8threads")
        cc = os.environ.get("CC", "cc")
        srcs = sorted(str(p) for p in SRC.rglob("*.c"))
        for border in (8, 16):   # the S3 and S31 build with 16-pixel borders
            with self.subTest(border=border):
                build = subprocess.run(
                    [cc, "-O2", "-w", f"-DVP8_MGX_BORDER={border}", f"-I{SRC}", "-o", str(exe),
                     str(ROOT / "tests/libvpx_vp8_threads_harness.c"), *srcs, "-lm", "-lpthread"],
                    capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                streams = sorted(str(p) for p in VEC.glob("*.ivf"))
                self.assertGreaterEqual(len(streams), 2)
                run = subprocess.run([str(exe), "10", *streams[:8]], capture_output=True, text=True, timeout=300)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("threads ok", run.stdout)


if __name__ == "__main__":
    unittest.main()
