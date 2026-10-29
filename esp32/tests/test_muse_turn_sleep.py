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

"""An active turn counts as activity for auto-sleep (issue #281).

The idle clock behind check_sleep() only moved on physical input, so a
long turn could use up the whole sleep timeout while thinking/speaking
(modes that block sleep), and the screen then blanked at the first IDLE
moment: between messages, on an error caption, or while the final
caption was still being read. The voice task now pokes the idle clock
while a reply is delivered and when a turn returns to idle.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VOICE = ROOT / "components" / "muse" / "muse_voice.c"
INPUT = ROOT / "components" / "muse" / "muse_input.c"


def body(source: str, signature: str) -> str:
    """The function body following `signature`, braces balanced."""
    match = re.search(re.escape(signature) + r"\s*\{", source)
    if not match:
        raise AssertionError(f"no definition found for {signature!r}")
    open_at = match.end() - 1
    depth = 0
    for i in range(open_at, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[open_at + 1 : i]
    raise AssertionError(f"unbalanced braces after {signature!r}")


class TurnSleepTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.voice = VOICE.read_text()
        cls.input = INPUT.read_text()

    def test_reply_loop_pokes_the_idle_clock(self):
        reply = body(self.voice, "static bool hatch_reply(bool *delivered)")
        loop = reply[reply.index("for (;;)") :]
        self.assertIn("muse_state_poke()", loop)

    def test_turn_end_restarts_the_reading_clock(self):
        idle = body(self.voice, "static void go_idle(const char *caption)")
        self.assertIn("muse_state_poke()", idle)
        self.assertLess(idle.index("muse_state_poke()"),
                        idle.index("muse_state_set_mode(MUSE_MODE_IDLE)"))

    def test_sleep_still_requires_idle_mode_and_timeout(self):
        check = body(self.input, "static void check_sleep(void)")
        self.assertIn("muse_state_mode(&mode_t) == MUSE_MODE_IDLE", check)
        self.assertIn("muse_state_idle_secs() > after", check)
        self.assertIn('set_asleep(true, "auto-sleep")', check)


if __name__ == "__main__":
    unittest.main()
