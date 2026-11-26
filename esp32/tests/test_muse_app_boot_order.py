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

"""muse_app_run() opens audio before Muse's session and Wi-Fi start (there is
no boot animation pause: the fast start). Link's Wi-Fi and TLS take internal
RAM as they come up; opened after them, the I2S DMA buffers can fail to
allocate and i2s_rx_channel_start() crashes on boot."""

from __future__ import annotations

import unittest
from pathlib import Path


APP = Path(__file__).resolve().parents[1] / "components" / "muse" / "muse_app.c"


def body(source: str, signature: str) -> str:
    """The braces after `signature`."""
    start = source.index("{", source.index(signature))
    depth = 0
    for i in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        if depth == 0:
            return source[start : i + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


class MuseAppBootOrderTest(unittest.TestCase):
    def test_audio_starts_before_the_boot_pause(self) -> None:
        run = body(APP.read_text(), "void muse_app_run(")
        self.assertEqual(run.count("muse_voice_start("), 1)
        self.assertLess(run.index("muse_voice_start("), run.index("muse_hatch_start()"))
        self.assertLess(run.index("muse_voice_start("), run.index("muse_wifi_apply()"))

    def test_display_only_boards_still_skip_audio(self) -> None:
        run = body(APP.read_text(), "void muse_app_run(")
        self.assertIn("has_audio ? muse_voice_start(q) : ESP_OK", run)


if __name__ == "__main__":
    unittest.main()
