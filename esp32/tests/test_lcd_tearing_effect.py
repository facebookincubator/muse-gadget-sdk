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

"""Compile muse_lcd_bands.c's tearing-effect timing (te_meets) on the host and
check it against the CO5300's measured scan: 16.8 ms refresh, 0.6 ms blanking,
466 rows."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/boards/muse_lcd_bands.c"


def function(source, name):
    match = re.search(r"^static bool " + name + r"\([^)]*\)\n\{", source, re.M)
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int s_rows = 466;
%s
int main(int argc, char **argv)
{
    /* t0 w y1 y2 fall period scan, in microseconds and rows */
    long long a[7];
    for (int i = 0; i < 7; i++) a[i] = atoll(argv[i + 1]);
    printf("%%d\n", te_meets(a[0], a[1], (int)a[2], (int)a[3], a[4], a[5], a[6]));
    return 0;
}
'''

PERIOD, SCAN = 16800, 16200   # the CO5300 on the Waveshare 1.75C, as logged


class TearingEffectTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "te.c"
        src.write_text(HARNESS % function(SOURCE.read_text(), "te_meets"))
        cls.exe = Path(cls.tmp.name) / "te"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-o", str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def meets(self, t0, w, y1, y2, fall=100000):
        out = subprocess.run([str(self.exe)] + [str(v) for v in (fall + t0, w, y1, y2, fall, PERIOD, SCAN)],
                             check=True, capture_output=True, text=True).stdout
        return out.strip() == "1"

    def test_whole_screen_slower_than_the_scan(self):
        # A 26.4 ms write stays behind the scan that just started and ahead of the
        # next one only if it starts within about 6 ms of the scan's start.
        for t in (0, 2000, 5000):
            self.assertFalse(self.meets(t, 26400, 0, 466), t)
        for t in (7000, 10000, 16000):
            self.assertTrue(self.meets(t, 26400, 0, 466), t)

    def test_whole_screen_faster_than_the_scan(self):
        # A 5 ms write overtakes a scan it starts behind, so it has to start late
        # in the scan, below which nothing is left for it to catch.
        self.assertTrue(self.meets(1000, 5000, 0, 466))
        self.assertFalse(self.meets(13000, 5000, 0, 466))

    def test_small_band(self):
        # Rows 100-200 are scanned 3.5-7 ms into the refresh: a 2 ms write meets
        # the scan only around then.
        self.assertFalse(self.meets(0, 2000, 100, 200))
        self.assertTrue(self.meets(4000, 2000, 100, 200))
        self.assertFalse(self.meets(9000, 2000, 100, 200))

    def test_scans_after_the_last_one(self):
        # The next refresh's scan counts too: the same band, one period later.
        self.assertTrue(self.meets(PERIOD + 4000, 2000, 100, 200))


if __name__ == "__main__":
    unittest.main()
