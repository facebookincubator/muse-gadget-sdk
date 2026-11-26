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

"""The vendored haptics core (components/muse_gadget_ble/haptics, the
core unchanged) against the core's vectors, with the core's own
test run on this copy (ASan/UBSan, every scenario byte for byte)."""

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "components" / "muse_gadget_ble" / "haptics"
MGC = ROOT.parent / "protocols" / "mgcommands.h"


class VendoredCore(unittest.TestCase):
    def test_vectors(self):
        r = subprocess.run([sys.executable, str(CORE / "tests" / "test_mg_haptics.py"), str(CORE), str(MGC),
                            str(CORE / "tests" / "mg-haptic-v1.json")], capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        self.assertIn("OK", r.stderr)



BLE = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()

DUTY_ORDER = r"""
#include <stdio.h>
#include <string.h>
#include "mg_haptics.h"
static const mgh_caps_t caps = { .kind = 1, .min_on_ms = 20, .strength_steps = 1, .max_on_ms = 1000 };
static uint32_t run(int settle_first)
{
    static mgh_t h;
    mgh_init(&h, &caps);
    mgh_env_t e = { .enabled = 1 };
    mgh_status_t s[2];
    uint32_t next;
    /* 2 s on in the first 5 s bucket. */
    mgh_req_t a = { .id = 1, .repeat = 1, .count = 1, .steps = { 1000 } };
    mgh_play(&h, &a, MGH_SRC_CLIENT, &e, 1000, s);
    mgh_tick(&h, 2000, s, &next);
    mgh_play(&h, &a, MGH_SRC_CLIENT, &e, 2000, s);
    mgh_tick(&h, 3000, s, &next);
    /* 900 ms on from 9,000 (then 1 s off): the on step ends at 9,900, in the second bucket; nothing ticks it
     * until 10,100, in the third. */
    mgh_req_t b = { .id = 2, .repeat = 1, .count = 2, .steps = { 900, 1000 } };
    mgh_play(&h, &b, MGH_SRC_CLIENT, &e, 9000, s);
    mgh_tick(&h, 9000, s, &next);
    if (settle_first) mgh_tick(&h, 10100, s, &next);   /* the board settles first (haptic_settle) */
    /* A request the duty limit refuses, at 10,100: its check rolls the buckets to the third. */
    mgh_req_t c = { .id = 3, .repeat = 1, .count = 1, .steps = { 4800 } };
    mgh_play(&h, &c, MGH_SRC_CLIENT, &e, 10100, s);
    mgh_tick(&h, 10100, s, &next);   /* the late tick adds the 900 ms in the second bucket */
    return mgh_duty_used(&h, 10100, 60000u);
}
int main(void)
{
    printf("duty: a refusal at the boundary, unsettled %u ms, settled first %u ms\n", run(0), run(1));
    return 0;
}
"""


class DutyOrder(unittest.TestCase):
    """A step's end added after a newer roll keeps the duty history (the haptics core; it cleared
    it before). The board also settles (mgh_tick) at `now` before anything that rolls at `now`."""

    def test_settled_before_play_keeps_the_history(self):
        import os, shlex, tempfile
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "duty.c"
            src.write_text(DUTY_ORDER)
            exe = Path(tmp) / "duty"
            b = subprocess.run(cc + ["-std=gnu11", "-Wall", "-Werror", "-fsanitize=address,undefined", "-I", str(CORE),
                                     f'-DMGH_MGCOMMANDS_H="{MGC}"', str(src), str(CORE / "mg_haptics.c"), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(b.returncode, 0, b.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60).stdout
        # The core keeps the history either way; the backend settles first all the same.
        self.assertIn("duty: a refusal at the boundary, unsettled 2900 ms, settled first 2900 ms", out)

    def test_every_entry_settles_first(self):
        settle = body(BLE, "static void haptic_settle(uint32_t now)")
        self.assertIn("mgh_tick(&s_mgh, now, s, &next)", settle)
        for name, call in (("static uint16_t op_haptic(void *ctx, const uint8_t *a, size_t n)", "mgh_parse_stop("),
                           ("bool mg_haptic_cue(uint8_t src, uint16_t ms)", "mgh_play("),
                           ("void mg_haptic_interrupt(uint8_t reason)", "mgh_interrupt("),
                           ("mght_result_t mg_ble_haptic_test(const mght_cmd_t *c)", "mght_run("),
                           ("static void haptic_tick_locked(void)", "mgh_interrupt(")):
            fn = body(BLE, name)
            self.assertLess(fn.index("haptic_settle(now);"), fn.index(call), name)



def body(src, name):
    i = src.index(name)
    return src[i:src.index("\n}\n", i)]


class Backend(unittest.TestCase):
    """The ESP32 backend: the gates the core asks for, the timer, the motor,
    and the moments that cut a client's pattern (HAPTICS.md)."""

    def test_gates(self):
        env = body(BLE, "static void haptic_env(mgh_env_t *e, bool own)")
        self.assertIn("mg_proto_haptics(&s_proto)", env)                  # the connection's haptics_enabled
        self.assertIn("ui == MG_SMP_UI_WRITING", env)                    # an update runs
        self.assertIn("b.soc_percent <= 10", env)                        # low battery, not charging
        self.assertIn("!(b.flags & mg_battery_status_flag_charging)", env)
        self.assertIn("e->in_turn = s_proto.turn;", env)

    def test_drive_and_timer(self):
        drive = body(BLE, "static void haptic_drive(uint32_t next_ms)")
        self.assertIn("bool on = mgh_motor(&s_mgh);", drive)
        self.assertIn("esp_timer_start_once(s_mgh_timer", drive)
        # A new on step while the motor runs (an on-to-on replacement) renews the board's own stop.
        self.assertIn("bool renew = on && s_motor && (s_mgh.played != s_played || s_mgh.step != s_step);", drive)
        self.assertIn("if (on != s_motor || renew) {", drive)
        tick = body(BLE, "static void haptic_tick_locked(void)")
        self.assertLess(tick.index("mgh_interrupt("), tick.index("mgh_tick("))   # a gate that closed cuts first

    def test_what_cuts_a_pattern(self):
        disc = BLE[BLE.index("case BLE_GAP_EVENT_DISCONNECT:"):]
        self.assertLess(disc.index("mgh_link_drop(&s_mgh"), disc.index("mg_proto_disconnect(&s_proto);"))
        smp = (ROOT / "components" / "muse_gadget_ble" / "mg_smp_esp.c").read_text()
        self.assertIn("mg_haptic_interrupt(2 /* mg_haptic_reason_updating */);", body(smp, "static int be_begin("))
        hdr = (ROOT.parent / "protocols" / "mgcommands.h").read_text()
        self.assertIn("mg_haptic_reason_updating = 2,", hdr)   # the literal above stays the header's value
        voice = (ROOT / "components" / "muse_gadget_ble" / "mg_voice.c").read_text()
        self.assertIn("mg_haptic_interrupt(mg_haptic_reason_in_use);", body(voice, "bool mg_voice_gesture(bool down)"))

    def test_own_cues_go_through_the_core(self):
        muse_voice = (ROOT / "components" / "muse" / "muse_voice.c").read_text()
        buzz = body(muse_voice, "static void threshold_buzz(void)")
        self.assertLess(buzz.index("mg_ble_haptic_talk_cue(BUZZ_MS)"), buzz.index("muse_board->buzz(BUZZ_MS"))
        self.assertIn("return mg_haptic_cue(MGH_SRC_TALK, ms);", body(BLE, "bool mg_vibrate("))

    def test_stopwatch_actuator(self):
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        self.assertIn("{ mg_haptic_actuator_erm, mg_haptic_actuator_flag_timing, 30, 0 }", glue)
        board = (ROOT / "components" / "muse" / "boards" / "board_m5stack_stopwatch.c").read_text()
        self.assertIn("ioe_set(IOE_MOTOR, false);", body(board, "static void buzz(int ms, int pct)"))


if __name__ == "__main__":
    unittest.main()
