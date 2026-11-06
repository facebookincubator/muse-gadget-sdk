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

"""The in-place WebSocket masking of ws_send_frame_inplace()."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WsMaskTest(unittest.TestCase):
    def test_word_masking_matches_the_byte_loop(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        for opt in ("-O0", "-O2"):
            with tempfile.TemporaryDirectory() as tmp:
                binary = Path(tmp) / "ws_mask"
                built = subprocess.run(
                    [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", opt,
                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                     "-I", str(ROOT / "main"), str(ROOT / "tests" / "ws_mask_harness.c"),
                     "-o", str(binary)],
                    capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                self.assertIn("ws_mask ok", ran.stdout)

    def test_frame_sender_uses_it(self) -> None:
        source = (ROOT / "main" / "noise_control.cpp").read_text()
        body = source[source.index("static bool ws_send_frame_inplace("):]
        body = body[: body.index("\n}\n")]
        self.assertIn("ws_mask_inplace(payload, len,", body)


if __name__ == "__main__":
    unittest.main()
