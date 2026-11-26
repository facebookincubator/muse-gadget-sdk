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

"""Developer > Tests > Vibration and the console's vib words on the
production haptics engine: the test logic (haptics/mg_haptic_test.c) over the
vendored core, unchanged, under ASan/UBSan (tests/mg_haptic_test_harness.c)."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLE = ROOT / "components" / "muse_gadget_ble"
CORE = BLE / "haptics"
PROTOCOLS = ROOT.parent / "protocols"


class Engine(unittest.TestCase):
    exe = None
    tmp = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.tmp.name, "mg_haptic_test")
        cc = os.environ.get("CC", "cc")
        san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined",
               "-fno-sanitize=object-size"]  # GCC flags (c ? A : B)[i] on two arrays as an overrun
        subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *san,
                        "-DMGH_MGCOMMANDS_H=\"mgcommands.h\"", f"-I{CORE}", f"-I{BLE / 'include'}", f"-I{PROTOCOLS}",
                        str(ROOT / "tests" / "mg_haptic_test_harness.c"), str(CORE / "mg_haptics.c"),
                        str(CORE / "mg_haptic_test.c"), "-o", cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, name):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        r = subprocess.run([self.exe, name], capture_output=True, text=True, timeout=120, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"{name} ok", r.stdout)
        return r.stdout

    def test_ready_at_rest(self):
        self.scenario("ready_at_rest")

    def test_refusals_show_their_reason(self):
        self.scenario("refusals")   # update, battery, asleep, turn; haptics_enabled skipped

    def test_a_playing_test_is_cut(self):
        self.scenario("cut_while_playing")

    def test_duty_budgets_hold_and_count(self):
        out = self.scenario("duty")
        self.assertRegex(out, r"hammer: \d+ played, \d+ refused")

    def test_caps(self):
        self.scenario("caps")

    def test_console_words_map_to_the_same_actions(self):
        self.scenario("console_words")


if __name__ == "__main__":
    unittest.main()
