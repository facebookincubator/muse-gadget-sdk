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

"""boards/muse_lcd_bands.c never hangs a task on a lost piece (seen on the W175 and
Watcher: LVGL spun in wait_for_flushing for a band whose last piece
never came back, and the voice task and the musegadgets worker on its core
starved until the task watchdog fired).

lcd_bands_harness.c compiles the real file against single-threaded fakes
(lcd_bands_fakes/): a wait that would block lets the fake wire finish its
pieces first, a timed one then lets its time pass and fails, and an
unbounded one aborts the harness ("HANG"), since nothing would ever end it.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


def fields(line):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}


class LcdBandsRecoveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "lcd_bands"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Werror=vla", "-Wno-unused-function",
                        "-Wno-unused-parameter", "-I", str(TESTS / "lcd_bands_fakes"), "-o", str(cls.exe),
                        str(TESTS / "lcd_bands_harness.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        r = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=30)
        self.assertNotIn("HANG", r.stdout, r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out, log = r.stdout.split("--log--\n")
        lines = {l.split()[0].split("=")[0]: fields(l) for l in out.splitlines() if l.strip()}
        return lines, log

    def test_bands_flush_and_lvgl_waits_on_a_semaphore(self):
        lines, log = self.run_case("normal")
        self.assertEqual(lines["band"]["wait"], 0)
        self.assertEqual(lines["band"]["flush_ready"], 3)   # one per band, from the last piece's interrupt
        self.assertEqual(lines["band"]["free"], 2)
        self.assertEqual(lines["band"]["async"], 0)
        self.assertEqual(log, "")

    def test_a_lost_piece_gives_the_band_up_and_redraws(self):
        lines, log = self.run_case("lost")
        lost = lines["lost"]
        self.assertEqual(lost["lost_pieces"], 1)
        self.assertEqual(lost["flush_ready"], 1)     # LVGL isn't left flushing
        self.assertEqual(lost["free"], 1)            # the lost piece's buffer stays owed: its done may still come
        self.assertEqual(lost["out"], 0)             # nothing counted in flight
        self.assertEqual(lost["async"], 1)           # the whole screen drawn again
        self.assertEqual(lost["invalidated"], 3)
        self.assertIn("a piece's done never came back", log)
        self.assertEqual(lines["next"]["flush_ready"], 2)   # the next band writes it off and goes out
        self.assertEqual(lines["next"]["lost_pieces"], 1)
        self.assertEqual(lines["next"]["free"], 2)
        self.assertIn("written off", log)

    def test_a_late_done_is_harmless(self):
        lines, _ = self.run_case("late")
        # The lost piece's done arrives inside piece_lost's settle window: it
        # ends no band and takes nothing off the count it is clearing.
        self.assertEqual(lines["settle"]["delivered"], 1)
        self.assertEqual(lines["settle"]["ended"], 0)
        self.assertEqual(lines["settle"]["out"], 3)  # the 3 in flight piece_lost gives up
        self.assertEqual(lines["late"]["free"], 2)   # never more than two
        self.assertEqual(lines["late"]["flush_ready"], 1)
        self.assertEqual(lines["next"]["flush_ready"], 2)
        self.assertEqual(lines["next"]["lost_flushes"], 0)

    def test_a_stuck_send_task_never_holds_lvgl(self):
        lines, log = self.run_case("stuck")
        self.assertEqual(lines["stuck"]["wait"], 1000)   # FLUSH_WAIT_MS, then LVGL goes on
        self.assertEqual(lines["stuck"]["lost_flushes"], 1)
        second = lines["second"]
        self.assertEqual(second["second"], 0)            # room for one more band
        self.assertEqual(second["third"], -1)            # then the queue is full: the flush fails at once
        self.assertEqual(second["wait3"], 0)
        self.assertIn("a band's flush never finished in 1000 ms: the send task is idle", log)
        self.assertIn("LVGL at priority 5", log)

    def test_a_lost_piece_in_a_frame_returns(self):
        lines, log = self.run_case("frame")
        self.assertEqual(lines["frame"]["free"], 1)          # the lost piece's buffer owed
        self.assertEqual(lines["frame"]["flush_ready"], 0)   # no LVGL band was out: none is ended for it
        self.assertEqual(lines["frame2"]["free"], 2)
        self.assertEqual(lines["frame2"]["lost_pieces"], 1)
        self.assertIn("a piece's done never came back", log)


if __name__ == "__main__":
    unittest.main()
