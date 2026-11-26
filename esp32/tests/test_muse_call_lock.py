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

"""A live call and the passcode lock: an
unpaired central's `!IN call` on the Nordic UART reaches mg_glue's bench input
allowlist, muse_input's `call` word, muse_call_start_from() and start_call(),
which on a locked gadget asked the phone (ASKED) with no keypad, after which the
phone's connecting/live skipped the keypad (it held only a call from NONE) and
the call's video was drawn over the lock. The decisions are
components/muse/muse_call_lock.h's, driven here the way muse_call.c drives them.
"""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "muse_call_lock.h"

enum { NONE, ASKED, CONNECTING, LIVE, ENDED };
static const char *NAMES[] = { "none", "asked", "connecting", "live" };
static int phase;
static bool locked, ui, held, asleep, unproven_call;

/* muse_call.c's start_call() and muse_call_client_state(), their decisions only. */
static void start(void)
{
    switch (muse_call_may_start(phase == NONE, locked, ui)) {
    case MUSE_CALL_START_GO: phase = ASKED; printf("asked\n"); break;
    case MUSE_CALL_START_LOCKED: printf("keypad Unlock to call\n"); break;
    case MUSE_CALL_START_UI: printf("refused ui\n"); break;
    default: break;
    }
}

static void client(int st, bool unproven)
{
    bool starts = st == CONNECTING || st == LIVE;
    if (unproven) {   /* a central without the token proof */
        bool owner_call = (phase != NONE && !unproven_call) || held;
        if (!muse_call_unproven_shows(owner_call, locked, asleep)) {
            printf("not shown\n");
            return;
        }
        if (starts && phase == NONE) unproven_call = true;
    } else if (starts) {
        unproven_call = false;
    }
    if (muse_call_keypad_holds(phase == NONE || phase == ASKED, starts, locked)) {
        held = true;
        asleep = false;   /* the screen wakes to the keypad */
        printf("held behind the keypad\n");
        return;
    }
    if (starts) {
        phase = st;
        printf("%s\n", NAMES[phase]);
    } else if (st == ENDED && phase != NONE) {
        phase = NONE;
        unproven_call = false;
        printf("ended\n");
    }
}

/* muse_call_poll's close: an unproven call view goes once locked or asleep. */
static void poll(void)
{
    if (unproven_call && phase != NONE && muse_call_unproven_closes(locked, asleep)) {
        phase = NONE;
        unproven_call = false;
        printf("closed\n");
    }
}

int main(void)
{
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "lock", 4)) locked = true;
        else if (!strncmp(line, "unlock", 6)) locked = false;
        else if (!strncmp(line, "ui", 2)) ui = true;
        else if (!strncmp(line, "start", 5)) start();
        else if (!strncmp(line, "connecting", 10)) client(CONNECTING, false);
        else if (!strncmp(line, "live", 4)) client(LIVE, false);
        else if (!strncmp(line, "u-connecting", 12)) client(CONNECTING, true);
        else if (!strncmp(line, "u-live", 6)) client(LIVE, true);
        else if (!strncmp(line, "u-ended", 7)) client(ENDED, true);
        else if (!strncmp(line, "sleep", 5)) asleep = true;
        else if (!strncmp(line, "poll", 4)) poll();
        else if (!strncmp(line, "screen", 6))
            printf("%s %s phase %s\n", locked ? "locked" : "unlocked", asleep ? "dark" : "on", NAMES[phase]);
        else if (!strncmp(line, "phase", 5)) printf("phase %s\n", NAMES[phase]);
    }
    return 0;
}
"""


WORD = r"""
#include <stdio.h>
#include "muse_call_lock.h"

enum { NONE, ASKED, CONNECTING, LIVE };

/* Every word a reader can load while muse_call.c's set_phase ends a call (one compare-and-swap: before and after),
 * and while client_state marks an unproven call and set_phase opens it. */
int main(void)
{
    int bad = 0;
    for (unsigned p = ASKED; p <= LIVE; p++) {
        for (int unproven = 0; unproven <= 1; unproven++) {
            uint32_t on = muse_call_word_with_phase(muse_call_word_with_unproven(0, unproven), p);
            uint32_t off = muse_call_word_with_phase(on, NONE);
            /* On: active, owner xor unproven as marked. Ended: none of them, the mark gone with the phase. */
            bad += muse_call_word_phase(on) != p || muse_call_word_owner(on) == (bool)unproven ||
                   muse_call_word_unproven(on) != (bool)unproven;
            bad += off != 0 || muse_call_word_owner(off) || muse_call_word_unproven(off) || muse_call_word_phase(off);
            /* An unproven call ending is never seen as the owner's, before the store or after. */
            if (unproven) {
                bad += muse_call_word_owner(on) || muse_call_word_owner(off);
            }
        }
    }
    /* The mark set with no call on counts for nothing until the phase leaves none, and stays with it. */
    uint32_t marked = muse_call_word_with_unproven(0, true);
    bad += muse_call_word_unproven(marked) || muse_call_word_owner(marked) || muse_call_word_phase(marked);
    uint32_t live = muse_call_word_with_phase(marked, LIVE);
    bad += !muse_call_word_unproven(live) || muse_call_word_owner(live);
    /* Proven now (the owner's answer): the owner's, the phase kept. */
    uint32_t proven = muse_call_word_with_unproven(live, false);
    bad += !muse_call_word_owner(proven) || muse_call_word_phase(proven) != LIVE;
    printf(bad ? "FAIL %d\n" : "call word ok\n", bad);
    return bad != 0;
}
"""


def function(src: str, head: str) -> str:
    body = src[src.index(head):]
    return body[:body.index("\n}\n")]


class CallLock(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "lock"
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(d / "driver.c"), "-o",
                             str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_lines(self, lines: list[str]) -> list[str]:
        return subprocess.run([str(self.exe)], input="\n".join(lines) + "\n", text=True, capture_output=True,
                              check=True).stdout.splitlines()

    def test_an_injected_start_while_locked_asks_for_the_keypad_not_the_phone(self) -> None:
        # `!IN call` on a locked gadget, then the phone's connecting and live.
        r = self.run_lines(["lock", "start", "phase", "connecting", "live", "phase"])
        self.assertEqual(r, ["keypad Unlock to call", "phase none", "held behind the keypad",
                             "held behind the keypad", "phase none"])

    def test_a_call_asked_then_locked_is_held_too(self) -> None:
        r = self.run_lines(["start", "lock", "connecting", "live", "phase"])
        self.assertEqual(r, ["asked", "held behind the keypad", "held behind the keypad", "phase asked"])

    def test_unlocked_unchanged(self) -> None:
        self.assertEqual(self.run_lines(["start", "connecting", "live", "phase"]),
                         ["asked", "connecting", "live", "phase live"])
        self.assertEqual(self.run_lines(["connecting", "live"]), ["connecting", "live"])   # the phone's own call
        self.assertEqual(self.run_lines(["ui", "start", "phase"]), ["refused ui", "phase none"])
        # A call already on isn't held by a lock (the passcode doesn't lock mid-call: passcode_busy()).
        self.assertEqual(self.run_lines(["start", "connecting", "lock", "live", "phase"]),
                         ["asked", "connecting", "live", "phase live"])

    def test_a_locked_gadget_stays_locked_and_dark_while_an_unproven_central_reports_a_call(self) -> None:
        # No keypad, no wake, no call screen; the lock stays as it was.
        self.assertEqual(self.run_lines(["lock", "sleep", "u-connecting", "u-live", "screen"]),
                         ["not shown", "not shown", "locked dark phase none"])
        # Unlocked but dark: the screen isn't woken for it either.
        self.assertEqual(self.run_lines(["sleep", "u-live", "screen"]), ["not shown", "unlocked dark phase none"])

    def test_an_unproven_call_shows_only_unlocked_and_awake_and_goes_at_the_lock(self) -> None:
        self.assertEqual(self.run_lines(["u-connecting", "u-live", "lock", "poll", "screen"]),
                         ["connecting", "live", "closed", "locked on phase none"])
        self.assertEqual(self.run_lines(["u-live", "sleep", "poll"]), ["live", "closed"])

    def test_an_unproven_central_never_cancels_or_replaces_the_owners_call(self) -> None:
        self.assertEqual(self.run_lines(["start", "connecting", "live", "u-ended", "u-connecting", "phase"]),
                         ["asked", "connecting", "live", "not shown", "not shown", "phase live"])
        self.assertEqual(self.run_lines(["start", "u-live", "phase"]), ["asked", "not shown", "phase asked"])

    def test_a_call_ending_is_never_seen_as_the_owners(self) -> None:
        # The call's phase and its unproven mark are one word: the end of an unproven call clears both in one store,
        # so passcode_busy (any task) never sees "on, and the owner's" on its way out and skips the screen-off lock.
        d = Path(self.tmp.name)
        (d / "word.c").write_text(WORD)
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(d / "word.c"), "-o",
                             str(d / "word")], check=True)
        r = subprocess.run([str(d / "word")], capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout), (0, "call word ok\n"))
        call = (MUSE / "muse_call.c").read_text()
        self.assertNotRegex(call, r"\bs_(?:phase|unproven)\b")   # no second copy of either
        self.assertIn("static atomic_uint s_call;", call)
        # Written only by compare-and-swap, in the two store helpers; every other use is one load.
        self.assertEqual(call.count("atomic_compare_exchange_weak(&s_call, "), 2)
        self.assertEqual(call.count("&s_call"), 2 + call.count("atomic_load(&s_call)"))
        sp = function(call, "static void set_phase(muse_call_phase_t phase)\n{")
        self.assertEqual(sp.count("call_store_"), 1)
        self.assertIn("call_store_phase(phase);", sp)
        self.assertIn("return muse_call_word_owner(atomic_load(&s_call));", function(call, "bool muse_call_owner_active(void)\n{"))
        # The gadget's own start is the owner's call, whatever mark a refused unproven state left with no call on.
        start = function(call, "static void start_call(const char *why, const char *by, const char *detail)\n{")
        self.assertLess(start.index("call_store_unproven(false);"), start.index("set_phase(MUSE_CALL_ASKED);"))
        # The readers that ask "the owner's call?" ask it in one call.
        ui = (MUSE / "muse_ui.c").read_text()
        self.assertIn("call = call || muse_call_owner_active();", function(ui, "static bool passcode_busy(void)\n{"))
        for src in (ui, (MUSE / "muse_input.c").read_text()):
            self.assertNotIn("muse_call_active() && !muse_call_unproven()", src)

    def test_no_call_row_where_a_call_cant_work(self) -> None:
        # The button menu's call row uses Settings' and the home's predicate (built hidden, skipped by navigation
        # and by the first selection); start_call refuses on such a board from any entry point.
        menu = (MUSE / "muse_menu.c").read_text()
        shown = function(menu, "static bool item_shown(int i)\n{")
        self.assertIn("if (i == ITEM_CALL) {\n        return muse_call_supported();", shown)
        self.assertIn("if (!item_shown(i)) {\n            lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);", menu)
        self.assertIn("} while (!item_shown(s_sel));", menu)
        self.assertIn("while (!item_shown(s_sel)) {\n        s_sel++;", function(menu, "static void open_menu(void)\n{"))
        self.assertIn("int v = item_vis(s_sel);", function(menu, "static void refresh(void)\n{"))

    def test_a_held_call_that_never_opened_leaves_nothing(self) -> None:
        # A call that waited behind the keypad and ended (or failed, the link went, or the
        # 60 s decline) takes its retained subtitles and any video it started with it; its phase never left
        # NONE, so set_phase's teardown never ran.
        call = (MUSE / "muse_call.c").read_text()
        td = function(call, "static void held_teardown(void)\n{")
        self.assertIn("s_held_state = 0;", td)
        self.assertIn("muse_subs_call_end();", td)
        self.assertIn("s_close_gen = muse_video_gen() + 1;", td)
        held = function(call, "static void held_poll(void)\n{")
        self.assertIn("held_teardown();\n        ESP_LOGW(TAG, \"a call from the phone, still locked after 60 s: declined\");", held)
        state = function(call, "static void client_state(uint8_t state, bool unproven)\n{")
        self.assertIn("if (s_held_state && !starts) {\n        held_teardown();", state)
        # Its reply audio waits too: the protocol hears the hold, and its end at the unlock or teardown.
        self.assertIn("mg_ble_live_call_held(true);", state)
        self.assertIn("mg_ble_live_call_held(false);", td)
        self.assertIn("mg_ble_live_call_held(false);", held)

    def test_a_talk_hold_in_a_call_is_the_calls_speech(self) -> None:
        # Any capture in a call or its 3 s tail, the user's own talk hold included, is the call's speech: no
        # listening face, no own turn (nothing left latched once the hold and the call end).
        voice = (MUSE / "muse_voice.c").read_text()
        t = function(voice, "static void mg_turn(mg_route_t route, bool from_client, bool barge_in, bool wake)\n{")
        self.assertIn("bool in_call = call_on();", t)
        self.assertIn("muse_state_set_own_turn(!from_client && !in_call);", t)
        self.assertLess(t.index("if (!in_call) {"), t.index("set_mode(MUSE_MODE_LISTENING);"))

    def test_a_late_video_close_never_ends_a_newer_stream(self) -> None:
        # The call's video closes with the call from the input task, by stream: finish names the stream that was
        # on, a new call voids the close, and the close checks the stream right before it.
        call = (MUSE / "muse_call.c").read_text()
        self.assertIn("s_close_gen = muse_video_gen() + 1;",
                      function(call, "static void finish(const char *banner, int ms, const char *why)\n{"))
        self.assertIn("s_close_gen = 0;         /* a new call", function(call, "static void set_phase(muse_call_phase_t phase)\n{"))
        self.assertIn("muse_video_close_gen(close_gen - 1);", function(call, "void muse_call_poll(void)\n{"))
        self.assertNotIn("muse_video_close", function(call, "static void client_state(uint8_t state, bool unproven)\n{"))
        video = (MUSE / "muse_video.c").read_text()
        cg = function(video, "void muse_video_close_gen(uint32_t gen)\n{")
        self.assertLess(cg.index("if (!s_open || gen != s_gen) {"), cg.index("muse_video_close();"))

    def test_no_touch_coordinates_on_the_mirrored_call_tags(self) -> None:
        # mg.* is on the Nordic UART log mirror, which any central reads with no pairing (release too): the call
        # screen's tap and touch-down coordinates go only to the cable console's tag, and the call timeline's
        # by= / end= details (mg.call, mg.lct) carry none.
        call = (MUSE / "muse_call.c").read_text()
        self.assertNotRegex(call, r'ESP_LOG[EWIDV]\(TAG, "(?:tap|touch-down)[^"]*%d, ?%d')
        self.assertEqual(len(re.findall(r'ESP_LOGI\(XY_TAG, "(?:tap|touch-down) at %d,%d', call)), 5)
        self.assertIn('static const char *XY_TAG = "muse_touch";', call)
        self.assertIn('ESP_LOGI(TAG, "touch-down: controls shown (not a talk surface)");', call)
        log = (MUSE / "muse_touch_log.h").read_text()
        self.assertNotIn("%d,%d", log)
        self.assertIn('"touch(%u)"', function(log, "static inline int muse_touch_tag("))

    def test_the_path_reaches_the_helper(self) -> None:
        glue = (ROOT / "main/mg_glue.c").read_text()
        allow = glue[glue.index("static const char *const exact[] = {"):]
        self.assertIn('"call", ', allow[:allow.index("};")])   # the word stays, injected input reaches it
        inp = (MUSE / "muse_input.c").read_text()
        self.assertIn('} else if (!line[4]) {\n            muse_state_poke();\n            muse_call_start_from("console");', inp)
        call = (MUSE / "muse_call.c").read_text()
        self.assertIn("start_call(why, ", function(call, "void muse_call_start_from(const char *why)\n{"))
        start = function(call, "static void start_call(const char *why, const char *by, const char *detail)\n{")
        self.assertTrue(start.startswith(
            "static void start_call(const char *why, const char *by, const char *detail)\n{\n"
            "    if (!muse_call_supported()) {"))   # no call on this board: nothing, from any entry point
        self.assertIn("    muse_call_start_t may = muse_call_may_start(call_phase() == MUSE_CALL_NONE, muse_passcode_locked(), "
                      "ui_stream_on());", start)
        self.assertLess(start.index("muse_passcode_request_keypad(MUSE_PASS_WHY_CALL);"),
                        start.index("set_phase(MUSE_CALL_ASKED);"))
        state = function(call, "static void client_state(uint8_t state, bool unproven)\n{")
        self.assertIn("    muse_call_phase_t phase = call_phase();\n"
                      "    if (muse_call_keypad_holds(phase == MUSE_CALL_NONE || phase == MUSE_CALL_ASKED, starts, "
                      "muse_passcode_locked())) {", state)
        # The call's video isn't exempt from the lock.
        video = (MUSE / "muse_video.c").read_text()
        self.assertIn("static bool held(void)\n{\n    return s_hold || s_cover;\n}", video)


if __name__ == "__main__":
    unittest.main()
