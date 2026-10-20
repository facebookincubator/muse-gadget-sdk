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

"""Muse Gadgets Test's H.264 clip loops from x264 (pictures past 240 px):
the keyed loop starts with the clip's only IDR, and the steady loop, which
plays over and over after it, must have none. x264 makes an IDR at a scene
cut, and the steady loop starts where the clip wraps around, which is one
for most clips (Big Buck Bunny's and Sintel's excerpts), so the apps turn
scene cuts off (scenecut=0); without it they refused those clips ("encoder
put a keyframe in the steady loop").

Encodes with the apps' x264 parameters, read from their ClipStreamer
sources, through the apps' x264 glue (ios/musegadgetstest/native/mgx264.c) and the
host's libx264 (tests/musegadgetstest_x264_loop_harness.c; skipped without it)."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
APPS = {
    "android": REPO / "android/musegadgetstest/src/com/musegadgets/test/ClipStreamer.java",
    "ios": REPO / "ios/musegadgetstest/Sources/ClipStreamer.swift",
}
SIZE, FRAMES = 96, 48


def clip_params(app: str) -> str:
    """The x264 options string encodeLoopsX264 passes, with sample rates."""
    text = APPS[app].read_text()
    m = re.search(r'"(no-deblock=1:[^"]*?vbv-maxrate=)', text)
    assert m, f"{app}: encodeLoopsX264's x264 options not found"
    return m.group(1) + "612:vbv-bufsize=183"


def libx264_flags() -> list[str] | None:
    if not shutil.which("pkg-config"):
        return None
    run = subprocess.run(["pkg-config", "--cflags", "--libs", "x264"], capture_output=True, text=True)
    return run.stdout.split() if run.returncode == 0 else None


class MuseGadgetsTestX264LoopsTest(unittest.TestCase):
    def test_apps_turn_scene_cuts_off(self) -> None:
        for app in APPS:
            with self.subTest(app=app):
                self.assertIn(":scenecut=0:", clip_params(app))

    def test_steady_loop_has_no_idr(self) -> None:
        flags = libx264_flags()
        if flags is None:
            self.skipTest("libx264 not found by pkg-config")
        cc = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory() as tmp:
            enc = Path(tmp, "enc")
            build = subprocess.run(
                [cc, "-O2", "-w", f"-I{REPO / 'ios/musegadgetstest/native'}", "-o", str(enc),
                 str(ROOT / "tests/musegadgetstest_x264_loop_harness.c"), str(REPO / "ios/musegadgetstest/native/mgx264.c"),
                 *flags], capture_output=True, text=True)
            if build.returncode:
                self.skipTest("can't build against the host's libx264: " + (build.stderr or build.stdout)[-300:])
            for app in APPS:
                with self.subTest(app=app):
                    run = subprocess.run([str(enc), clip_params(app), str(SIZE), str(FRAMES)],
                                         capture_output=True, text=True, timeout=120)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertEqual(run.stdout.strip(), "IDR at keyed loop frame 0")
            # Scene cuts on, the same clip gets IDRs in its steady loop: the test sees the
            # failure it guards against.
            run = subprocess.run([str(enc), clip_params("android").replace(":scenecut=0:", ":"), str(SIZE),
                                  str(FRAMES)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
            self.assertIn("IDR at steady loop frame 0", run.stdout)


if __name__ == "__main__":
    unittest.main()
