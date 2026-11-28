#!/usr/bin/env python3
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

"""The mic ring (components/muse/muse_mic_ring.c): its on/off state and
listener, what it paints over a picture's bands, and where the firmware turns
it on and draws it (voice loops, LVGL's top layer)."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_mic_ring.h"

static int calls, last;
static void listener(bool on) { calls++; last = on; }

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    int w = atoi(argv[1]), h = atoi(argv[2]), round = atoi(argv[3]), corner = atoi(argv[4]);
    printf("width %d\n", muse_mic_ring_width(w, h));
    muse_mic_ring_init(w, h, round, corner);
    muse_mic_ring_set_listener(listener);
    /* Off: nothing painted. */
    uint8_t *px = malloc((size_t)w * h * 2);
    memset(px, 0x11, (size_t)w * h * 2);
    muse_mic_ring_apply(px, w * 2, 0, 0, w, h);
    int painted = 0;
    for (int i = 0; i < w * h; i++) painted += px[2 * i] != 0x11 || px[2 * i + 1] != 0x11;
    printf("off_painted %d\n", painted);
    unsigned g0 = muse_mic_ring_generation();
    muse_mic_ring_set(true);
    muse_mic_ring_set(true);   /* idempotent */
    printf("on %d gen %u calls %d last %d\n", muse_mic_ring_on(), muse_mic_ring_generation() - g0, calls, last);
    /* The whole screen in bands of 7 rows, and once more in windows that
     * don't start at column 0, as draw_frame's pieces do. */
    for (int y = 0; y < h; y += 7) {
        int rows = h - y < 7 ? h - y : 7;
        muse_mic_ring_apply(px + (size_t)y * w * 2, w * 2, 0, y, w, rows);
    }
    uint8_t *px2 = malloc((size_t)w * h * 2);
    memset(px2, 0x11, (size_t)w * h * 2);
    int xs[] = { 0, w / 3, 2 * w / 3, w };
    for (int k = 0; k < 3; k++) {
        int x = xs[k], ww = xs[k + 1] - xs[k];
        uint8_t *tmp = malloc((size_t)ww * 2 * 8);
        for (int y = 0; y < h; y += 8) {
            int rows = h - y < 8 ? h - y : 8;
            for (int r = 0; r < rows; r++) memcpy(tmp + (size_t)r * ww * 2, px2 + ((size_t)(y + r) * w + x) * 2, (size_t)ww * 2);
            muse_mic_ring_apply(tmp, ww * 2, x, y, ww, rows);
            for (int r = 0; r < rows; r++) memcpy(px2 + ((size_t)(y + r) * w + x) * 2, tmp + (size_t)r * ww * 2, (size_t)ww * 2);
        }
        free(tmp);
    }
    printf("windows_match %d\n", memcmp(px, px2, (size_t)w * h * 2) == 0);
    /* The mask, one char per pixel: R ring, . untouched, ? anything else. */
    printf("mask\n");
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * 2;
            putchar(p[0] == 0xE1 && p[1] == 0xC7 ? 'R' : p[0] == 0x11 && p[1] == 0x11 ? '.' : '?');
        }
        putchar('\n');
    }
    free(px2);
    free(px);
    muse_mic_ring_set(false);
    printf("after_off %d calls %d last %d\n", muse_mic_ring_on(), calls, last);
    return 0;
}
"""


class MicRingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "h.c"
        src.write_text(HARNESS)
        cls.exe = Path(cls.tmp.name) / "h"
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-I", str(MUSE), str(src), str(MUSE / "muse_mic_ring.c"), "-lm", "-o", str(cls.exe)],
                       check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_ring(self, w, h, round_, corner=0):
        out = subprocess.run([str(self.exe), str(w), str(h), str(int(round_)), str(corner)], check=True,
                             capture_output=True, text=True).stdout
        head, mask = out.split("mask\n")
        rows = mask.splitlines()
        info = dict(line.split(" ", 1) for line in head.strip().splitlines())
        info["after_off"] = rows[-1].split(" ", 1)[1]
        return info, rows[:-1]


    def test_state_listener_and_off(self) -> None:
        info, rows = self.run_ring(64, 64, True)
        self.assertEqual(info["off_painted"], "0")
        self.assertEqual(info["on"], "1 gen 1 calls 1 last 1")   # a repeat isn't a change
        self.assertEqual(info["after_off"], "0 calls 2 last 0")
        self.assertEqual(info["windows_match"], "1")             # pieces paint as the whole does
        self.assertNotIn("?", "".join(rows))


class MicRingWiringTest(unittest.TestCase):
    def test_capture_turns_it_on_and_off(self) -> None:
        voice = (MUSE / "muse_voice.c").read_text()
        # record() (to Muse), mg_turn() (to the BLE client: push-to-talk and start_mic) and a call's open mic.
        rec = voice[voice.index("static bool record("):]
        rec = rec[:rec.index("\n}\n")]
        self.assertLess(rec.index("muse_mic_ring_set(true)"), rec.index("muse_audio_read"))
        self.assertIn("muse_mic_ring_set(false)", rec)
        mg = voice[voice.index("static void mg_turn("):]
        mg = mg[:mg.index("\n}\n")]
        # On before the first audio goes out, always (every capture
        # shows it, a released app's pre-live start_mic included).
        self.assertLess(mg.index("muse_mic_ring_set(true);"), mg.index("mg_voice_audio"))
        self.assertNotIn("call_pending()", mg)
        self.assertLess(mg.index("muse_mic_ring_set(false)"), mg.index("mg_voice_end"))
        call = voice[voice.index("static void run_call("):]
        call = call[:call.index("\n}\n")]
        self.assertIn("muse_mic_ring_set(mic);", call)
        self.assertLess(call.index("muse_mic_ring_set(mic);"), call.index("muse_mic_ring_set(false);"))
        # Not the idle pre-roll: nothing goes anywhere from it.
        idle = voice[voice.index("static void idle_capture("):]
        idle = idle[:idle.index("\n}\n")]
        self.assertNotIn("muse_mic_ring", idle)


if __name__ == "__main__":
    unittest.main()
