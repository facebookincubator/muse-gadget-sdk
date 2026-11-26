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

"""What a central without the owner's token proof can do to the screen through Live call subtitles and call reads.

Subtitles and call states stay open to an unproven central, within limits:
- A card that asks for a press or a code (the pairing confirm, the update prompt, the re-setup prompt, a passcode
  keypad) always draws above every subtitle and call layer: the tall band (on lv_layer_sys, above the card's
  lv_layer_top) is hidden while one is up, the video's band and the call's controls aren't drawn, and the update
  prompt's confirm counts the band as covering the card. The real muse_subs.c, muse_call.c and muse_ui.c in the
  simulator on every touch board with subtitles, with a re-setup prompt's Confirm card (the update prompt is the
  same card, s_pair, and the same sync_card; the simulator has no BLE update path) and an unproven subtitle stream.
  An unproven central's words go as the card comes up and never come back after it, so no lure outlives a prompt;
  the owner's come back.
- An unproven central's subtitles never wake the screen, keep it lit or hold the panel (so auto-sleep and the
  passcode's auto-lock run as usual); they show only on a screen that's already on.
- `!IN call?` is a read: it never counts as activity.
"""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
SIM = Path(os.environ.get("MUSE_SIMULATOR", ROOT / "simulator" / "build" / "muse_simulator"))
BOARDS = ("stopwatch", "w175", "watcher", "mosaico")
BASE = "face=idle\ncaption=\nwifi=connected\nble=connected\npaired=true\nbattery=72\nusb=false\nclock=10:09\n"
TEXT = "Tap the middle of the screen now to see your message"
MORE = "Keep reading below"


def run(test: unittest.TestCase, board: str, scenario: str) -> tuple[list[str], str]:
    """The scenario on one board: its stdout lines (dumps, prints) and its log."""
    if not SIM.exists():
        test.skipTest("the simulator isn't built (simulator/README.md)")
    with tempfile.TemporaryDirectory(prefix="subs-unproven-") as tmp:
        sc = os.path.join(tmp, "s.txt")
        with open(sc, "w") as f:
            f.write(BASE + scenario)
        env = dict(os.environ, MUSE_SIM_BOARD=board, SDL_VIDEODRIVER="dummy")
        r = subprocess.run([str(SIM), "--headless", "--run-ms", "1", "--scenario", sc, "--screenshot",
                            os.path.join(tmp, "o.ppm")], env=env, capture_output=True, text=True, timeout=60)
    test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
    return r.stdout.splitlines(), r.stderr


def geometry(test: unittest.TestCase, board: str) -> tuple[int, int, tuple[int, int, int, int]]:
    """The panel's size and the two-line band's rectangle (x, y, w, h), from the simulator's log."""
    _, log = run(test, board, "advance=40\n")
    w, h = map(int, re.search(r"UI up: (\d+)x(\d+)", log).groups())
    bw, bh, bx, by = map(int, re.search(r"subtitles: band (\d+)x(\d+) at (\d+),(\d+)", log).groups())
    return w, h, (bx, by, bw, bh)


def dumps(lines: list[str]) -> list[list[str]]:
    out: list[list[str]] = []
    for line in lines:
        if line.startswith("@page"):
            out.append([])
        elif out and line.startswith("@label "):
            out[-1].append(line[len("@label "):].replace(" / ", " "))
    return out


def band_text(labels: list[str]) -> bool:
    return any("Tap the middle" in ln for ln in labels)


class CardOverSubtitles(unittest.TestCase):
    def test_a_confirm_card_stays_on_top_of_an_unproven_subtitle_stream(self) -> None:
        for board in BOARDS:
            with self.subTest(board=board):
                w, h, (bx, by, bw, bh) = geometry(self, board)
                band_tap = (bx + bw // 2, by + bh // 2)   # where the band lies, below the card
                lines, _ = run(self, board, "subs.unproven=true\n"
                                            f"subs={TEXT}\nadvance=600\ndump=every\n"
                                            "reconfirm=true\nlink=confirm\nadvance=300\ndump=every\n"
                                            f"subs.more={MORE}\nadvance=600\ndump=every\n"
                                            f"tap={band_tap[0]},{band_tap[1]}\nadvance=300\n"
                                            f"tap={w // 2},{h // 2}\nadvance=300\n"
                                            "link=online\nadvance=900\ndump=every\n")
                before, card, streaming, after = dumps(lines)
                self.assertTrue(band_text(before), before)
                # The card up: its Confirm on show, nothing of the band, the stream's new words included.
                for d in (card, streaming):
                    self.assertIn("Confirm", d)
                    self.assertFalse(band_text(d), d)
                    self.assertFalse(any("Keep reading" in ln for ln in d), d)
                # A tap where the band would be never reaches Confirm (a tap away from the card does nothing); the
                # visible Confirm itself confirms.
                taps = [ln for ln in lines if ln in ("@confirm", "@dismiss")]
                self.assertEqual(taps, ["@confirm"], lines)
                # The card gone: the unproven words from before it never come back (they went as the card came up, so
                # no lure written beside a prompt outlives it); words the stream sent while it was up show.
                self.assertFalse(band_text(after), after)
                self.assertTrue(any("Keep reading" in ln for ln in after), after)

    def test_the_owners_subtitles_come_back_after_the_card(self) -> None:
        for board in BOARDS:
            with self.subTest(board=board):
                w, h, _ = geometry(self, board)
                lines, _ = run(self, board, f"subs={TEXT}\nadvance=600\ndump=every\n"
                                            "reconfirm=true\nlink=confirm\nadvance=300\ndump=every\n"
                                            f"tap={w // 2},{h // 2}\nadvance=300\n"
                                            "link=online\nadvance=900\ndump=every\n")
                before, card, after = dumps(lines)
                self.assertTrue(band_text(before), before)
                self.assertIn("Confirm", card)
                self.assertFalse(band_text(card), card)
                self.assertTrue(band_text(after), after)


class UnprovenSubtitles(unittest.TestCase):
    def test_they_never_wake_keep_lit_or_hold_the_panel(self) -> None:
        for board in BOARDS:
            with self.subTest(board=board):
                lines, _ = run(self, board, "subs.unproven=true\nasleep=true\nadvance=100\n"
                                            f"subs={TEXT}\nadvance=300\n"
                                            "asleep=false\nadvance=300\n"
                                            f"subs={TEXT}\nadvance=600\ndump=sys\nsubs.holds=1\n"
                                            "subs.unproven=false\n"
                                            f"subs={TEXT}\nadvance=600\nsubs.holds=1\n")
                i = lines.index("@holds 0") if "@holds 0" in lines else -1
                self.assertGreaterEqual(i, 0, lines)
                unproven, proven = lines[:i + 1], lines[i + 1:]
                # Unproven: no wake (asleep or not), no keep-lit, no hold; shown on the screen already on.
                self.assertFalse([ln for ln in unproven if ln.startswith(("@wake", "@keep_lit"))], unproven)
                self.assertTrue(band_text(dumps(unproven)[-1]), unproven)
                # The owner's: state 1 wakes, words keep it lit, the band holds the panel.
                self.assertIn("@wake subtitles", proven)
                self.assertIn("@keep_lit subtitles", proven)
                self.assertIn("@holds 1", proven)


class InTheSource(unittest.TestCase):
    def body(self, src: str, head: str) -> str:
        i = src.index(head)
        return src[i:src.index("\n}\n", i)]

    def test_the_update_prompt_counts_the_band_and_every_layer_asks_the_card(self) -> None:
        ui = (MUSE / "muse_ui.c").read_text()
        chrome = self.body(ui, "static void update_chrome(float now)")
        shown = chrome.index("lv_obj_set_flag(s_pair, LV_OBJ_FLAG_HIDDEN, !b.passkey && !confirm && !let_go && !upd);")
        sync = chrome.index("sync_card();", shown)
        band = chrome.index("covered |= muse_subs_band_drawn();", sync)
        self.assertLess(band, chrome.index("atomic_store(&s_ask_shown, ask && !covered", band))
        card = self.body(ui, "static void sync_card(void)")
        self.assertIn("muse_passcode_ui_covering()", card)
        self.assertIn("muse_subs_card(card);", card)
        self.assertIn("muse_call_card(card);", card)
        subs = (MUSE / "muse_subs.c").read_text()
        self.assertIn("&& !s_card;", self.body(subs, "void muse_subs_tick(void)"))
        for head in ("void muse_subs_overlay(uint8_t *dst", "bool muse_subs_overlay_rect("):
            self.assertIn("muse_passcode_locked() || ", self.body(subs, head))
        self.assertIn("lv_obj_add_flag(s_obj, LV_OBJ_FLAG_HIDDEN);", self.body(subs, "void muse_subs_card(bool up)"))
        call = (MUSE / "muse_call.c").read_text()
        self.assertIn("!s_card &&", self.body(call, "static bool controls_shown(void)"))
        self.assertIn("face_touch_ok() && !s_card", self.body(call, "static void on_touch(lv_event_t *e)"))

    def test_an_unproven_call_remaps_no_key(self) -> None:
        inp = (MUSE / "muse_input.c").read_text()
        buttons = self.body(inp, "static void dedicated_buttons(unsigned ev)")
        # Power and speaker keep their own jobs (sleep, speaker on/off) unless the call is the owner's.
        self.assertIn("else if (muse_call_owner_active()) {\n            /* The owner's call only", buttons)
        self.assertIn("} else if (muse_call_owner_active()) {\n            muse_call_volume_key(1);", buttons)
        self.assertNotIn("} else if (muse_call_active()) {", buttons)
        # A press leaves the video view as outside a call, unless the call is the owner's.
        self.assertIn("if (muse_call_owner_active()) {\n            presses = 0;", inp)

    def test_call_query_is_not_activity(self) -> None:
        inp = (MUSE / "muse_input.c").read_text()
        start = inp.index('    if (!strcmp(line, "call") || !strcmp(line, "call=0") || !strcmp(line, "call?")) {')
        block = inp[start:inp.index("        return true;\n    }\n", start) + len("        return true;\n    }\n")]
        harness = r"""
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int muse_call_phase_t;
static int pokes, starts, ends;
static void muse_state_poke(void) { pokes++; }
static void muse_call_start_from(const char *why) { (void)why; starts++; }
static void muse_call_end_from(const char *why) { (void)why; ends++; }
static muse_call_phase_t muse_call_phase(int *s) { *s = 0; return 0; }
static const char *muse_call_phase_name(muse_call_phase_t p) { (void)p; return "none"; }
static int muse_call_volume(void) { return 50; }
static bool word(const char *line) {
""" + block + r"""
    return false;
}
int main(void) {
    word("call?");
    fprintf(stderr, "read %d %d %d\n", pokes, starts, ends);
    word("call");
    fprintf(stderr, "start %d %d %d\n", pokes, starts, ends);
    word("call=0");
    fprintf(stderr, "end %d %d %d\n", pokes, starts, ends);
    return 0;
}
"""
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="call-read-") as tmp:
            src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
            src.write_text(harness)
            r = subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("read 0 0 0", r.stderr)    # an open read: no activity
        self.assertIn("start 1 1 0", r.stderr)
        self.assertIn("end 2 1 1", r.stderr)


if __name__ == "__main__":
    unittest.main()
