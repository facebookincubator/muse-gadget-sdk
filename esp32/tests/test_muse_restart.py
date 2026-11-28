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

"""Intentional restarts vs real watchdog resets (components/muse_restart).

On the ESP32-S31 an esp_restart() can come back as ESP_RST_WDT (its own RTC
watchdog completes it). The marker muse_restart() leaves in RTC memory makes
that boot read as a software restart; any watchdog reset without it, and task
or interrupt watchdogs and panics always, stay what they are.
"""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "components" / "muse_restart"

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "muse_restart.h"

int main(void)
{
    muse_restart_mark_t m;
    memset(&m, 0xA5, sizeof(m));                     /* what a power-on can leave */
    printf("garbage_valid %d\n", muse_restart_mark_valid(&m));
    memset(&m, 0, sizeof(m));
    printf("zero_valid %d\n", muse_restart_mark_valid(&m));
    muse_restart_mark_set(&m, MUSE_RESTART_CONSOLE, 1);
    printf("set_valid %d why %u core %u\n", muse_restart_mark_valid(&m), (unsigned)m.why, (unsigned)m.core);
    m.why ^= 1;                                      /* a flipped bit */
    printf("flipped_valid %d\n", muse_restart_mark_valid(&m));
    muse_restart_mark_set(&m, MUSE_RESTART_OTA, 0);
    muse_restart_mark_clear(&m);
    printf("cleared_valid %d\n", muse_restart_mark_valid(&m));
    for (int marked = 0; marked < 2; marked++) {
        printf("effective%d", marked);
        for (int r = 0; r <= 15; r++) {
            printf(" %d", muse_restart_effective_reason(r, marked));
        }
        printf("\n");
    }
    /* failure marker */
    memset(&m, 0xA5, sizeof(m));
    printf("fail_garbage %d\n", muse_restart_fail_reason(&m));
    muse_restart_fail_set(&m, MUSE_RST_PANIC, 1);
    printf("fail_panic %d\n", muse_restart_fail_reason(&m));
    printf("fail_as_planned %d\n", muse_restart_mark_valid(&m));
    muse_restart_fail_set(&m, MUSE_RST_UNKNOWN, 0);
    printf("fail_none %d\n", muse_restart_fail_reason(&m));
    m.core ^= 2;
    printf("fail_flipped %d\n", muse_restart_fail_reason(&m));
    muse_restart_mark_set(&m, MUSE_RESTART_CONSOLE, 0);
    printf("planned_as_fail %d\n", muse_restart_fail_reason(&m));
    for (int marked = 0; marked < 2; marked++) {
        printf("effective2_%d", marked);
        for (int r = 0; r <= 15; r++) {
            printf(" %d", muse_restart_effective_reason2(r, marked, MUSE_RST_PANIC));
        }
        printf("\n");
    }
    return 0;
}
"""


class MuseRestartCoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "h.c"
            src.write_text(HARNESS)
            exe = Path(d) / "h"
            subprocess.run([cc, "-std=c11", "-Wall", "-Werror", "-I", str(COMP / "include"), str(src),
                            str(COMP / "muse_restart_core.c"), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        cls.out = dict(line.split(" ", 1) for line in out.strip().splitlines())

    def test_marker_only_when_written(self) -> None:
        self.assertEqual(self.out["garbage_valid"], "0")
        self.assertEqual(self.out["zero_valid"], "0")
        self.assertEqual(self.out["set_valid"], "1 why 2 core 1")
        self.assertEqual(self.out["flipped_valid"], "0")
        self.assertEqual(self.out["cleared_valid"], "0")

    def test_reason_with_and_without_marker(self) -> None:
        plain = list(map(int, self.out["effective0"].split()))
        marked = list(map(int, self.out["effective1"].split()))
        self.assertEqual(plain, list(range(16)))         # no marker: the chip's reason, unchanged
        expect = list(range(16))
        expect[7] = 3                                     # RTC watchdog after a marked restart: software
        self.assertEqual(marked, expect)                  # panic (4), int/task watchdog (5, 6) never masked

    def test_failure_marker(self) -> None:
        self.assertEqual(self.out["fail_garbage"], "0")
        self.assertEqual(self.out["fail_panic"], "4")
        self.assertEqual(self.out["fail_as_planned"], "0")   # never taken for a planned restart
        self.assertEqual(self.out["fail_none"], "3")         # esp_restart(): no hint, software
        self.assertEqual(self.out["fail_flipped"], "0")
        self.assertEqual(self.out["planned_as_fail"], "0")
        # After a software or RTC watchdog reset, a failure marker's reason (a panic here);
        # every other chip reason stays; a planned marker wins.
        fail = list(map(int, self.out["effective2_0"].split()))
        expect = list(range(16))
        expect[3] = expect[7] = 4
        self.assertEqual(fail, expect)
        both = list(map(int, self.out["effective2_1"].split()))
        expect = list(range(16))
        expect[7] = 3
        self.assertEqual(both, expect)


if __name__ == "__main__":
    unittest.main()
