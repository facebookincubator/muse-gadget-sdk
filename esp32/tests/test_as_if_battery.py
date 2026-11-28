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

"""A bench nap (">nap": as if on battery) makes the gadget behave as on battery but never power itself off on USB
(an ESP-Mosaico stranded that way restarts only on its ON/OFF switch, not on USB).
The as-if battery ends by itself after 10 min (logged), and everything that cuts power
goes by the real power state (muse_state_on_battery_real, muse_power_off_ok): the Power off a board only allows on
battery (Settings, the app's power_off, the power-off itself), and the low-battery shutdown (by the
reading's own USB flag). What may use the as-if battery only changes behavior: the codecs resting, the display
pause (the CPU stays at full speed with a USB host), the charged-screen hold and the battery meter's run."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_muse_battery import build_harness  # noqa: E402
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"


class Expiry(unittest.TestCase):
    def test_ten_minutes_then_off_and_what_may_cut_power(self) -> None:
        harness = r"""
#include <stdio.h>
#include "muse_state.h"
#define MIN 60000000LL
int main(void)
{
    int bad = 0;
    bool expired;
    muse_as_if_t a = { 0 };
    bad += MUSE_AS_IF_BATTERY_US != 10 * MIN;
    bad += muse_as_if_on(&a, 0, &expired) || expired;             /* off until a nap */
    muse_as_if_set(&a, true, 5 * MIN);
    bad += !muse_as_if_on(&a, 15 * MIN - 1, &expired) || expired;  /* on for 10 min... */
    bad += muse_as_if_on(&a, 15 * MIN, &expired) || !expired;     /* ...then off by itself, said once */
    bad += muse_as_if_on(&a, 16 * MIN, &expired) || expired;
    muse_as_if_set(&a, true, 20 * MIN);                            /* a new nap: 10 min from it */
    muse_as_if_set(&a, false, 21 * MIN);                           /* an existing clear still clears */
    bad += muse_as_if_on(&a, 22 * MIN, &expired) || expired;
    /* The real power: USB is never battery, whatever a nap says; no battery isn't either. */
    muse_power_t usb = { .battery_pct = 80, .usb = true }, batt = { .battery_pct = 80 }, none = { .battery_pct = -1 };
    bad += muse_power_real_battery(&usb) || !muse_power_real_battery(&batt) || muse_power_real_battery(&none);
    /* A board whose latch USB holds (the ESP-Mosaico) powers off only really on battery; others as before. */
    bad += muse_power_off_ok(true, &usb) || !muse_power_off_ok(true, &batt) || muse_power_off_ok(true, &none);
    bad += !muse_power_off_ok(false, &usb) || !muse_power_off_ok(false, &batt);
    printf("%s\n", bad ? "BAD" : "ok");
    return bad;
}
"""
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "h.c"), Path(t, "h")
            src.write_text(harness)
            r = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE),
                                str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)


class NeverCutsPowerOnUsb(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = build_harness(Path(cls.tmp.name) / "muse_battery_harness")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def events(self, *commands: str) -> list[str]:
        r = run_child([str(self.binary)], input="\n".join(commands) + "\n", capture_output=True, text=True,
                      timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return [ln for ln in r.stdout.splitlines() if ln.startswith(("@ui.", "@poweroff", "level"))]

    def test_low_battery_shutdown(self) -> None:
        # A nap on USB with the cell reading under the shutdown threshold: nothing is low, nothing shuts down.
        ev = self.events("lowmv 3300", "nap 1", *["p 1 3 3200"] * 8, "level")
        self.assertNotIn("@poweroff", ev)
        self.assertEqual(ev, ["level 0"])
        # Real battery is unchanged, nap or not: five low readings in a row power it off.
        for nap in ("nap 0", "nap 1"):
            with self.subTest(nap=nap):
                ev = self.events("lowmv 3300", nap, *["p 0 3 3290"] * 5, "level")
                self.assertEqual(ev[-3:], ["@ui.lowbat 3 3", "@poweroff", "level 3"])


if __name__ == "__main__":
    unittest.main()
