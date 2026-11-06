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

"""Muse Gadgets Test's VP8 clip loops, as a gadget plays them: the keyed
loop, then steady loops back to back with no decoder reset. Each steady loop
was encoded to follow the keyed loop, and VP8 carries its entropy
probabilities from frame to frame unless the encoder is error resilient, so
without the apps' er=1 the second steady loop in a row is parsed with the
wrong probabilities (garbage until the next keyframe, on any decoder).

Encodes with the apps' encoder glue (ios/musegadgetstest/native/mgvpx.c, the same
as android/musegadgetstest/jni/mgvpx.c) and the host's libvpx
(tests/musegadgetstest_vp8_loop_harness.c; skipped without libvpx's encoder), and
decodes with components/libvpx_vp8 (tests/libvpx_vp8_harness.c)."""

from __future__ import annotations

import math
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SRC = ROOT / "components/libvpx_vp8/src"
APPS = {
    "android": REPO / "android/musegadgetstest/src/com/musegadgets/test/ClipStreamer.java",
    "ios": REPO / "ios/musegadgetstest/Sources/ClipStreamer.swift",
}
GLUE = [REPO / "android/musegadgetstest/jni/mgvpx.c", REPO / "ios/musegadgetstest/native/mgvpx.c"]
# CameraStreamer.vp8Params at 480 px (both apps) plus ClipStreamer's clip settings.
PARAMS = "cpu=2:deadline=good:profile=1:lf=0:mvcheap=1:threads=1:vbr=1:vbv-ms=300"
SIZE, FRAMES, STEADY = 96, 24, 4


def libvpx_flags() -> list[str] | None:
    if not shutil.which("pkg-config"):
        return None
    run = subprocess.run(["pkg-config", "--cflags", "--libs", "vpx"], capture_output=True, text=True)
    return run.stdout.split() if run.returncode == 0 else None


def psnr_y(a: bytes, b: bytes) -> float:
    e = sum((x - y) * (x - y) for x, y in zip(a, b))
    return 99.0 if e == 0 else 10 * math.log10(255 * 255 * len(a) / e)


class MuseGadgetsTestVp8LoopsTest(unittest.TestCase):
    def test_apps_encode_clip_loops_error_resilient(self) -> None:
        for app, path in APPS.items():
            with self.subTest(app=app):
                text = path.read_text()
                self.assertIn(':vbr=1:vbv-ms=300:er=1"', text)
        for path in GLUE:
            with self.subTest(glue=str(path.relative_to(REPO))):
                self.assertIn('opt(params, "er", 0) ? VPX_ERROR_RESILIENT_DEFAULT : 0', path.read_text())

    def test_steady_loops_back_to_back(self) -> None:
        flags = libvpx_flags()
        if flags is None:
            self.skipTest("libvpx (with its encoder) not found by pkg-config")
        cc = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory() as tmp:
            enc, dec = Path(tmp, "enc"), Path(tmp, "dec")
            build = subprocess.run(
                [cc, "-O2", "-w", f"-I{REPO / 'ios/musegadgetstest/native'}", "-o", str(enc),
                 str(ROOT / "tests/musegadgetstest_vp8_loop_harness.c"), str(REPO / "ios/musegadgetstest/native/mgvpx.c"),
                 *flags], capture_output=True, text=True)
            if build.returncode:
                self.skipTest("can't build against the host's libvpx: " + (build.stderr or build.stdout)[-300:])
            srcs = sorted(str(p) for p in SRC.rglob("*.c"))
            build = subprocess.run(
                [cc, "-O2", "-w", f"-I{SRC}", "-o", str(dec), str(ROOT / "tests/libvpx_vp8_harness.c"), *srcs, "-lm"],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            worst = {}
            for er in (1, 0):
                ivf, yuv = Path(tmp, f"er{er}.ivf"), Path(tmp, f"er{er}.yuv")
                run = subprocess.run([str(enc), f"{PARAMS}:er={er}", str(SIZE), str(FRAMES), str(STEADY), str(ivf)],
                                     capture_output=True, text=True, timeout=120)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                run = subprocess.run([str(dec), str(ivf), str(yuv)], capture_output=True, text=True, timeout=120)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                data = yuv.read_bytes()
                fs = SIZE * SIZE * 3 // 2
                self.assertEqual(len(data), fs * FRAMES * (1 + STEADY))
                # The first steady loop is what the encoder meant (it follows the keyed loop);
                # every later one should look the same, give or take reference drift.
                first = [data[fs * (FRAMES + i):fs * (FRAMES + i) + SIZE * SIZE] for i in range(FRAMES)]
                worst[er] = min(psnr_y(data[fs * (FRAMES * loop + i):fs * (FRAMES * loop + i) + SIZE * SIZE], first[i])
                                for loop in range(2, 1 + STEADY) for i in range(FRAMES))
            self.assertGreater(worst[1], 30.0, f"steady loops drift: worst {worst[1]:.1f} dB with er=1")
            # Without it the same loops break: the test sees the failure it guards against.
            self.assertLess(worst[0], 25.0, f"expected garbage without er=1, worst {worst[0]:.1f} dB")


if __name__ == "__main__":
    unittest.main()
