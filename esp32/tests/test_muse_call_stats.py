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

"""The call video's stats overlay.

Five taps on the call video (each within ~400 ms of the last) show it: the
first tap still toggles the controls, taps 2-4 do nothing, the fifth hides the
controls; one tap hides it (consumed); hidden at every call's start and end.
Its lines, refreshed once a second, leave out what isn't measured. A console
word toggles it (an automation word) and its lines print on the USB console,
never the Nordic UART. muse_call_stats.c runs here; the rest checks
muse_call.c's, muse_video.c's and mg_ble.c's wiring.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
MG = ROOT / "components" / "muse_gadget_ble"

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_call_stats.h"

/* One command a line:
 *   tap T B        a tap at T ms (B 1: on a control button): prints normal, swallow, show or hide
 *   edge           a call starts or ends
 *   reset          a call starts: its totals from the next sample
 *   open           the overlay opens: totals stay, rates start over
 *   s k=v ...      a sample (fields below; unset ones 0 / NULL): prints its lines, "|" between
 *   place W H R C BW BH   a BW x BH box on a W x H panel (R round, C corner px): "x y" or "none" */
static const char *str(const char *v) { return strcmp(v, "-") ? strdup(v) : NULL; }

int main(void)
{
    static char line[1024];
    muse_vstats_taps_t t = { 0 };
    muse_vstats_acc_t a = { 0 };
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "tap ", 4)) {
            long long ms;
            int b;
            sscanf(line + 4, "%lld %d", &ms, &b);
            static const char *const R[] = { "normal", "swallow", "show", "hide" };
            printf("%s\n", R[muse_vstats_tap(&t, ms, b != 0)]);
        } else if (!strcmp(line, "edge")) {
            muse_vstats_call_edge(&t);
            printf("edge\n");
        } else if (!strcmp(line, "reset")) {
            muse_vstats_acc_reset(&a);
            printf("reset\n");
        } else if (!strcmp(line, "open")) {
            muse_vstats_acc_open(&a);
            printf("open\n");
        } else if (!strncmp(line, "s ", 2)) {
            muse_vstats_sample_t s;
            memset(&s, 0, sizeof(s));
            for (char *tok = strtok(line + 2, " "); tok; tok = strtok(NULL, " ")) {
                char *eq = strchr(tok, '=');
                if (!eq) continue;
                *eq = 0;
                const char *k = tok, *v = eq + 1;
                long long n = atoll(v);
                if (!strcmp(k, "t")) s.t_ms = n;
                else if (!strcmp(k, "video")) s.video = n;
                else if (!strcmp(k, "codec")) s.codec = str(v);
                else if (!strcmp(k, "cw")) s.coded_w = (uint16_t)n;
                else if (!strcmp(k, "ch")) s.coded_h = (uint16_t)n;
                else if (!strcmp(k, "sw")) s.shown_w = (uint16_t)n;
                else if (!strcmp(k, "sh")) s.shown_h = (uint16_t)n;
                else if (!strcmp(k, "rx")) s.rx_bytes = (uint32_t)n;
                else if (!strcmp(k, "shown")) s.shown = (uint16_t)n;
                else if (!strcmp(k, "err")) s.dec_errors = (uint32_t)n;
                else if (!strcmp(k, "drop")) s.drops = (uint32_t)n;
                else if (!strcmp(k, "kf")) s.kf_requests = (uint32_t)n;
                else if (!strcmp(k, "link")) s.link = n;
                else if (!strcmp(k, "itvl")) s.itvl_1250us = (uint16_t)n;
                else if (!strcmp(k, "mode")) s.link_mode = str(v);
                else if (!strcmp(k, "play")) s.play = n;
                else if (!strcmp(k, "stream")) s.play_stream = (uint32_t)n;
                else if (!strcmp(k, "under")) s.play_underruns = (uint32_t)n;
                else if (!strcmp(k, "depth")) s.play_depth_ms = (uint32_t)n;
            }
            char lines[MUSE_VSTATS_LINES][MUSE_VSTATS_COLS];
            int n = muse_vstats_lines(&a, &s, lines);
            for (int i = 0; i < n; i++) printf("%s%s", i ? "|" : "", lines[i]);
            printf("\n");
            free((char *)s.codec);   /* the sample's own copies: nothing keeps them past its lines */
            free((char *)s.link_mode);
        } else if (!strncmp(line, "place ", 6)) {
            int w, h, r, c, bw, bh;
            sscanf(line + 6, "%d %d %d %d %d %d", &w, &h, &r, &c, &bw, &bh);
            int s = w < h ? w : h, ring = (int)(s * 0.022f + 0.5f);
            muse_vstats_box_t b;
            if (muse_vstats_place(w, h, r != 0, c, ring < 2 ? 2 : ring, 4, bw, bh, &b)) {
                printf("%d %d\n", b.x, b.y);
            } else {
                printf("none\n");
            }
        }
    }
    return 0;
}
"""


class CallStatsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "call_stats"
        proc = subprocess.run([*cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                               "-I", str(MUSE), str(d / "driver.c"), str(MUSE / "muse_call_stats.c"), "-lm", "-o",
                               str(cls.exe)], text=True, capture_output=True)
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_lines(self, lines: list[str]) -> list[str]:
        return subprocess.run([str(self.exe)], input="\n".join(lines) + "\n", text=True, capture_output=True,
                              check=True).stdout.splitlines()

    # ---- the taps ----

    def test_five_taps_show_it_the_first_still_toggles(self) -> None:
        self.assertEqual(self.run_lines(["tap 1000 0", "tap 1300 0", "tap 1600 0", "tap 1900 0", "tap 2200 0"]),
                         ["normal", "swallow", "swallow", "swallow", "show"])
        # Each within 400 ms of the last, not of the first: 5 taps over 1.6 s show it.
        self.assertEqual(self.run_lines(["tap 0 0", "tap 400 0", "tap 800 0", "tap 1200 0", "tap 1600 0"])[-1], "show")

    def test_one_tap_hides_it_consumed(self) -> None:
        out = self.run_lines(["tap 0 0", "tap 100 0", "tap 200 0", "tap 300 0", "tap 400 0",   # shown
                              "tap 5000 0",                                                     # hide
                              "tap 5100 0",                                                     # a new run's first
                              "tap 9000 1"])                                                    # a button while hidden
        self.assertEqual(out[4:], ["show", "hide", "normal", "normal"])
        # A tap right after the show hides it too (whatever it lands on).
        self.assertEqual(self.run_lines(["tap 0 0", "tap 1 0", "tap 2 0", "tap 3 0", "tap 4 0", "tap 5 1"])[-1], "hide")

    def test_a_slow_tap_starts_over(self) -> None:
        self.assertEqual(self.run_lines(["tap 0 0", "tap 300 0", "tap 600 0", "tap 1001 0", "tap 1300 0",
                                         "tap 1600 0", "tap 1900 0", "tap 2200 0"]),
                         ["normal", "swallow", "swallow", "normal", "swallow", "swallow", "swallow", "show"])

    def test_a_button_tap_is_the_buttons_and_breaks_the_run(self) -> None:
        # Quick "+" presses on the shown controls keep stepping the volume: never swallowed.
        self.assertEqual(self.run_lines(["tap 0 0", "tap 200 1", "tap 400 1", "tap 600 0", "tap 800 0"]),
                         ["normal", "normal", "normal", "normal", "swallow"])

    def test_hidden_at_every_call_edge(self) -> None:
        out = self.run_lines(["tap 0 0", "tap 100 0", "tap 200 0", "tap 300 0", "tap 400 0", "edge", "tap 500 0",
                              "tap 600 0", "tap 700 0", "edge", "tap 800 0"])
        # Shown, then a call edge: the next tap is a plain first tap (no hide); an edge mid-run starts over.
        self.assertEqual(out[4:], ["show", "edge", "normal", "swallow", "swallow", "edge", "normal"])

    # ---- the text ----

    def test_lines_and_the_last_seconds_rates(self) -> None:
        base = ("video=1 codec=H.264 cw=232 ch=232 sw=466 sh=466 link=1 itvl=12 mode=active play=1 stream=3 "
                "depth=80")
        out = self.run_lines(["reset",
                              f"s t=1000 {base} rx=100000 shown=100 err=2 drop=5 kf=7 under=1",
                              f"s t=2000 {base} rx=139000 shown=124 err=2 drop=6 kf=8 under=2"])
        first = out[1].split("|")
        # No rate from one sample; totals count from the call's start (its base sample).
        self.assertEqual(first, ["H.264 232x232>466x466", "err 0 drop 0 kf 0", "link 15.00 ms active",
                                 "reply 80 ms under 0"])
        self.assertEqual(out[2].split("|"), ["H.264 232x232>466x466", "312 kb/s 24 fps", "err 0 drop 1 kf 1",
                                             "link 15.00 ms active", "reply 80 ms under 1"])
        for line in out[1:]:
            for part in line.split("|"):
                self.assertLess(len(part), 24, part)   # MUSE_VSTATS_COLS with its terminator

    def test_whats_not_measured_is_left_out(self) -> None:
        out = self.run_lines(["reset",
                              "s t=0 video=1 codec=- link=0 play=0",           # nothing but the counters
                              "reset",
                              "s t=0 video=0 link=1 itvl=0 mode=sleep play=0",  # no video, a link without its interval
                              "reset",
                              "s t=0 video=0 link=1 itvl=80 mode=- play=1 depth=0",
                              "reset",
                              "s t=0 video=0 link=0 play=0"])
        self.assertEqual(out[1], "err 0 drop 0 kf 0")
        self.assertEqual(out[3], "link sleep")
        self.assertEqual(out[5], "link 100.00 ms|reply 0 ms under 0")
        self.assertEqual(out[7], "")
        self.assertNotIn(" 0 kb/s", "".join(out))

    def test_opened_mid_call_the_totals_are_the_calls(self) -> None:
        # The base is the call's start; opening the overlay later keeps the totals, restarts the rates.
        out = self.run_lines(["reset",
                              "s t=0 video=1 rx=0 shown=0 err=0 drop=0 kf=10 play=1 stream=5 under=2",   # the base
                              "open",
                              "s t=60000 video=1 rx=900000 shown=1500 err=1 drop=4 kf=13 play=1 stream=6 under=1",
                              "s t=61000 video=1 rx=930000 shown=1525 err=1 drop=4 kf=13 play=1 stream=6 under=1"])
        self.assertIn("err 1 drop 4 kf 3", out[3])
        self.assertIn("under 1", out[3])          # stream 5's 2 were before the call
        self.assertNotIn("kb/s", out[3])          # no rate across the time it was hidden
        self.assertIn("240 kb/s 25 fps", out[4])  # one second since the opening

    def test_underruns_add_up_across_reply_streams(self) -> None:
        out = self.run_lines(["reset",
                              "s t=0 play=1 stream=1 under=0 depth=60",
                              "s t=1000 play=1 stream=1 under=3 depth=60",
                              "s t=2000 play=1 stream=2 under=1 depth=20",   # a new reply: its own count from 0
                              "s t=3000 play=1 stream=2 under=2 depth=0"])
        self.assertEqual(out[1:], ["reply 60 ms under 0", "reply 60 ms under 3", "reply 20 ms under 4",
                                   "reply 0 ms under 5"])

    def test_rates_only_within_one_stream(self) -> None:
        out = self.run_lines(["reset",
                              "s t=0 video=1 codec=VP8 cw=240 ch=240 rx=500000 shown=900",
                              "s t=1000 video=1 codec=VP8 cw=240 ch=240 rx=1000 shown=2",       # a new stream: no rate
                              "s t=2000 video=1 codec=VP8 cw=240 ch=240 rx=26000 shown=27"])
        self.assertNotIn("kb/s", out[2])
        self.assertIn("200 kb/s 25 fps", out[3])
        # The shown counter wraps at 2^16 (the video core's 16 bits).
        out = self.run_lines(["reset", "s t=0 video=1 rx=0 shown=65530", "s t=1000 video=1 rx=0 shown=14"])
        self.assertIn("0 kb/s 20 fps", out[2])

    # ---- where it goes ----

    # ---- the wiring ----

    def call_src(self) -> str:
        return (MUSE / "muse_call.c").read_text()

if __name__ == "__main__":
    unittest.main()
