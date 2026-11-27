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

"""xplat/libvpx_vp8 on the host: the trimmed, sped-up decoder against
stock libvpx (MD5s of its output in tests/vectors/vp8/md5.txt, from libvpx
v1.16.0's vpxdec) on streams covering versions 0 to 3, the normal, simple and
no loop filter, segmentation, four token partitions, error resilience, an
alt-ref (hidden) frame and repeated keyframes, also built with VPX_XPULP
(rv_xpulp.h's plain-C twins); its row progress hook
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
SRC = ROOT.parent / "xplat/libvpx_vp8/src"
VEC = ROOT / "tests/vectors/vp8"


ALLOC_FAIL_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
/* vpx_mem.c is built with malloc -> vp8_test_malloc: picture-sized blocks
 * fail while fail_big counts down. */
static int fail_big;
void *vp8_test_malloc(size_t n)
{
    if (n >= 65536 && fail_big > 0) {
        fail_big--;
        return NULL;
    }
    return malloc(n);
}
static uint32_t sum(const vpx_image_t *img)
{
    uint32_t s = 2166136261u;
    for (int p = 0; p < 3; p++) {
        int w = p ? (int)(img->d_w + 1) / 2 : (int)img->d_w, h = p ? (int)(img->d_h + 1) / 2 : (int)img->d_h;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) s = (s ^ img->planes[p][y * img->stride[p] + x]) * 16777619u;
    }
    return s;
}
/* IN.ivf FAILS: decodes frame 0, then frames 1..FAILS each with its picture
 * allocation failing, then the rest; then the same stream without frames
 * 1..FAILS in a fresh decoder. Prints a checksum line per shown picture of
 * each run. */
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    int fails = atoi(argv[2]);
    for (int run = 0; run < 2; run++) {
        vpx_codec_ctx_t d;
        vpx_codec_dec_cfg_t cfg = { 1, 0, 0 };
        if (vpx_codec_dec_init(&d, vpx_codec_vp8_dx(), &cfg, 0)) return 3;
        long i = 32;
        for (int k = 0; i + 12 <= n; k++) {
            uint32_t sz = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
            const uint8_t *fr = b + i + 12;
            i += 12 + sz;
            if (k >= 1 && k <= fails) {
                if (run) continue;
                fail_big = 1;
                int rc = vpx_codec_decode(&d, fr, sz, NULL, 0);
                printf("run %d frame %d failed %d\n", run, k, rc != 0 && fail_big == 0);
                fail_big = 0;
                continue;
            }
            if (vpx_codec_decode(&d, fr, sz, NULL, 0)) {
                printf("run %d frame %d error\n", run, k);
                continue;
            }
            vpx_codec_iter_t it = NULL;
            vpx_image_t *img = vpx_codec_get_frame(&d, &it);
            if (img && k > fails) printf("pic %d %08x\n", k, sum(img));
        }
        vpx_codec_destroy(&d);
    }
    return 0;
}
"""


class LibvpxVp8Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name, "vp8dec")
        cls.exe_xpulp = Path(cls.tmp.name, "vp8dec_xpulp")
        cc = os.environ.get("CC", "cc")
        srcs = sorted(str(p) for p in SRC.rglob("*.c"))
        # VPX_XPULP with rv_xpulp.h's plain-C twins: the same pictures
        for exe, flags in ((cls.exe, []), (cls.exe_xpulp, ["-DVPX_XPULP=1", f"-I{ROOT.parent / 'xplat/include'}"])):
            build = subprocess.run(
                [cc, "-O2", "-w", *flags, f"-I{SRC}", "-o", str(exe), str(ROOT / "tests/libvpx_vp8_harness.c"), *srcs,
                 "-lm"],
                capture_output=True, text=True)
            if build.returncode:
                raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_bit_exact_with_libvpx(self) -> None:
        want = dict(line.split() for line in (VEC / "md5.txt").read_text().splitlines() if line.strip())
        self.assertGreaterEqual(len(want), 7)
        for exe in (self.exe, self.exe_xpulp):
            for name, md5 in want.items():
                with self.subTest(build=exe.name, stream=name):
                    out = Path(self.tmp.name, name + ".yuv")
                    run = subprocess.run([str(exe), str(VEC / name), str(out)], capture_output=True, text=True,
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


    def test_picture_allocation_failure_keeps_references(self) -> None:
        """A picture allocation that fails mid-stream (pictures are allocated
        on first use) releases its own reservation and nothing else: after
        three such failures the decoder goes on exactly as one that never saw
        those frames. Before, each failure released the last frame's
        reference instead, until a frame decoded into its own reference (or
        the slot table ran out)."""
        exe = Path(self.tmp.name, "vp8allocfail")
        harness = Path(self.tmp.name, "allocfail.c")
        harness.write_text(ALLOC_FAIL_HARNESS)
        cc = os.environ.get("CC", "cc")
        mem = SRC / "vpx_mem/vpx_mem.c"
        srcs = sorted(str(p) for p in SRC.rglob("*.c") if p != mem)
        obj = Path(self.tmp.name, "vpx_mem.o")
        steps = [[cc, "-O1", "-g", "-w", "-Dmalloc=vp8_test_malloc", f"-I{SRC}", "-c", str(mem), "-o", str(obj)],
                 [cc, "-O1", "-g", "-w", f"-I{SRC}", "-o", str(exe), str(harness), str(obj), *srcs, "-lm"]]
        for cmd in steps:
            build = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        run = subprocess.run([str(exe), str(VEC / "p0_rt_normallf.ivf"), "3"], capture_output=True, text=True,
                             timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        lines = run.stdout.splitlines()
        self.assertEqual([l for l in lines if l.startswith("run 0 frame")],
                         [f"run 0 frame {k} failed 1" for k in (1, 2, 3)])
        pics = [l for l in lines if l.startswith("pic ")]
        half = len(pics) // 2
        self.assertGreater(half, 10)
        self.assertEqual(pics[:half], pics[half:])
        self.assertNotIn("error", run.stdout)


if __name__ == "__main__":
    unittest.main()
