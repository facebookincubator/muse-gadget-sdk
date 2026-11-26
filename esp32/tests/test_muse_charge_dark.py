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

"""Charge dark ("plugging in should turn on").

USB turns an off gadget on (its power chip does); on a low cell the gadget
charges dark and boots once the cell is healthy. The decision is pure C
(muse_charge_dark_core.c), run here through a small driver.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "muse_charge_dark.h"

/* One reading a line: "<ok> <usb> <usb_on> <key> <mv> <now_ms>", threshold 3600
 * (muse_cd_healthy_mv(3300)). Prints "<action> <why> <dark>". "reset" starts a new power-up. */
int main(void)
{
    muse_cd_state_t st;
    memset(&st, 0, sizeof(st));
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "reset", 5)) {
            memset(&st, 0, sizeof(st));
            continue;
        }
        int ok, usb, usb_on, key, mv;
        long long now;
        if (sscanf(line, "%d %d %d %d %d %lld", &ok, &usb, &usb_on, &key, &mv, &now) != 6) {
            continue;
        }
        muse_cd_reading_t r = { .usb = usb, .usb_on = usb_on, .by_key = key, .battery_mv = mv };
        muse_cd_why_t why;
        muse_cd_action_t a = muse_cd_decide(&st, ok, &r, muse_cd_healthy_mv(3300), now, &why);
        printf("%d %d %d\n", a, why, st.dark);
    }
    return 0;
}
"""

BOOT, WAIT, OFF = 0, 1, 2
NOT_USB, UNREADABLE, NO_BATTERY, HEALTHY, LOW, KEY, UNPLUGGED, LIMIT = range(8)
MAX_MS = 2 * 60 * 60 * 1000


def r(ok=1, usb=1, usb_on=1, key=0, mv=3400, now=0) -> str:
    return f"{ok} {usb} {usb_on} {key} {mv} {now}"


class ChargeDarkDecisionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "driver.c"
        src.write_text(DRIVER)
        cls.exe = Path(cls.tmp.name) / "driver"
        res = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(src),
                              str(MUSE / "muse_charge_dark_core.c"), "-o", str(cls.exe)], text=True,
                             capture_output=True)
        if res.returncode:
            raise AssertionError(res.stdout + res.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_readings(self, *lines: str) -> list[tuple[int, int, int]]:
        res = subprocess.run([str(self.exe)], input="".join(l + "\n" for l in lines), text=True,
                             capture_output=True, check=True)
        return [tuple(int(x) for x in l.split()) for l in res.stdout.splitlines()]

    def test_a_healthy_cell_boots_at_once(self) -> None:
        self.assertEqual(self.run_readings(r(mv=3900)), [(BOOT, HEALTHY, 0)])
        self.assertEqual(self.run_readings(r(mv=3600)), [(BOOT, HEALTHY, 0)])

    def test_only_a_usb_power_up_of_a_low_cell_holds(self) -> None:
        self.assertEqual(self.run_readings(r(usb=0, usb_on=0, mv=3350))[0][:2], (BOOT, NOT_USB))   # battery
        self.assertEqual(self.run_readings(r(usb_on=0))[0][:2], (BOOT, NOT_USB))   # USB, but not what turned it on
        self.assertEqual(self.run_readings(r(key=1))[0][:2], (BOOT, KEY))   # a press turned it on
        self.assertEqual(self.run_readings(r(ok=0))[0][:2], (BOOT, UNREADABLE))
        self.assertEqual(self.run_readings(r(mv=0))[0][:2], (BOOT, NO_BATTERY))
        self.assertEqual(self.run_readings(r(mv=3400)), [(WAIT, LOW, 1)])
        self.assertEqual(self.run_readings(r(mv=2400)), [(WAIT, LOW, 1)])   # past the PM1's 2.5 V cut-off

    def test_charges_dark_then_boots_once_healthy(self) -> None:
        out = self.run_readings(r(mv=3400, now=0), r(mv=3500, now=5000), r(mv=3610, now=10000),
                                r(mv=3590, now=15000), r(mv=3605, now=20000), r(mv=3620, now=25000))
        self.assertEqual([o[0] for o in out], [WAIT, WAIT, WAIT, WAIT, WAIT, BOOT])
        self.assertEqual(out[-1][1:], (HEALTHY, 0))   # two healthy readings in a row: one dip starts over

    def test_no_boot_loop(self) -> None:
        # However long it stays low, the hold never boots, restarts or powers off on USB.
        lines = [r(mv=3300 + i, now=i * 5000) for i in range(200)]
        self.assertTrue(all(o[0] == WAIT for o in self.run_readings(*lines)))

    def test_unplugged_low_powers_off_again(self) -> None:
        out = self.run_readings(r(mv=3400), r(usb=0, mv=3380, now=5000), r(usb=0, mv=3380, now=10000))
        self.assertEqual(out[1][0], WAIT)   # one reading without USB is a glitch
        self.assertEqual(out[2][:2], (OFF, UNPLUGGED))
        out = self.run_readings(r(mv=3400), r(usb=0, mv=3380, now=5000), r(mv=3400, now=10000),
                                r(usb=0, mv=3380, now=15000))
        self.assertEqual([o[0] for o in out], [WAIT, WAIT, WAIT, WAIT])   # USB back in between
        out = self.run_readings(r(mv=3400), r(usb=0, mv=3650, now=5000))
        self.assertEqual(out[1][:2], (BOOT, HEALTHY))   # charged by then: it may run on battery

    def test_a_key_or_the_limit_ends_the_hold(self) -> None:
        out = self.run_readings(r(mv=3400), r(mv=3400, key=1, now=5000))
        self.assertEqual(out[1][:2], (BOOT, KEY))
        out = self.run_readings(r(mv=3400), r(mv=3400, now=MAX_MS - 1), r(mv=3400, now=MAX_MS))
        self.assertEqual([o[:2] for o in out[1:]], [(WAIT, LOW), (BOOT, LIMIT)])
        out = self.run_readings(r(mv=3400), r(ok=0, now=5000))
        self.assertEqual(out[1][:2], (BOOT, UNREADABLE))   # fail open: USB powers the board


if __name__ == "__main__":
    unittest.main()
