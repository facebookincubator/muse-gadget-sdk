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

"""Link modes (mgcommands.h "Link modes").

mg_linkpol.c through a small driver with a fake clock, for what would fail
silently on a board: the active set from a policy that was never initialized,
re-asks that stop (collision refusals, a request the stack holds unanswered),
no listening-window override left after a disconnect, and the sleep set's
bench word refusing what the spec refuses. Settings 16 and 17 themselves run
in test_mg_ble's protocol harness.
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
MG = ROOT / "components" / "muse_gadget_ble"

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "mg_linkpol.h"

/* One command a line, times in ms (a fake clock):
 *   connect T ITVL LAT TO | update ITVL LAT TO [T [STATUS]] | act T
 *   step T [BUSY [REPLY]] | busy T | disc | note | wake | wakesync | sw T WORD
 * update's T defaults to the last time seen, STATUS to 0 (granted; else
 * refused). step (BUSY 1: something ongoing) prints "chg_FROM_TO " when the
 * effective mode changed since the last report, then "req_MIN-MAX/LAT/TO" or
 * "-"; busy = the stack still held our earlier, unanswered request (NimBLE's
 * EALREADY); disc = the link dropped; note prints the policy's note
 * ("note_-": none) and clears it. The listening window: wake prints "wake_1"
 * while open, else "wake_0_WHY_MS" (the last window's close and length; WHY's
 * spaces as _); wakesync prints what the caller does with its override (keep,
 * apply, reapply, release). sw runs a console word (mg_linkpol_sleep_word)
 * and prints its answer ("notword": not one). */
static mg_linkpol_t p;
static mg_linkpol_wake_t ws;

static void change(void)
{
    uint8_t from;
    mg_linkpol_mode_t to;
    const char *why;
    if (mg_linkpol_take_change(&p, &from, &to, &why)) {
        printf("chg_%s_%s ", mg_linkpol_mode_name(from), mg_linkpol_mode_name(to));
    }
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc < 2) {   /* any argument: no init, as mg_ble's static policy starts */
        mg_linkpol_init(&p);
    }
    char line[128];
    long long last = 0;
    while (fgets(line, sizeof(line), stdin)) {
        int a, b, c, f = 0;
        long long t;
        int n;
        char word[128], ans[320];
        if (sscanf(line, "sw %lld %127s", &t, word) == 2) {
            last = t;
            bool ok = mg_linkpol_sleep_word(&p, word, t * 1000, ans, sizeof(ans));
            printf("%s\n", ok ? ans : "notword");
        } else if (sscanf(line, "connect %lld %d %d %d", &t, &a, &b, &c) == 4) {
            last = t;
            mg_linkpol_connect(&p, t * 1000, (uint16_t)a, (uint16_t)b, (uint16_t)c);
        } else if ((n = sscanf(line, "update %d %d %d %lld %d", &a, &b, &c, &t, &f)) >= 3) {
            if (n >= 4) last = t;
            mg_linkpol_conn_update(&p, last * 1000, n >= 5 ? f : 0, (uint16_t)a, (uint16_t)b, (uint16_t)c);
        } else if (sscanf(line, "act %lld", &t) == 1) {
            last = t;
            mg_linkpol_action(&p, t * 1000, "talk press");
        } else if (!strncmp(line, "wakesync", 8)) {
            static const char *const acts[] = { "keep", "apply", "reapply", "release" };
            printf("wakesync_%s\n", acts[mg_linkpol_wake_sync(&p, &ws)]);
        } else if (!strncmp(line, "wake", 4)) {
            if (p.on && p.wake) {
                printf("wake_1\n");
            } else {
                char why[48];
                snprintf(why, sizeof(why), "%s", p.wake_why ? p.wake_why : "-");
                for (char *s = why; *s; s++) {
                    if (*s == ' ') *s = '_';
                }
                printf("wake_0_%s_%lld\n", why, (long long)(p.wake_held_us / 1000));
            }
        } else if (sscanf(line, "busy %lld", &t) == 1) {
            last = t;
            mg_linkpol_request_busy(&p, t * 1000);
        } else if (!strncmp(line, "disc", 4)) {
            mg_linkpol_disconnect(&p);
        } else if (!strncmp(line, "note", 4)) {
            printf("note_%s\n", p.note ? p.note : "-");
            p.note = NULL;
        } else if ((n = sscanf(line, "step %lld %d %d", &t, &a, &b)) >= 1) {
            last = t;
            mg_linkpol_in_t in = { .busy = n >= 2 && a ? "video stream" : NULL, .reply = n >= 3 && b };
            mg_linkpol_params_t w;
            bool go = mg_linkpol_step(&p, t * 1000, &in, &w);
            change();
            if (go) {
                printf("req_%u-%u/%u/%u\n", w.itvl_min, w.itvl_max, w.latency, w.timeout);
            } else {
                printf("-\n");
            }
        }
    }
    return 0;
}
"""

MIN = 60 * 1000
ACTIVE = "req_12-12/0/400"
READY = "req_24-36/5/400"
CONNECTED = "chg_-_ready "    # the first report of a connection: ready
# Connected at 0 on the central's 30 ms / 0 / 720 ms, its own update to 15 ms
# at 100 (the S22's), ready asked at 600 and granted at 700.
READY_AT_700 = ["connect 0 24 0 72", "update 12 0 400 100", "step 600", "update 24 5 400 700"]


class LinkModesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "linkpol"
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MG), str(d / "driver.c"),
                             str(MG / "mg_linkpol.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def steps(self, lines: list[str], args=()) -> list[str]:
        out = subprocess.run([str(self.exe), *args], input="\n".join(lines) + "\n", text=True, capture_output=True,
                             check=True).stdout
        return [l.strip() for l in out.splitlines()]


    def test_fast_interval_without_init(self) -> None:
        # mg_ble's policy is static zero until its first connection: the
        # active set must still be 12/12, never "0-0" (refused by the stack).
        r = self.steps(["connect 0 24 0 72", "step 1000", "act 2000", "step 3000"], args=["noinit"])
        self.assertEqual(r, [CONNECTED + READY, "chg_ready_active " + ACTIVE])


    def test_collision_retries_are_bounded_then_as_any_refusal(self) -> None:
        r = self.steps(READY_AT_700 + ["act 5000", "step 5000", "update 24 5 400 5100 554", "step 5400",
                                       "update 24 5 400 5500 554", "step 5800", "update 24 5 400 5900 554", "note",
                                       "step 6200 1", "step 10899 1", "step 10900 1"])
        self.assertEqual(r[-6:], [ACTIVE, ACTIVE, "note_request refused: asking once more in 5 s", "-", "-", ACTIVE])


    # No answer at all (seen on the Watcher): a talk press from sleep
    # after a quiet night asked active and neither a grant nor a refusal ever
    # came; only a refusal re-asked, so the turn rode the 200 ms link. PRESS:
    # asleep (ASLEEP below), the talk press at P, its request at once.
    def _press_from_sleep(self, p: int) -> list[str]:
        return self.ASLEEP + [f"act {p}", f"step {p} 1"]


    def test_unanswered_reasks_are_bounded_at_45s(self) -> None:
        # Every 2 s while the stack holds it (EALREADY), for 45 s from the
        # mode's first unanswered request; then a refusal: once more after
        # 5 s, then left until the next mode change.
        p = 8 * 60 * MIN
        lines = self._press_from_sleep(p)
        for t in range(p + 2000, p + 48000, 2000):
            lines += [f"step {t} 1", f"busy {t}"]
        lines += ["note"]
        lines += [f"step {t} 1" for t in range(p + 46500, p + 70000, 500)]
        r = self.steps(lines)
        n = len(self.steps(self._press_from_sleep(p)))   # its outputs
        self.assertEqual(r[n:n + 23], [ACTIVE] * 23)   # 2 s to 46 s
        self.assertEqual(r[n + 23], "note_no answer for 45 s and the stack still holds the request: treated as refused")
        tail = r[n + 24:]
        asks = [p + 46500 + 500 * i for i, x in enumerate(tail) if x != "-"]
        self.assertEqual(asks, [p + 51000])   # once more after 5 s, then left
        self.assertEqual(tail[(51000 - 46500) // 500], ACTIVE)


    # ASLEEP: ready granted at 700, 10 min in ready, sleep asked at 10 min
    # and granted 100 ms later.
    ASLEEP = READY_AT_700 + [f"step {10 * MIN}", f"update 160 4 400 {10 * MIN + 100}"]


    def test_no_override_remains_after_a_disconnect(self) -> None:
        r = self.steps(READY_AT_700 + ["act 5000", "step 5000", "wakesync", "disc", "wake", "wakesync",
                                       "wakesync", "update 12 0 400 6000", "wake", "wakesync",
                                       # a new connection starts with the window closed
                                       "connect 9000 24 4 400", "wake", "wakesync"])
        self.assertEqual(r, [CONNECTED + READY, "chg_ready_active " + ACTIVE, "wakesync_apply",
                             "wake_0_disconnect_0", "wakesync_release", "wakesync_keep", "wake_0_disconnect_0",
                             "wakesync_keep", "wake_0_-_0", "wakesync_keep"])


    # link.sleep (the sleep parameters' A/B).
    SLEEP_TUNED = "link.sleep 160-320/2/600 (link.sleep; 200.00-400.00 ms, latency 2, 6000 ms timeout)"
    SLEEP_USAGE = ("link.sleep: link.sleep=ITVL_MIN,ITVL_MAX,LAT (1.25 ms units, 6-3200, min <= max; latency 0-499; "
                   "6000 ms timeout > (1 + LAT) x ITVL_MAX x 2), link.sleep=0 (defaults) or link.sleep?")


    def test_sleep_knob_refuses_what_the_spec_refuses(self) -> None:
        # Interval 6-3200 with min <= max, latency 0-499, and the 6 s sleep
        # timeout > (1 + latency) x max x 2 (4 x 600 > (1 + LAT) x MAX); malformed
        # words get the usage line, and the set in effect stays.
        bad = ["link.sleep=5,10,0", "link.sleep=100,50,0", "link.sleep=100,200,500", "link.sleep=100,3201,0",
               "link.sleep=1200,1200,1", "link.sleep=600,600,3", "link.sleep=100,200", "link.sleep=a,b,c",
               "link.sleep=100,200,4,5", "link.sleep=100,,4", "link.sleep=", "link.sleep=-1,200,4",
               "link.sleep=100000,100000,0", "link.sleep= 100,200,4", "link.sleepy", "link.sleep", "link.sleep=00"]
        r = self.steps(["sw 0 link.sleep=160,320,2"] + [f"sw 0 {w}" for w in bad] + ["sw 0 link.sleep?"])
        self.assertEqual(r, [self.SLEEP_TUNED + ", no sleep grant yet"] + [self.SLEEP_USAGE] * len(bad) +
                         [self.SLEEP_TUNED + ", no sleep grant yet"])
        # The boundaries the spec allows: 1199/1199/1 (2398 < 2400), 6/6/0,
        # 3200/3200/0 (a 4 s interval needs a timeout over 8 s: refused),
        # 100/100/23 (2400: refused) and 100/100/22 (2300).
        r = self.steps(["sw 0 link.sleep=1199,1199,1", "sw 0 link.sleep=6,6,0", "sw 0 link.sleep=3200,3200,0",
                        "sw 0 link.sleep=100,100,23", "sw 0 link.sleep=100,100,22", "sw 0 link.wake?"])
        self.assertTrue(r[0].startswith("link.sleep 1199-1199/1/600 (link.sleep; 1498.75-1498.75 ms, latency 1, "
                                        "6000 ms timeout; outside Apple's rules, an iPhone may refuse it)"), r[0])
        self.assertTrue(r[1].startswith("link.sleep 6-6/0/600 (link.sleep; 7.50-7.50 ms, latency 0, 6000 ms timeout; "
                                        "outside Apple's rules"), r[1])
        self.assertEqual(r[2:4], [self.SLEEP_USAGE] * 2)
        self.assertTrue(r[4].startswith("link.sleep 100-100/22/600 (link.sleep; 125.00-125.00 ms, latency 22"), r[4])
        self.assertEqual(r[5], "notword")


if __name__ == "__main__":
    unittest.main()
