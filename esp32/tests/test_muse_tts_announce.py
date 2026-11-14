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

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MuseTtsAnnounceTest(unittest.TestCase):
    def test_announce_check(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "muse_tts_announce"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")),
                 "-include", str(ROOT / "tests" / "host_compat.h"),
                 "-std=gnu11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components" / "muse"),
                 str(ROOT / "tests" / "muse_tts_announce_harness.c"),
                 str(ROOT / "components" / "muse" / "muse_tts_text.c"),
                 str(ROOT / "components" / "muse" / "muse_text.c"),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)


if __name__ == "__main__":
    unittest.main()
