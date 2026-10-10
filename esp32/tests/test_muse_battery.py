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

"""The battery meter (muse_battery.c) against esp_pm_dump_locks() text in IDF's own
formats, and the report tools/muse/power.py makes of its "@power" JSON."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "muse"))

import power  # noqa: E402

S = 1_000_000


def dump(now: int, locks: list[tuple], modes: dict[str, tuple[int, int]], rejects: int = 0,
         light_sleep: bool = True) -> str:
    """What esp_pm_dump_locks() prints (esp_pm/pm_locks.c, pm_impl.c in IDF 6.0)."""
    d100 = now // 100
    out = f"Time since boot up: {now} us\nLock stats:\n"
    out += "%-15s %-14s  %-5s  %-8s  %-13s  %-14s  %-8s\n" % (
        "Name", "Type", "Arg", "Active", "Total_count", "Time(us)", "Time(%)")
    for name, kind, taken, held in locks:
        out += "%-15.15s " % name + "%-14s  %-5d  %-8d  %-13d  %-14d  %-3d%%\n" % (
            kind, 0, 1, taken, held, (held + d100 - 1) // d100)
    out += "\nMode stats:\n" + "%-8s  %-10s  %-10s  %-10s\n" % ("Mode", "CPU_freq", "Time(us)", "Time(%)")
    for name, (mhz, us) in modes.items():
        if name != "SLEEP" or light_sleep:
            out += "%-8s  %-3dM%-7s %-10d  %-2d%%\n" % (name, mhz, "", us, us * 100 // now)
    if light_sleep:
        out += f"\nSleep stats:\nlight_sleep_counts:500  light_sleep_reject_counts:{rejects}\n"
    return out


START = dump(10 * S, [
    ("rtos0", "CPU_FREQ_MAX", 5000, 9 * S),
    ("rtos1", "CPU_FREQ_MAX", 4000, 6 * S),
    ("usb_serial_jtag", "NO_LIGHT_SLEEP", 1, S // 2),   # 5%: printed "5  %"
    ("wifi", "APB_FREQ_MAX", 10, 9_900_000),
    ("i2s_driver", "APB_FREQ_MAX", 2, 9 * S),
    ("i2s_driver", "APB_FREQ_MAX", 2, 8 * S),
    ("lock@0x3fc9a000", "NO_LIGHT_SLEEP", 0, 0),
], {"SLEEP": (40, 0), "APB_MIN": (40, S), "APB_MAX": (80, 0), "CPU_MAX": (240, 9 * S)})

# 100 s later, with a lock made since (IDF puts new ones first).
END = dump(110 * S, [
    ("bt", "NO_LIGHT_SLEEP", 3, 1_234_000),
    ("rtos0", "CPU_FREQ_MAX", 15000, 29 * S),
    ("rtos1", "CPU_FREQ_MAX", 9000, 16 * S),
    ("usb_serial_jtag", "NO_LIGHT_SLEEP", 2, 3_500_000),
    ("wifi", "APB_FREQ_MAX", 100, 14_900_000),
    ("i2s_driver", "APB_FREQ_MAX", 3, 9_500_000),
    ("i2s_driver", "APB_FREQ_MAX", 3, 8_250_000),
    ("lock@0x3fc9a000", "NO_LIGHT_SLEEP", 0, 0),
], {"SLEEP": (40, 60 * S), "APB_MIN": (40, 26 * S), "APB_MAX": (80, 5 * S), "CPU_MAX": (240, 19 * S)},
    rejects=7)


class BatteryTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_battery_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "tests" / "battery_fakes"),
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_battery_harness.c"),   # includes muse_battery.c
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, *commands: str) -> list[dict]:
        script = "".join(f"dump\n{c}end\n" if c.startswith("Time since") else c + "\n" for c in commands)
        proc = subprocess.run([str(self.binary)], input=script, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        lines = proc.stdout.splitlines()
        # What boot prints unasked, for a console that takes no commands.
        tag = "@power.saved "
        self.booted_with = [json.loads(line[len(tag):]) for line in lines if line.startswith(tag)]
        return [json.loads(line) for line in lines if not line.startswith(tag)]

    def measure(self, *after: str) -> list[dict]:
        """On battery from 10 s to 110 s: the screen off from 20 s, resting from 30 s,
        60 s of light sleep in 400 wakes."""
        return self.run_harness(
            "t 10000000", START, "p 0 100 4190",
            "t 20000000", "s 1 0",
            "t 30000000", "s 1 1",
            *["w 150000"] * 400, "w 0",
            "t 110000000", END, "p 0 99 4100",
            *after,
        )

    def test_waits_for_battery(self) -> None:
        read, j = self.run_harness("t 5000000", START, "p 1 100 4200", "r", "j")
        self.assertEqual(read["started"], 0)
        self.assertEqual(j["started"], False)
        self.assertNotIn("modes", j)
        read, = self.run_harness("p 0 -1 0", "r")   # no battery connected
        self.assertEqual(read["started"], 0)

    def test_measurement(self) -> None:
        read, j = self.measure("r", "j")
        self.assertEqual(j["started"], True)
        self.assertEqual(j["running"], True)
        self.assertEqual(j["secs"], 100)
        self.assertEqual(j["battery_pct"], [100, 99])
        self.assertEqual(j["battery_mv"], [4190, 4100])
        self.assertEqual((j["screen_off"], j["resting"]), (90.0, 80.0))
        self.assertEqual((j["slept"], j["sleeps"]), (60.0, 400))
        self.assertEqual(j["busy"], 10.0)
        self.assertEqual(j["modes"], {"SLEEP": 60.0, "APB_MIN": 25.0, "APB_MAX": 5.0, "CPU_MAX": 10.0})
        self.assertEqual(j["sleep_rejects"], 7)
        self.assertEqual([(l["name"], l["type"], l["held"], l["taken"]) for l in j["locks"]], [
            ("bt", "NO_LIGHT_SLEEP", 1.2, 3),
            ("rtos0", "CPU_FREQ_MAX", 20.0, 10000),
            ("rtos1", "CPU_FREQ_MAX", 10.0, 5000),
            ("usb_serial_jtag", "NO_LIGHT_SLEEP", 3.0, 1),
            ("wifi", "APB_FREQ_MAX", 5.0, 90),
            ("i2s_driver", "APB_FREQ_MAX", 0.5, 1),
            ("i2s_driver", "APB_FREQ_MAX", 0.2, 1),
            ("lock@0x3fc9a000", "NO_LIGHT_SLEEP", 0.0, 0),
        ])
        self.assertEqual(read["awake"], "wifi 5.0%, usb_serial_jtag 3.0%")
        self.assertEqual((read["rate10"], read["full_h"]), (-1, -1))   # under 10 minutes
        self.assertEqual((read["slept_pm"], read["busy_pm"]), (600, 100))

    def test_usb_ends_it(self) -> None:
        later = dump(500 * S, [("rtos0", "CPU_FREQ_MAX", 1, 400 * S)], {"CPU_MAX": (240, 400 * S)})
        stopped, kept = self.measure("p 1 99 4150", "j", "t 500000000", later, "s 0 0", "w 5000000",
                                     "p 1 100 4200", "j")
        self.assertEqual(stopped["running"], False)
        self.assertEqual(stopped["battery_mv"], [4190, 4100])
        self.assertEqual((kept["uptime"], stopped["uptime"]), (500, 110))
        self.assertEqual({**kept, "running": False, "uptime": 0}, {**stopped, "uptime": 0})

    def test_reset(self) -> None:
        running, stopped = self.measure("t 120000000", "x", "t 130000000", "j", "p 1 99 4150", "x", "j")
        self.assertEqual(running["secs"], 10)
        self.assertEqual(running["battery_pct"], [99, 99])
        self.assertEqual(running["screen_off"], 100.0)
        self.assertEqual(stopped["started"], False)

    def test_drain(self) -> None:
        # 100% to 96% in 2 h: 2.0%/h, 50 h from full. Up isn't a drain.
        drained, charged = self.run_harness("t 0", "p 0 100 4190", "t 7200000000", "p 0 96 4050", "r",
                                            "p 0 101 4200", "r")
        self.assertEqual((drained["rate10"], drained["full_h"]), (20, 50))
        self.assertEqual((charged["rate10"], charged["full_h"]), (-1, -1))
        early, = self.run_harness("t 0", "p 0 100 4190", "t 599000000", "p 0 90 4000", "r")
        self.assertEqual(early["rate10"], -1)

    def test_new_lock_under_an_old_name(self) -> None:
        start = dump(10 * S, [("spi_master", "APB_FREQ_MAX", 50, 5 * S)], {"CPU_MAX": (240, 10 * S)})
        end = dump(20 * S, [("spi_master", "APB_FREQ_MAX", 4, S)], {"CPU_MAX": (240, 20 * S)})
        j, = self.run_harness("t 10000000", start, "p 0 50 3900", "t 20000000", end, "j")
        self.assertEqual(j["locks"], [{"name": "spi_master", "type": "APB_FREQ_MAX", "held": 10.0, "taken": 4}])

    def test_without_light_sleep(self) -> None:
        start = dump(10 * S, [], {"SLEEP": (40, 0), "CPU_MAX": (160, 10 * S)}, light_sleep=False)
        end = dump(20 * S, [], {"SLEEP": (40, 0), "CPU_MAX": (160, 20 * S)}, light_sleep=False)
        j, = self.run_harness("t 10000000", start, "p 0 50 3900", "t 20000000", end, "j")
        self.assertEqual(j["modes"], {"SLEEP": 0.0, "APB_MIN": 0.0, "APB_MAX": 0.0, "CPU_MAX": 100.0})
        self.assertEqual((j["slept"], j["sleeps"], j["sleep_rejects"], j["locks"]), (0.0, 0, 0, []))

    def test_kept_across_a_reset(self) -> None:
        rtc = str(Path(self.tmp.name) / "rtc")
        # Unplugged, then USB back: the board saves the finished measurement, then resets.
        stopped, none = self.measure("p 1 99 4150", "j", "k", f"save {rtc}")
        self.assertEqual(none, None)
        self.assertEqual((stopped["boot"], stopped["running"]), ("power on", False))
        # USB_UART_CHIP_RESET as the port opens: the next boot has it, and nothing of its own.
        saved, now = self.run_harness(f"load {rtc}", "boot 11", "t 2000000", "p 1 100 4200", "k", "j")
        self.assertEqual(saved, stopped)
        self.assertEqual(self.booted_with, [stopped])
        self.assertEqual((now["boot"], now["uptime"], now["started"]), ("usb", 2, False))
        # Until power.reset.
        cleared, = self.run_harness(f"load {rtc}", "x", "k")
        self.assertEqual(cleared, None)

    def test_saved_on_battery(self) -> None:
        rtc = str(Path(self.tmp.name) / "rtc")
        # On battery at 10 s; the saves follow battery readings, at most every 30 min.
        self.run_harness("t 10000000", START, "p 0 100 4190", "t 1000000000", "p 0 99 4150", f"save {rtc}")
        early, = self.run_harness(f"load {rtc}", "boot 4", "k")
        self.assertEqual(early["secs"], 0)
        self.run_harness("t 10000000", START, "p 0 100 4190", "t 1810000000", END, "p 0 98 4100", f"save {rtc}")
        late, = self.run_harness(f"load {rtc}", "boot 4", "k")
        self.assertEqual((late["secs"], late["battery_pct"], late["running"]), (1800, [100, 98], True))
        # A panic on battery, then a new measurement from boot.
        now, = self.run_harness(f"load {rtc}", "boot 4", "t 3000000", START, "p 0 97 4080", "j")
        self.assertEqual((now["boot"], now["started"], now["secs"]), ("panic", True, 0))
        text = power.report({**now, "secs": 7200, "uptime": 7203})
        self.assertIn("The board restarted (panic) on battery", text)
        # A corrupt record is ignored.
        with open(rtc, "r+b") as f:
            f.seek(20)
            f.write(b"#")
        bad, = self.run_harness(f"load {rtc}", "k")
        self.assertEqual(bad, None)

    def test_kept_in_flash(self) -> None:
        flash = str(Path(self.tmp.name) / "flash")
        Path(flash).unlink(missing_ok=True)
        # Not the start: each boot on battery would replace the run with an empty one.
        self.run_harness(f"flash {flash}", "t 10000000", START, "p 0 100 4190")
        none, = self.run_harness(f"flash {flash}", "k")
        self.assertEqual(none, None)
        # A periodic save, then the battery runs flat: the RTC memory is gone, the flash isn't.
        self.run_harness(f"flash {flash}", "t 10000000", START, "p 0 100 4190", "t 1810000000", END,
                         "p 0 98 4100")
        saved, = self.run_harness(f"flash {flash}", "t 2000000", "p 1 100 4200", "k")
        self.assertEqual((saved["secs"], saved["battery_pct"], saved["running"]), (1800, [100, 98], True))
        self.assertEqual(self.booted_with, [saved])
        # USB back saves it too, and power.reset clears it.
        stopped, = self.run_harness(f"flash {flash}", "t 10000000", START, "p 0 100 4190", "t 110000000", END,
                                    "p 0 99 4100", "p 1 99 4150", "j")
        kept, = self.run_harness(f"flash {flash}", "k")
        self.assertEqual(kept, stopped)
        self.run_harness(f"flash {flash}", "x")
        cleared, = self.run_harness(f"flash {flash}", "k")
        self.assertEqual(cleared, None)

    def test_boots_on_battery(self) -> None:
        flash = str(Path(self.tmp.name) / "flash")
        Path(flash).unlink(missing_ok=True)
        # Counted in flash, until a boot that sees USB power.
        on_battery = ("t 3000000", START, "p 0 50 3900", "j")
        first, = self.run_harness(f"flash {flash}", *on_battery)
        brownout, = self.run_harness(f"flash {flash}", "boot 9", *on_battery)
        unplugged, = self.run_harness(f"flash {flash}", "p 1 50 4000", *on_battery)
        self.assertEqual([j["battery_boots"] for j in (first, brownout, unplugged)], [1, 2, 0])
        text = power.report({**brownout, "secs": 600, "uptime": 603})
        self.assertIn("The board restarted (brownout) on battery", text)
        self.assertIn("Booted 2 times on battery since USB power was last seen.", text)
        self.assertNotIn("Booted", power.report({**first, "secs": 600, "uptime": 603}))

    def test_boots_not_counted_by_reset(self) -> None:
        flash = str(Path(self.tmp.name) / "flash")
        Path(flash).unlink(missing_ok=True)
        # power.reset restarts the run, but it isn't a boot.
        reset, = self.run_harness(f"flash {flash}", "t 3000000", START, "p 0 50 3900", "x", "x", "j")
        self.assertEqual(reset["battery_boots"], 1)
        again, = self.run_harness(f"flash {flash}", "t 3000000", START, "p 0 50 3900", "j")
        self.assertEqual(again["battery_boots"], 2)

    def test_boots_cleared_when_usb_is_seen(self) -> None:
        flash = str(Path(self.tmp.name) / "flash")
        Path(flash).unlink(missing_ok=True)
        on_battery = ("t 3000000", START, "p 0 50 3900", "j")
        self.run_harness(f"flash {flash}", *on_battery)
        self.run_harness(f"flash {flash}", *on_battery)
        # Charged on USB and switched off without unplugging: no run starts, but the count clears.
        charged, = self.run_harness(f"flash {flash}", "p 1 80 4100", "j")
        self.assertEqual(charged["battery_boots"], 0)
        woke, = self.run_harness(f"flash {flash}", *on_battery)
        self.assertEqual(woke["battery_boots"], 1)

    def test_report_reads_the_json(self) -> None:
        j, = self.measure("j")
        text = power.report(j)
        self.assertIn("Battery   100% -> 99%   4.19 V -> 4.10 V   (too soon to tell", text)
        self.assertIn("in light sleep 60.0%, 400 wakes (4.0/s); a core busy 10.0%", text)
        self.assertIn("7 light sleeps refused", text)
        locks = [line.split()[0] for line in text.splitlines()[-7:]]
        self.assertEqual(locks, ["rtos0", "rtos1", "wifi", "usb_serial_jtag", "bt", "i2s_driver", "i2s_driver"])


class ReportTest(unittest.TestCase):
    def test_drain(self) -> None:
        p = {"started": True, "running": False, "secs": 7200, "battery_pct": [100, 96], "battery_mv": [4190, 4050],
             "screen_off": 95.0, "resting": 94.0, "slept": 90.0, "sleeps": 7200, "busy": 1.0}
        text = power.report(p, mah=450)
        self.assertIn("Ran on battery for 2 h 0 min", text)
        self.assertIn("2.0%/h, about 50 h from full, 9 mA average", text)
        self.assertIn("in light sleep 90.0%, 7200 wakes (1.0/s); a core busy 1.0%", text)

    def test_not_started(self) -> None:
        self.assertIn("No measurement yet", power.report({"started": False}))


if __name__ == "__main__":
    unittest.main()
