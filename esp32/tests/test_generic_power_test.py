# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Run the real core and generic backend against simple host hardware fakes."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_power_test import arm
import test_power_test as core_tests
ROOT = Path(__file__).resolve().parents[1]
class GenericPowerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        # Every IDF header the backend includes is empty here; generic_power_test_stub.h supplies the API.
        for header in ("bsp/esp-bsp.h", "driver/uart.h", "esp_event.h", "esp_rom_sys.h", "esp_netif.h",
                       "esp_private/esp_clk.h", "esp_random.h", "esp_rtc_time.h", "esp_sleep.h", "esp_wifi.h",
                       "driver/gpio.h", "driver/i2c_master.h", "driver/usb_serial_jtag.h", "esp_codec_dev.h",
                       "freertos/task.h", "lvgl.h"):
            path = Path(cls.tmp.name) / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        cc = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
        includes = ["-I", cls.tmp.name, "-I", str(ROOT / "components/muse")]
        for directory in ("power_test_fakes", "battery_fakes"):
            includes += ["-I", str(ROOT / "tests" / directory)]
        includes += ["-include", str(ROOT / "tests/generic_power_test_stub.h")]
        harness = str(ROOT / "tests/generic_power_test_harness.c")
        core = str(Path(cls.tmp.name) / "core.o")
        subprocess.run(cc + includes + ["-DTEST_CORE", "-c", harness, "-o", core], check=True)
        subprocess.run(cc + includes + [harness, core, "-o", str(Path(cls.tmp.name) / "generic")], check=True)
        core_tests.PowerTest.setUpClass()
        cls.addClassCleanup(core_tests.PowerTest.tearDownClass)
    def run_commands(self, *commands, profile="175c", target="generic"):
        proc = subprocess.run([str(core_tests.PowerTest.binary if target == "watcher" else Path(self.tmp.name) / "generic"), profile], input="\n".join(commands) + "\n",
                              text=True, capture_output=True, timeout=3, check=True)
        self.assertEqual(proc.stderr, "")
        return [row for line in proc.stdout.splitlines() if line.startswith("PTEST ")
                if (row := json.loads(line[6:]))["type"] != "ready"]
    def test_charging_unsupported(self):
        rows = self.run_commands(arm(charging_off=1), ">ptest.status", profile="aipi")
        self.assertEqual(rows[0]["message"], "charging_off_not_supported")
        self.assertEqual(rows[1]["state"], "idle")
    def test_charging_lifecycle(self):
        for board in ("175c", "216"):
            for ending, state in (("advance 90000", "complete"), ("poll >ptest.abort", "aborted"),
                                  ("error", "error"), ("guard", "error")):
                with self.subTest(board=board, ending=ending):
                    rows = self.run_commands("charging 1", arm(usb_powered=1, charging_off=1),
                                             "charging 1", "advance 10", "charging 0", "advance 1490", "capture",
                                             "poll >ptest.status", ending, "charging 1", ">ptest.results", profile=board)
                    self.assertEqual((rows[1]["type"], rows[1]["state"]), ("status", "running"))
                    self.assertEqual(rows[-1]["state"], state)
                    if ending == "guard":
                        self.assertEqual(rows[-1]["error"], 263)  # ESP_ERR_TIMEOUT
    def test_charging_write_failures(self):
        for board in ("175c", "216"):
            with self.subTest(board=board, failure="disable"):
                # The disable write fails at run start: the run ends as error with charging on.
                rows = self.run_commands(arm(usb_powered=1, charging_off=1), "pmufail 1", "advance 1500",
                                         "charging 1", ">ptest.results", profile=board)
                self.assertEqual((rows[-1]["state"], rows[-1]["error"]), ("error", -1))  # ESP_FAIL
            with self.subTest(board=board, failure="restore"):
                # Every cleanup restore fails: the run cannot finish cleanly, the guard
                # retries until charging is back, and the next run is unaffected.
                rows = self.run_commands(arm(usb_powered=1, charging_off=1), "advance 1500", "capture",
                                         "pmufail 3", "error", "charging 0", ">ptest.results", "guard",
                                         "charging 1", arm(usb_powered=1), "advance 90000", ">ptest.results",
                                         profile=board)
                self.assertEqual((rows[1]["state"], rows[1]["restore_error"]), ("error", -1))
                self.assertEqual(rows[-1]["state"], "complete")
    def test_guard_expiry_does_not_poison_next_run(self):
        for board in ("175c", "216"):
            with self.subTest(board=board):
                rows = self.run_commands(arm(usb_powered=1, charging_off=1), "advance 1500",
                                         "guard", "charging 1", ">ptest.results", arm(usb_powered=1),
                                         "advance 90000", ">ptest.results", profile=board)
                self.assertEqual((rows[1]["state"], rows[1]["error"]), ("error", 263))
                self.assertEqual(rows[-1]["state"], "complete")
    def test_usb_sleep_rejected_at_arm(self):
        rows = self.run_commands(arm(matrix="sleep", usb_powered=1), ">ptest.status", target="watcher")
        self.assertEqual(rows[0]["message"], "usb_powered_requires_peripheral_matrix")
        self.assertEqual(rows[1]["state"], "idle")
    def test_usb_run_and_capture(self):
        # I2C fakes assert at every access; status/results also run inside capture.
        for board in ("175c", "216", "aipi"):
            for ending, state in (("advance 90000", "complete"), ("poll >ptest.abort", "aborted")):
                with self.subTest(board=board, ending=ending):
                    rows = self.run_commands(arm(usb_powered=1), "advance 1500", "capture",
                                             "poll >ptest.status", ">ptest.results", ending, ">ptest.results", profile=board)
                    self.assertEqual((rows[1]["type"], rows[1]["state"], rows[1]["usb"]), ("status", "running", True))
                    self.assertEqual(rows[-1]["state"], state)
                    self.assertTrue(any(r["measure_start_us"] for r in rows[-1]["records"]))
