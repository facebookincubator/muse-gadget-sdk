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

"""A board's read_mic records at the gain muse_audio last applied.

The loopback test steps the gain through muse_audio_set_mic_gain() without
saving it, so read_mic must not read the saved setting instead.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


AUDIO_C = Path(__file__).resolve().parents[1] / "components" / "muse" / "muse_audio.c"


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : i]
    raise AssertionError(f"could not find end of {signature}")


class MuseAudioReadMicGainTest(unittest.TestCase):
    def test_read_mic_uses_the_applied_gain(self) -> None:
        source = AUDIO_C.read_text()
        apply = _function_body(source, "static void apply_mic_gain(int db)")
        first_return = apply.find("return")
        stored = re.search(r"\b(s_\w+)\s*=\s*db\s*;", apply)
        self.assertIsNotNone(stored, "apply_mic_gain must remember the gain")
        self.assertLess(stored.start(), first_return, "gain must be kept before any early return")

        read = _function_body(source, "static esp_err_t read_input(size_t frames)")
        call = re.search(r"muse_board->read_mic\([^;]*,\s*(\w+(?:\(\))?)\s*\)", read)
        self.assertIsNotNone(call)
        self.assertEqual(call.group(1), stored.group(1))

        loopback = _function_body(source, "void muse_audio_loopback_test(int volume)")
        self.assertIn("muse_audio_set_mic_gain(gains[g]);", loopback)
        self.assertIn("read_input(", loopback)


if __name__ == "__main__":
    unittest.main()
