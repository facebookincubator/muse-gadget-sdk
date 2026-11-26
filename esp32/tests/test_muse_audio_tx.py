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

"""The speaker's DMA, cleared at a barge-in's touch-down (muse_audio_tx.h).

The barge-in echo rule mutes a reply at the talk
press's touch-down, but the ~30 ms already in the I2S TX DMA still played into
the pre-roll (+5.1 dB over the room in a barge-in's first 100 ms at volume 30).
The clear silences it in place (faded over 4 ms after a 2 ms margin, the
bus and the mic's clock never stopping) and hands back the samples it held
unplayed, for a tap's resume (no sample lost).

tests/muse_audio_tx_harness.c runs the real core against a model of ESP-IDF
v6.0.1's TX driver: every written sample numbered, what the speaker plays and
what a clear keeps checked sample by sample.
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"


class SpeakerClearTest(unittest.TestCase):
    def test_against_the_driver_model(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "run"
            build = subprocess.run(
                cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(MUSE),
                      str(ROOT / "tests/muse_audio_tx_harness.c"), str(MUSE / "muse_audio_tx.c"), "-lm",
                      "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        for line in ("steady: 500 clears", "0 repeated at most, silent after 6 ms, resume exact",
                     "late ISR (0-4 ms) and merged events: never a sample skipped", "after idle: exact",
                     "rest and wake: exact", "unaligned writes: exact", "stopped or unknown: untouched",
                     "no click"):
            self.assertIn(line, run.stdout)
        # The clear hands back most of the 30 ms DMA (what's past the read position and the margin).
        m = re.search(r"kept (\d+)-(\d+) frames", run.stdout)
        self.assertIsNotNone(m)
        self.assertGreaterEqual(int(m.group(1)), 2 * 160 - 32 - 1)
        self.assertLessEqual(int(m.group(2)), 3 * 160)


if __name__ == "__main__":
    unittest.main()
