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
"""Where each video board's picture goes (components/muse/muse_video_fit.c),
in tests/muse_video_fit_harness.c: a round panel's camera picture covers
the circle (two times upscaled to at least the
panel, the crop centered and only where the round mask hides it, no black
column or row inside the circle, the window on whole frame_x_align columns),
the suggested sizes (Watcher 208, StopWatch and Waveshare S3 1.75C 240,
ESP-Mosaico 480). No black bars: every
video board covers its whole screen for every size and aspect up to its max
(two times where 2x reaches the panel, else the fill's cover-scale)."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MuseVideoFitTest(unittest.TestCase):
    def test_harness(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp, "fit")
            build = subprocess.run(
                [*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 f"-I{MUSE}", "-o", str(exe), str(ROOT / "tests/muse_video_fit_harness.c"),
                 str(MUSE / "muse_video_fit.c"), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout[-4000:] + run.stderr[-4000:])
            self.assertTrue(run.stdout.rstrip().endswith("ok"), run.stdout[-2000:])
            for line in ("SenseCAP Watcher: suggests 208", "M5Stack StopWatch: suggests 240",
                         "Waveshare S3 1.75C: suggests 240", "ESP-Mosaico: suggests 480",
                         "SenseCAP Watcher, 208 px: 2x 416, crop 2,2, window 0,0 412x412",
                         "M5Stack StopWatch, 240 px: 2x 480, crop 7,7, window 0,0 466x466",
                         "Waveshare C6 1.8 fill, 128x158: 128x156 x 368/128, crop 0,0, window 0,0 368x448",
                         "no black bars: 165 board x size x kind placements cover the screen"):
                self.assertIn(line, run.stdout)


if __name__ == "__main__":
    unittest.main()
