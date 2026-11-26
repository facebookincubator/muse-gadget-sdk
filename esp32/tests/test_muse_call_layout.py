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

"""The call controls panel (muse_call_controls.h) on every ESP32 call-video
panel: the original panel (big corner controls are for other gadget families only; every ESP32 board keeps
its old layout and sizes, with the soft volume − / +),
the same pixels on every board, every drawn shape inside the panel's real outline with 2 px to spare (checked
point by point against the outline), and every hit area at least 7 mm tall and wide on the screen (only the
hit rows grow). The conditions for the grown areas: no two overlap, none overlaps the
talk/hold region (the home's hold disc yields to the shown controls' hit areas, muse_call_controls_at), and every
one stays inside the panel's real outline (2 px to spare), checked pixel by pixel per board; taps land on the right
control."""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from test_muse_subs import home_geom_header

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

# name, w, h, round, corner, px per mm x 10 (each panel's physical pixels per mm)
PANELS = [
    ("m5sw", 466, 466, 1, 0, 106),
    ("scw1", 412, 412, 1, 0, 112),
    ("w175", 466, 466, 1, 0, 106),
    ("emo1", 480, 480, 0, 70, 124),
    ("w216", 480, 480, 0, 0, 111),
]

HARNESS = r"""
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* M_PI: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "muse_call_controls.h"
#include "home_geom.h"
static int W, H, ROUND, CORNER;
/* Inside the real outline with `spare` px. */
static int inside(double x, double y, double spare)
{
    if (ROUND) {
        return hypot(x - W / 2.0, y - H / 2.0) <= W / 2.0 - spare;
    }
    if (x < spare || y < spare || x > W - spare || y > H - spare) return 0;
    if (!CORNER) return 1;
    double ax = x < CORNER ? CORNER : x > W - CORNER ? W - CORNER : x;
    double ay = y < CORNER ? CORNER : y > H - CORNER ? H - CORNER : y;
    return hypot(x - ax, y - ay) <= CORNER - spare;
}
/* A rounded rectangle (screen coordinates, radius r) inside the outline with 2 px to spare: its outline traced. */
static int rrect_in(double x0, double y0, double x1, double y1, double r)
{
    for (int i = 0; i < 2000; i++) {
        double t = i / 2000.0 * 2 * M_PI, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        double hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r, c = cos(t), s = sin(t);
        double px = cx + (c >= 0 ? hx : -hx) + r * c, py = cy + (s >= 0 ? hy : -hy) + r * s;
        if (!inside(px, py, 2 - 0.5)) return 0;
    }
    return 1;
}
/* The screen pixels on row y that a tap on the visible panel reaches the control by. */
static int visible_run(const muse_call_panel_t *P, int y, muse_call_hit_t want)
{
    int n = 0, best = 0;
    for (int x = 0; x < W; x++) {
        int hit = inside(x + 0.5, y + 0.5, 0) && muse_call_ctl_hit(P, x - P->x, y - P->y) == want;
        n = hit ? n + 1 : 0;
        best = n > best ? n : best;
    }
    return best;
}
static int visible_col(const muse_call_panel_t *P, int x, muse_call_hit_t want)
{
    int n = 0, best = 0;
    for (int y = 0; y < H; y++) {
        int hit = inside(x + 0.5, y + 0.5, 0) && muse_call_ctl_hit(P, x - P->x, y - P->y) == want;
        n = hit ? n + 1 : 0;
        best = n > best ? n : best;
    }
    return best;
}
int main(int argc, char **argv)
{
    (void)argc;
    W = atoi(argv[1]); H = atoi(argv[2]); ROUND = atoi(argv[3]); CORNER = atoi(argv[4]);
    int ppm = atoi(argv[5]);
    muse_call_panel_t P;
    if (!muse_call_panel(W, H, ROUND, CORNER, ppm, &P)) { printf("none\n"); return 1; }
    int bad = 0;
    double ox = P.x, oy = P.y, bw = MUSE_CALL_BTN, by = MUSE_CALL_BTN_Y;
    /* Same pixels everywhere: centered, MUSE_CALL_CTL_BOTTOM above the bottom. */
    bad += P.x != (W - MUSE_CALL_CTL_W) / 2 || P.y != H - MUSE_CALL_CTL_BOTTOM - MUSE_CALL_CTL_H;
    int minus_in = rrect_in(ox, oy + by, ox + bw, oy + by + bw, bw / 2);
    int plus_in = rrect_in(ox + MUSE_CALL_CTL_W - bw, oy + by, ox + MUSE_CALL_CTL_W, oy + by + bw, bw / 2);
    int end_in = rrect_in(ox + MUSE_CALL_END_X0, oy + MUSE_CALL_END_Y0, ox + MUSE_CALL_END_X1, oy + MUSE_CALL_END_Y1,
                          (MUSE_CALL_END_Y1 - MUSE_CALL_END_Y0) / 2.0);
    int hint_in = rrect_in(ox + MUSE_CALL_HINT_X0, oy, ox + MUSE_CALL_HINT_X1, oy + MUSE_CALL_HINT_H, MUSE_CALL_HINT_H / 2.0);
    int bar_in = rrect_in(ox + MUSE_CALL_BAR_X0 - 8, oy + MUSE_CALL_BAR_Y0 - 8, ox + MUSE_CALL_BAR_X1 + 8,
                          oy + MUSE_CALL_BAR_Y1 + 8, 8);
    bad += !(minus_in && plus_in && end_in && hint_in && bar_in);
    /* Hit areas: the visible run across each control's center row and down its center column. */
    int cy = P.y + MUSE_CALL_BTN_Y + MUSE_CALL_BTN / 2, ey = P.y + (MUSE_CALL_END_Y0 + MUSE_CALL_END_Y1) / 2;
    int mw = visible_run(&P, cy, MUSE_CALL_HIT_MINUS), mh = visible_col(&P, P.x + MUSE_CALL_BTN / 2, MUSE_CALL_HIT_MINUS);
    int pw = visible_run(&P, cy, MUSE_CALL_HIT_PLUS), ph = visible_col(&P, P.x + MUSE_CALL_CTL_W - MUSE_CALL_BTN / 2, MUSE_CALL_HIT_PLUS);
    int ew = visible_run(&P, ey, MUSE_CALL_HIT_END), eh = visible_col(&P, W / 2, MUSE_CALL_HIT_END);
    int lim = (70 * ppm + 99) / 100;
    bad += mw < lim || mh < lim || pw < lim || ph < lim || ew < lim || eh < lim;
    /* Taps on the drawn shapes' centers. */
    bad += muse_call_ctl_hit(&P, MUSE_CALL_BTN / 2, cy - P.y) != MUSE_CALL_HIT_MINUS;
    bad += muse_call_ctl_hit(&P, MUSE_CALL_CTL_W - MUSE_CALL_BTN / 2, cy - P.y) != MUSE_CALL_HIT_PLUS;
    bad += muse_call_ctl_hit(&P, MUSE_CALL_CTL_W / 2, ey - P.y) != MUSE_CALL_HIT_END;
    bad += muse_call_ctl_hit(&P, MUSE_CALL_CTL_W / 2, cy - P.y) != MUSE_CALL_HIT_NONE;   /* the bar */
    /* The grown hit rows stay on the screen. */
    bad += MUSE_CALL_END_HIT_Y1 + P.end_down + P.y > H || MUSE_CALL_BTN_HIT_Y0 - P.btn_up < 0;
    /* (1) The grown areas never overlap: End call's rows below − and +'s, − left of +. */
    bad += !(MUSE_CALL_END_HIT_Y0 >= MUSE_CALL_BTN_HIT_Y1 && MUSE_CALL_BTN + MUSE_CALL_HIT_PX < MUSE_CALL_CTL_W - MUSE_CALL_BTN - MUSE_CALL_HIT_PX);
    /* (3) every pixel any control takes is inside the real outline with 2 px to spare. */
    int outside = 0, hold_px = 0, hold_yield = 0;
    muse_home_geom_t g;
    muse_home_geom(W, H, ROUND, CORNER, &g);
    double hcx = W / 2.0, hcy = H / 2.0 + g.muse_y, hr = g.hold_d / 2.0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int hit = muse_call_ctl_hit(&P, x - P.x, y - P.y) != MUSE_CALL_HIT_NONE;
            if (hit && !inside(x + 0.5, y + 0.5, 2)) {   /* the pixel's center, 2 px inside */
                outside++;
            }
            if (hypot(x + 0.5 - hcx, y + 0.5 - hcy) <= hr && inside(x + 0.5, y + 0.5, 0)) {
                hold_px++;
                hold_yield += hit;
            }
        }
    }
    bad += outside != 0;
    /* (2) the hold disc yields where the controls take a press (muse_home.c's on_hold, checked below), and keeps
     * Muse's center and most of itself for talking. */
    bad += muse_call_ctl_hit(&P, (int)hcx - P.x, (int)hcy - P.y) != MUSE_CALL_HIT_NONE || hold_yield * 2 > hold_px;
    printf("hold disc r%.0f at %.0f,%.0f: %d of %d px yield to the controls | outside %d\n", hr, hcx, hcy, hold_yield,
           hold_px, outside);
    double k = 10.0 / ppm;
    printf("minus d%.1fmm hit %.1fx%.1fmm | plus hit %.1fx%.1fmm | end %.1fx%.1fmm hit %.1fx%.1fmm | in %d%d%d%d%d | %s\n",
           bw * k, mw * k, mh * k, pw * k, ph * k, (MUSE_CALL_END_X1 - MUSE_CALL_END_X0) * k,
           (MUSE_CALL_END_Y1 - MUSE_CALL_END_Y0) * k, ew * k, eh * k, minus_in, plus_in, end_in, hint_in, bar_in,
           bad ? "BAD" : "ok");
    return bad != 0;
}
"""


class MuseCallLayoutTest(unittest.TestCase):
    def test_every_panel(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "h.c"), Path(t, "h")
            src.write_text(HARNESS)
            home_geom_header(Path(t, "home_geom.h"))   # the home's own hold disc (muse_home_geom)
            r = subprocess.run([*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", t,
                                "-I", str(MUSE), str(src), "-lm", "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            for name, w, h, rnd, corner, ppm in PANELS:
                with self.subTest(panel=name):
                    r = subprocess.run([str(exe), str(w), str(h), str(rnd), str(corner), str(ppm)], capture_output=True,
                                       text=True)
                    print(name, r.stdout.strip())
                    self.assertEqual(r.returncode, 0, r.stdout)

    def test_a_compact_end_call_button_where_the_panel_doesnt_fit(self) -> None:
        # A 320x240 touch board (M5Stack CoreS3) has no room for the panel; it
        # gets a compact End call button at the bottom middle, at least 7 mm tall, inside the screen; a tap on it
        # ends the call, a tap anywhere else (the calling screen, the face) doesn't. A screen the panel fits gets
        # no button.
        harness = r"""
#include <stdio.h>
#include "muse_call_controls.h"
int main(void)
{
    int bad = 0;
    muse_call_end_btn_t b;
    /* CoreS3: 320x240, 2.0" (about 200 px per inch: 7.9 px/mm). */
    bad += !muse_call_end_button(320, 240, 0, 79, &b);
    bad += b.x < 0 || b.y < 0 || b.x + b.w > 320 || b.y + b.h > 240 || b.h < (70 * 79 + 99) / 100 || b.w > 200;
    bad += b.y + b.h < 240 - 10;   /* at the bottom */
    bad += !muse_call_end_button_hit(&b, 160, b.y + b.h / 2);   /* on it: ends the call */
    bad += muse_call_end_button_hit(&b, 160, 120) || muse_call_end_button_hit(&b, 4, b.y + 2) ||
           muse_call_end_button_hit(&b, 160, b.y - 1);   /* the middle, beside it, just above: nothing */
    /* The 466 px round boards (and every panel board): the panel, no button. */
    bad += muse_call_end_button(466, 466, 1, 0, &b) || muse_call_end_button(320, 320, 0, 0, &b);
    printf("%s button %d,%d %dx%d\n", bad ? "BAD" : "ok", b.x, b.y, b.w, b.h);
    return bad != 0;
}
"""
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "end.c"), Path(t, "end")
            src.write_text(harness)
            r = subprocess.run([*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(src), "-o",
                                str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertTrue(r.stdout.startswith("ok"), r.stdout)
        # muse_call.c: built on a touch board without the panel, shown for the whole call while LVGL has the
        # screen, and tap() ends the call only on it (the same hang-up as the panel's End call).
        call = (MUSE / "muse_call.c").read_text()
        self.assertIn("if (muse_board->touch && muse_call_end_button(w, h, muse_board->round, ppm10, &s_eb)) {", call)
        tap = call[call.index("static void tap(int x, int y, bool was_up)\n{"):]
        tap = tap[:tap.index("\n}\n")]
        first = tap[:tap.index("muse_call_hit_t hit")]
        self.assertIn("muse_call_end_button_hit(&s_eb, x, y)", first)
        self.assertIn('muse_call_end_touch("End call button", x, y,', first)
        sync = call[call.index("static void ctl_sync(void)\n{"):]
        self.assertIn("bool on = call_phase() != MUSE_CALL_NONE && !drawing;", sync[:sync.index("\n}\n")])

    def test_a_tap_away_dismisses_the_controls(self) -> None:
        # A tap away from the controls dismisses them. Hidden, a tap brings them up; up when the touch went down, a tap away dismisses them at once and a
        # tap on a control acts and re-arms the 3 s; a hold away (250 ms or more: not a tap) keeps them; with no
        # tap they hide at 3 s. muse_call_tap, as muse_call.c's tap() runs it (up = up at the touch-down).
        harness = r"""
#include <stdio.h>
#include "muse_call_controls.h"
#define S 1000000LL
int main(void)
{
    long long until = 0;
    int bad = 0;
    /* Hidden: the first tap only brings them up, 3 s from it (a tap on where a control would be too). */
    bad += muse_call_tap(10 * S, &until, 0, MUSE_CALL_HIT_PLUS, 0) != MUSE_CALL_TAP_SHOWN || until != 13 * S;
    /* No tap: up at 2.999 s, hidden at 3 s. */
    bad += !muse_call_controls_up(13 * S - 1, until) || muse_call_controls_up(13 * S, until);
    /* Up, a tap away from the controls (anywhere on the picture) at 11 s: hidden at once. */
    until = 13 * S;
    bad += muse_call_tap(11 * S, &until, 1, MUSE_CALL_HIT_NONE, 0) != MUSE_CALL_TAP_DISMISSED;
    bad += muse_call_controls_up(11 * S, until);
    /* Up, a tap on a control acts (End call, - and +) and keeps them, the 3 s re-armed from it. */
    const muse_call_hit_t hits[3] = { MUSE_CALL_HIT_END, MUSE_CALL_HIT_MINUS, MUSE_CALL_HIT_PLUS };
    for (int k = 0; k < 3; k++) {
        until = 21 * S;
        bad += muse_call_tap(20 * S, &until, 1, hits[k], 0) != MUSE_CALL_TAP_CONTROL || until != 23 * S;
        bad += muse_call_tap(20 * S, &until, 1, hits[k], 1) != MUSE_CALL_TAP_CONTROL || until != 23 * S;   /* held */
    }
    /* Up, a hold away (not a tap): they stay, 3 s from its lift. */
    until = 31 * S;
    bad += muse_call_tap(30 * S, &until, 1, MUSE_CALL_HIT_NONE, 1) != MUSE_CALL_TAP_KEPT || until != 33 * S;
    printf("%s\n", bad ? "BAD" : "ok");
    return bad != 0;
}
"""
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "tap.c"), Path(t, "tap")
            src.write_text(harness)
            r = subprocess.run([*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(src), "-o",
                                str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, "ok"), r.stdout)
        # tap() runs it with up = up at this touch's touch-down (the touch that showed them never dismisses them),
        # hides them on DISMISSED, and the stats overlay's fifth tap hides them again.
        call = (MUSE / "muse_call.c").read_text()
        tap = call[call.index("static void tap(int x, int y, bool was_up)\n{"):]
        tap = tap[:tap.index("\n}\n")]
        self.assertIn("muse_call_tap(now_us(), &until, was_up && controls_shown(), hit, held);", tap)
        self.assertIn("bool held = now_us() - s_press.t0_us >= MUSE_CALL_TALK_HOLD_US;", tap)
        self.assertIn("s_controls_until_us = until;\n    if (what == MUSE_CALL_TAP_DISMISSED) {", tap)
        self.assertNotIn("height / 2", tap)   # the whole picture, not its top half
        fifth = tap[tap.index("case MUSE_VSTATS_TAP_SHOW:"):]
        self.assertIn("s_controls_until_us = 0;   /* the fifth hides the controls */", fifth[:fifth.index("return;")])
        self.assertNotIn("keep_controls", call)

    def test_touch_down_or_lift(self) -> None:
        # From filmed calls: off a talk surface the controls come up at the touch-down, a
        # drag or a hold keeps them, 3 s from the last touch; on a talk surface they wait for a tap's lift, and a
        # hold (talk) shows nothing. muse_call_touch_*, as muse_call.c's press_down / press_move / press_up run them.
        harness = r"""
#include <stdio.h>
#include "muse_call_controls.h"
#define S 1000000LL
#define MS 1000LL
int main(void)
{
    int bad = 0;
    muse_call_touch_t t = { 0 };
    long long until = 0;
    /* Not a talk surface: up at the touch-down sample (hidden before it: the lift only brought them up). */
    bad += !muse_call_touch_down(&t, 10 * S, &until, 0, 0) || !muse_call_controls_up(10 * S, until) || t.was_up;
    /* A drag (moves) and a hold (no moves, 5 s): up throughout, 3 s from the last touch. */
    for (long long now = 10 * S; now <= 15 * S; now += 10 * MS) {
        bad += !muse_call_controls_up(now, until);
        muse_call_touch_held(&t, now, &until);
    }
    /* The lift at 15 s after a drag: no tap; up until 18 s, hidden at 18 s. */
    bad += muse_call_touch_up(&t, 15 * S, &until, 1) != MUSE_CALL_LIFT_NONE;
    bad += !muse_call_controls_up(18 * S - 1, until) || muse_call_controls_up(18 * S, until);
    /* The touch that showed them (a tap): its lift is a tap with was_up false, which keeps them (muse_call_tap:
     * SHOWN, never DISMISSED); a second tap away (up at its touch-down) dismisses them at once. */
    until = 0;
    muse_call_touch_down(&t, 16 * S, &until, 0, 0);
    bad += muse_call_touch_up(&t, 16 * S + 80 * MS, &until, 0) != MUSE_CALL_LIFT_TAP || t.was_up;
    bad += muse_call_tap(16 * S + 80 * MS, &until, t.was_up, MUSE_CALL_HIT_NONE, 0) != MUSE_CALL_TAP_SHOWN;
    bad += !muse_call_controls_up(17 * S, until);
    muse_call_touch_down(&t, 17 * S, &until, muse_call_controls_up(17 * S, until), 0);
    bad += muse_call_touch_up(&t, 17 * S + 80 * MS, &until, 0) != MUSE_CALL_LIFT_TAP || !t.was_up;
    bad += muse_call_tap(17 * S + 80 * MS, &until, t.was_up, MUSE_CALL_HIT_NONE, 0) != MUSE_CALL_TAP_DISMISSED;
    bad += muse_call_controls_up(17 * S + 80 * MS, until);
    /* A drag that showed them: no tap at its lift (moved), so nothing dismisses them; up 3 s from the lift. */
    until = 0;
    muse_call_touch_down(&t, 18 * S, &until, 0, 0);
    muse_call_touch_held(&t, 18 * S + 500 * MS, &until);
    bad += muse_call_touch_up(&t, 18 * S + 500 * MS, &until, 1) != MUSE_CALL_LIFT_NONE;
    bad += !muse_call_controls_up(21 * S + 499 * MS, until) || muse_call_controls_up(21 * S + 500 * MS, until);
    /* A hold, lifted without moving: a tap, not acting (they were hidden at its touch-down). */
    until = 0;
    muse_call_touch_down(&t, 20 * S, &until, 0, 0);
    muse_call_touch_held(&t, 24 * S, &until);
    bad += muse_call_touch_up(&t, 24 * S, &until, 0) != MUSE_CALL_LIFT_TAP || t.was_up || until != 27 * S;
    /* Up at the touch-down: the lift acts on the control under it (was_up), and keeps them. */
    muse_call_touch_down(&t, 25 * S, &until, 1, 0);
    bad += muse_call_touch_up(&t, 25 * S + 80 * MS, &until, 0) != MUSE_CALL_LIFT_TAP || !t.was_up;
    bad += until != 28 * S + 80 * MS;
    /* A talk surface: nothing at the touch-down; a tap's lift is a tap (tap() shows them). */
    until = 0;
    bad += muse_call_touch_down(&t, 30 * S, &until, 0, 1) || muse_call_controls_up(30 * S, until);
    muse_call_touch_held(&t, 30 * S + 100 * MS, &until);
    bad += until != 0;
    bad += muse_call_touch_up(&t, 30 * S + 120 * MS, &until, 0) != MUSE_CALL_LIFT_TAP;
    /* A hold there (talk, 250 ms or longer): nothing at its lift, no controls flash. */
    muse_call_touch_down(&t, 40 * S, &until, 0, 1);
    bad += muse_call_touch_up(&t, 40 * S + 250 * MS, &until, 0) != MUSE_CALL_LIFT_NONE || until != 0;
    muse_call_touch_down(&t, 41 * S, &until, 0, 1);
    bad += muse_call_touch_up(&t, 41 * S + 249 * MS, &until, 0) != MUSE_CALL_LIFT_TAP;   /* just under: a tap */
    /* A lift with no touch-down does nothing. */
    bad += muse_call_touch_up(&t, 50 * S, &until, 0) != MUSE_CALL_LIFT_NONE;
    printf("%s\n", bad ? "BAD" : "ok");
    return bad != 0;
}
"""
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "touch.c"), Path(t, "touch")
            src.write_text(harness)
            r = subprocess.run([*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(src), "-o",
                                str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, "ok"), r.stdout)
        # muse_call.c wires them: the video's touches are never a talk surface; the face's ask the home.
        call = (MUSE / "muse_call.c").read_text()
        self.assertIn("press_down(d.point.x, d.point.y, false, false);", call)
        self.assertIn("press_down(p.x, p.y, press_talks(p.x, p.y), true);", call)
        self.assertIn("return !s_calling_up && muse_home_talk_at(x, y);", call)
        home = (MUSE / "muse_home.c").read_text()
        talk = home[home.index("bool muse_home_talk_at(int x, int y)\n{"):]
        talk = talk[:talk.index("\n}\n")]
        self.assertIn("!screen_talks()", talk)            # Settings > Talk on Button: not a talk surface
        self.assertIn("muse_call_controls_at(x, y)", talk)

if __name__ == "__main__":
    unittest.main()
