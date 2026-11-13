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

"""Host test for the WS1001 station GPS parser (components/muse/wg1200_gps.h).

The swipe-down forecast card names the station's location from the RMC
sentence the outdoor station sends in its WEATHER_GPS_SIGNED packets.
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

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "wg1200_gps.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        double lat = 0, lon = 0;
        if (wg1200_gps_parse_rmc(argv[i], strlen(argv[i]), &lat, &lon)) {
            printf("%.5f %.5f\n", lat, lon);
        } else {
            printf("none\n");
        }
    }
    return 0;
}
"""


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise RuntimeError("C compiler not available; set CC")
    return cmd


class Wg1200GpsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "gps_harness.c"
        src.write_text(HARNESS)
        cls.binary = Path(cls.tmp.name) / "gps_harness"
        subprocess.run([
            *_cc_command(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "components/muse"), str(src), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def parse(self, *sentences: str) -> list[str]:
        out = subprocess.run([str(self.binary), *sentences], check=True,
                             capture_output=True, text=True).stdout
        return out.split("\n")[:-1]

    def test_valid_fix_from_a_ws1001_packet(self) -> None:
        # The sample in WeatherXM's ws1001.cpp decoder.
        [line] = self.parse("GNRMC,101554.000,A,3758.84244,N,02342.65014,E,0.00,0.00,140922,,,A")
        lat, lon = map(float, line.split())
        self.assertAlmostEqual(lat, 37 + 58.84244 / 60, places=5)
        self.assertAlmostEqual(lon, 23 + 42.65014 / 60, places=5)

    def test_southern_and_western_hemispheres(self) -> None:
        [line] = self.parse("GPRMC,120000.000,A,3352.00000,S,15112.00000,W,0,0,010126,,,A")
        lat, lon = map(float, line.split())
        self.assertLess(lat, 0)
        self.assertLess(lon, 0)

    def test_rejects_no_fix_and_junk(self) -> None:
        self.assertEqual(self.parse(
            "GNRMC,101554.000,V,3758.84244,N,02342.65014,E,0.00,0.00,140922,,,N",  # no fix
            "GNRMC,101554.000,A,,N,,E,0.00,0.00,140922,,,A",                      # empty position
            "GNRMC,101554.000,A,3799.00000,N,02342.65014,E,0,0,140922,,,A",       # 99 minutes
            "GNRMC,101554.000,A,3758.84244,X,02342.65014,E,0,0,140922,,,A",       # bad hemisphere
            "GNGGA,101554.000,3758.84244,N,02342.65014,E,1,08,1.0,100,M",          # not RMC
            "GNRMC,101554.000,A",                                                 # truncated
            "",
        ), ["none"] * 7)

    def test_accepts_a_leading_dollar(self) -> None:
        [line] = self.parse("$GNRMC,101554.000,A,3758.84244,N,02342.65014,E,0.00,0.00,140922,,,A")
        self.assertNotEqual(line, "none")


if __name__ == "__main__":
    unittest.main()
