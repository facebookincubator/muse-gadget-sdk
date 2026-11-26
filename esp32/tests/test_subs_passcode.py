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

"""Subtitles never draw over the passcode's screens: the path a phone's subtitle messages take
(muse_subs_event: state 1, words shown at once, then muse_subs_tick unhiding the tall band on lv_layer_sys) shows no
text while the gadget is locked, a phone's call held behind the keypad included; the model keeps the words, and the
latest band shows once it's unlocked with the code. The real muse_subs.c, muse_passcode.c and muse_ui.c in the
simulator on every touch board with subtitles; the video overlay's gate in the source."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
SIM = ROOT / "simulator" / "build" / "muse_simulator"
BOARDS = ("stopwatch", "w175", "watcher", "mosaico")
BASE = "face=idle\ncaption=\nwifi=connected\nble=connected\npaired=true\nbattery=72\nusb=false\nclock=10:09\n"
TEXT = "Your package arrives tomorrow"


def run(test: unittest.TestCase, board: str, scenario: str) -> list[list[str]]:
    """The scenario on one board: each dump's labels, the system layer's included."""
    if not SIM.exists():
        test.skipTest("the simulator isn't built (simulator/README.md)")
    with tempfile.TemporaryDirectory(prefix="subs-lock-") as tmp:
        sc = os.path.join(tmp, "s.txt")
        with open(sc, "w") as f:
            f.write(BASE + scenario)
        env = dict(os.environ, MUSE_SIM_BOARD=board, SDL_VIDEODRIVER="dummy")
        r = subprocess.run([str(SIM), "--headless", "--run-ms", "1", "--scenario", sc, "--screenshot",
                            os.path.join(tmp, "o.ppm")], env=env, capture_output=True, text=True, timeout=60)
    test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
    dumps: list[list[str]] = []
    for line in r.stdout.splitlines():
        if line.startswith("@page"):
            dumps.append([])
        elif dumps and line.startswith("@label "):
            dumps[-1].append(line[len("@label "):].replace(" / ", " "))
    return dumps


def shows(labels: list[str]) -> bool:
    return any(TEXT in ln for ln in labels)


class OnTheBoards(unittest.TestCase):
    def test_a_subtitle_while_locked_stays_hidden_and_shows_after_the_unlock(self) -> None:
        for board in BOARDS:
            with self.subTest(board=board):
                locked, unlocked = run(self, board, "passcode=1234\npasslock=true\nadvance=900\n"
                                                    f"subs={TEXT}\nadvance=600\ndump=sys\n"
                                                    "passenter=1234\nadvance=600\ndump=sys\n")
                self.assertFalse(shows(locked), locked)
                self.assertTrue(shows(unlocked), unlocked)

    def test_the_lock_takes_a_shown_band_away(self) -> None:
        for board in BOARDS:
            with self.subTest(board=board):
                shown, locked = run(self, board, f"passcode=1234\nadvance=600\nsubs={TEXT}\nadvance=600\ndump=sys\n"
                                                 "passlock=true\nadvance=600\ndump=sys\n")
                self.assertTrue(shows(shown), shown)
                self.assertFalse(shows(locked), locked)

class InTheSource(unittest.TestCase):
    def test_every_drawing_path_asks_the_lock(self) -> None:
        src = (MUSE / "muse_subs.c").read_text()

        def body(head: str) -> str:
            i = src.index(head)
            return src[i:src.index("\n}\n", i)]

        self.assertIn("if (!muse_subs_visible(&s_m) || muse_passcode_locked()) {", body("static void render_locked(void)"))
        self.assertIn("s_shown_gen = ~0u;   /* hidden at the lock", body("static void refresh_locked(int64_t now)"))
        for head in ("void muse_subs_overlay(uint8_t *dst", "bool muse_subs_overlay_rect("):
            self.assertRegex(body(head), re.escape("if (f < 0 || !s_ready || muse_passcode_locked() || "))

    def test_a_locked_gadget_is_not_woken_by_subtitles(self) -> None:
        """State 1 never wakes a locked gadget: the wake asks the lock. There's no proof gate on subtitles;
        mg_proto_harness's test_subtitles has a set-up gadget take an unproven connection's state and words."""
        subs = (MUSE / "muse_subs.c").read_text()
        i = subs.index('muse_input_wake("subtitles");')
        self.assertIn("if (ev->active && ev->proven && !muse_passcode_locked()) {", subs[subs.rindex("\n        if ", 0, i):i])


if __name__ == "__main__":
    unittest.main()
