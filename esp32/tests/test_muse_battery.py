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
import re
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


def as_saved(p: dict) -> dict:
    """The live JSON as the saved record has it: without the retention-capable RAM."""
    heap = {k: v for k, v in p["heap"].items() if k not in ("retention_free", "retention_largest")}
    return {**p, "heap": heap}


def build_harness(out: Path, *defines: str) -> Path:
    """The harness, for a board with power management and profiling unless `defines` say
    otherwise (MUSE_TEST_NO_PM_PROFILING, MUSE_TEST_NO_PM: see battery_fakes/sdkconfig.h)."""
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    proc = subprocess.run(
        [
            *cc,
            "-include",
            str(ROOT / "tests" / "host_compat.h"),
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            *[f"-D{d}" for d in defines],
            "-I",
            str(ROOT / "tests" / "battery_fakes"),
            "-I",
            str(ROOT / "components" / "muse"),
            str(ROOT / "tests" / "muse_battery_harness.c"),   # includes muse_battery.c
            "-o",
            str(out),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )
    if proc.returncode:
        raise AssertionError(proc.stdout + proc.stderr)
    return out


class HarnessCase(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = build_harness(Path(cls.tmp.name) / "muse_battery_harness")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness_raw(self, *commands: str, binary: Path | None = None) -> list[str]:
        """The lines the commands print, as text; boot's own "@power.saved" in self.booted_with."""
        script = "".join(f"dump\n{c}end\n" if c.startswith("Time since") else c + "\n" for c in commands)
        proc = subprocess.run([str(binary or self.binary)], input=script, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        lines = proc.stdout.splitlines()
        # What boot prints unasked, for a console that takes no commands.
        tag = "@power.saved "
        self.booted_with = [json.loads(line[len(tag):]) for line in lines if line.startswith(tag)]
        return [line for line in lines if not line.startswith(tag)]

    def run_harness(self, *commands: str, binary: Path | None = None) -> list[dict]:
        return [json.loads(line) for line in self.run_harness_raw(*commands, binary=binary)]


class BatteryTest(HarnessCase):
    # On battery from 10 s to 110 s: the screen off from 20 s, resting from 30 s,
    # 60 s of light sleep in 400 wakes.
    MEASURE = (
        "t 10000000", START, "p 0 100 4190",
        "t 20000000", "s 1 0",
        "t 30000000", "s 1 1",
        *["w 150000"] * 400, "w 0",
        "t 110000000", END, "p 0 99 4100",
    )

    def measure(self, *after: str) -> list[dict]:
        return self.run_harness(*self.MEASURE, *after)

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
        self.assertEqual(saved, as_saved(stopped))
        self.assertEqual(self.booted_with, [as_saved(stopped)])
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
        self.assertEqual(kept, as_saved(stopped))
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

    def test_internal_ram(self) -> None:
        rtc = str(Path(self.tmp.name) / "rtc")
        # Free now, largest block and lowest since boot are live; resting_min is the lowest
        # free internal RAM seen resting this run (the headroom then: CPU retention memory,
        # where there is any, was taken at boot and is out of every figure).
        before, resting, awake, stopped = self.run_harness(
            "h 90000 50000 40000", "t 10000000", START, "p 0 100 4190", "j",
            "t 30000000", "h 70000 50000 30000", "s 1 1",           # on entry
            "t 40000000", "h 65000 45000 30000", "p 0 100 4180",    # a reading while resting
            "j", "h 60000 45000 31000", "s 0 0",
            "t 50000000", "h 52000 44000 31000", "p 0 100 4170",    # awake: not counted
            "j", "p 1 100 4200", "j", f"save {rtc}")
        ret = {"retention_free": 20000, "retention_largest": 12000, "cpu_pd": False}
        self.assertEqual(before["heap"], {"free": 90000, "largest": 40000, "min": 50000, "resting_min": None, **ret})
        self.assertEqual(resting["heap"], {"free": 65000, "largest": 30000, "min": 45000, "resting_min": 65000, **ret})
        self.assertEqual(awake["heap"], {"free": 52000, "largest": 31000, "min": 44000, "resting_min": 65000, **ret})
        self.assertEqual(stopped["heap"]["resting_min"], 65000)
        # Saved with the run, for a Watcher that resets as its port opens.
        saved, = self.run_harness(f"load {rtc}", "boot 11", "k")
        self.assertEqual(saved["heap"], {"free": 52000, "largest": 31000, "min": 44000, "resting_min": 65000,
                                         "cpu_pd": False})   # without the retention fields
        self.assertIn("Memory    internal RAM 50.8 KB free (largest block 30.3 KB), 43.0 KB lowest since boot, "
                      "63.5 KB lowest while resting", power.report(saved))
        # A new run starts over.
        again, = self.run_harness("t 10000000", START, "p 0 100 4190", "x", "j")
        self.assertIsNone(again["heap"]["resting_min"])
        self.assertIn("Memory    internal RAM 58.6 KB free", power.report({"started": False, "heap": again["heap"]}))

    def test_cpu_power_down(self) -> None:
        # Noted before init (muse_input_power_init runs first), with the retention-capable heap.
        j, = self.run_harness("c 1", "h 56000 46000 38000 9000 4000", "j")
        self.assertEqual(j["heap"]["cpu_pd"], True)
        self.assertEqual((j["heap"]["retention_free"], j["heap"]["retention_largest"]), (9000, 4000))
        text = power.report(j)
        self.assertIn("retention-capable RAM left 8.8 KB (largest block 3.9 KB); CPU powers down in light sleep", text)
        off, = self.run_harness("c 0", "j")
        self.assertIn("CPU stays powered in light sleep", power.report(off))

    def test_report_reads_the_json(self) -> None:
        j, = self.measure("j")
        text = power.report(j)
        self.assertIn("Battery   100% -> 99%   4.19 V -> 4.10 V   (too soon to tell", text)
        self.assertIn("in light sleep 60.0%, 400 wakes (4.0/s); a core busy 10.0%", text)
        self.assertIn("7 light sleeps refused", text)
        locks = [line.split()[0] for line in text.splitlines()[-7:]]
        self.assertEqual(locks, ["rtos0", "rtos1", "wifi", "usb_serial_jtag", "bt", "i2s_driver", "i2s_driver"])


    def test_cut_short_stays_valid(self) -> None:
        # Into any buffer too small for it, the JSON loses whole pieces from the end (the
        # lock list first) but stays valid: never a field cut in half or a bracket left open.
        full, = self.measure("j")
        caps = range(0, len(json.dumps(full, separators=(",", ":"))) + 2)
        outputs = self.run_harness_raw(*self.MEASURE, *[f"j {cap}" for cap in caps])
        for cap, text in zip(caps, outputs):
            with self.subTest(cap=cap):
                self.assertLess(len(text), max(cap, 1))
                if cap < 3:
                    self.assertEqual(text, "")   # no room for "{}"
                    continue
                cut = json.loads(text)
                for key, value in cut.items():
                    if key == "locks":
                        self.assertEqual(value, full["locks"][:len(value)])
                    else:
                        self.assertEqual(value, full[key], key)
        self.assertEqual(json.loads(outputs[-1]), full)
        # The head goes in before any lock, so only the lock list is short of room in the end.
        self.assertIn("heap", json.loads(outputs[len(caps) // 2]))

# The longest value each field of the JSON can take on the chip: unsigned and size_t are 32 bits
# there, and so is long (sleep_rejects). Uptime and secs are microseconds since boot / 10^6.
I32_MIN, I32_MAX, U32_MAX, I64_MAX = -2**31, 2**31 - 1, 2**32 - 1, 2**63 - 1
# Two dozen locks (MAX_LOCKS), names and types as long as lock_stat_t holds (dump() cuts names to 15).
WORST_LOCKS = [(f"lock{i:02d}_" + "n" * 15, "NO_LIGHT_SLEEPX", I32_MIN, 2 * 10**9) for i in range(24)]
VARIANTS = {   # harness defines; the boards built that way
    "PM profiling": (),                                  # every board with PM (Watcher, AIPI, ...)
    "PM, no profiling": ("MUSE_TEST_NO_PM_PROFILING",),  # none today, but allowed
    "no PM": ("MUSE_TEST_NO_PM",),                       # the other boards with the full UI
}


def saved_max(profiling: bool) -> int:
    """SAVED_MAX in muse_battery.c, with or without CONFIG_PM_PROFILING."""
    source = (ROOT / "components" / "muse" / "muse_battery.c").read_text()
    block = source[source.index("#if CONFIG_PM_PROFILING\n#define SAVED_MAX"):]
    values = [int(v) for v in re.findall(r"#define SAVED_MAX (\d+)", block[:block.index("#endif")])]
    return values[0] if profiling else values[1]


class RecordSizeTest(HarnessCase):
    """The record saved in RTC memory and flash (and printed as "@power.saved") at its longest,
    for every build and CPU power-down state, against its SAVED_MAX buffer."""

    binaries: dict[str, Path]

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.binaries = {name: build_harness(Path(cls.tmp.name) / f"harness{i}", *defines)
                        for i, (name, defines) in enumerate(VARIANTS.items())}

    def worst(self, variant: str, cpu_pd: bool) -> tuple[str, str]:
        """The saved record and the live JSON with every field at its longest."""
        callbacks = variant != "no PM"
        script = [
            "boot 8",                     # "deep sleep", the longest reset name; 65535 boots once counted
            "b 65534", f"c {int(cpu_pd)}", f"h {U32_MAX} {U32_MAX} {U32_MAX} {U32_MAX} {U32_MAX}",
            "t 0", dump(1, [], {"SLEEP": (40, 0), "APB_MIN": (40, 0), "APB_MAX": (80, 0), "CPU_MAX": (240, 0)}),
            f"p 0 {I32_MAX} {I32_MIN}",   # a negative percentage means no battery: it wouldn't start
            "s 1 1",                      # resting, so resting_min isn't null
            *([f"w {I64_MAX}", f"n {U32_MAX}"] if callbacks else []),
            f"t {I64_MAX}",
            dump(2 * 10**9 + 1, WORST_LOCKS, {m: (240, 2 * 10**9) for m in ("SLEEP", "APB_MIN", "APB_MAX", "CPU_MAX")},
                 rejects=I32_MIN),
            f"p 0 {I32_MAX} {I32_MIN}",   # saved: SAVE_MS since the last save
            "rtc", "j",
        ]
        saved, live = self.run_harness_raw(*script, binary=self.binaries[variant])
        return saved, live

    def test_worst_case_fits(self) -> None:
        for variant in VARIANTS:
            profiling = variant == "PM profiling"
            cap = saved_max(profiling)
            for cpu_pd in (False, True):
                with self.subTest(variant=variant, cpu_pd=cpu_pd):
                    saved, live = self.worst(variant, cpu_pd)
                    record, now = json.loads(saved), json.loads(live)
                    self.assertLess(len(saved), cap)
                    # Every field at its longest, so this is the worst case.
                    self.assertEqual((record["boot"], record["battery_boots"], record["secs"]),
                                     ("deep sleep", 65535, I64_MAX // 10**6))
                    self.assertEqual(record["battery_mv"], [I32_MIN, I32_MIN])
                    self.assertEqual(record["screen_off"], 100.0)
                    self.assertEqual(record["heap"], {"free": U32_MAX, "largest": U32_MAX, "min": U32_MAX,
                                                      "resting_min": U32_MAX, "cpu_pd": cpu_pd})
                    sleep = (None, 0) if variant == "no PM" else (100.0, U32_MAX)
                    self.assertEqual((record["slept"], record["sleeps"]), sleep)
                    # Live, the retention fields too.
                    self.assertEqual((now["heap"]["retention_free"], now["heap"]["retention_largest"]),
                                     (U32_MAX, U32_MAX))
                    if profiling:
                        # The head always fits; the lock list takes what's left, whole locks only.
                        self.assertEqual(record["modes"], {"SLEEP": 100.0, "APB_MIN": 100.0, "APB_MAX": 100.0,
                                                           "CPU_MAX": 100.0})
                        self.assertEqual(record["sleep_rejects"], I32_MIN)
                        self.assertGreaterEqual(len(record["locks"]), 18)   # SAVED_MAX's comment
                        self.assertEqual(record["locks"][0], {"name": "lock00_nnnnnnnn", "type": "NO_LIGHT_SLEEPX",
                                                              "held": 100.0, "taken": I32_MIN})
                        self.assertLessEqual(len(now["locks"]), len(record["locks"]))   # also 2048 B, with retention
                    else:
                        # The whole record, with room to spare.
                        self.assertEqual(json.dumps(record, separators=(",", ":")), saved)
                        self.assertLessEqual(len(saved), (cap - 1) * 9 // 10, f"{len(saved)} of {cap - 1} bytes")
                        self.assertNotIn("modes", record)
                        self.assertEqual(record["busy"], None)
                        self.assertEqual({**now, "heap": None}, {**record, "heap": None})

    def test_power_reads_the_worst_case(self) -> None:
        for variant in VARIANTS:
            saved, live = self.worst(variant, True)
            for p in (json.loads(saved), json.loads(live)):
                self.assertIn("CPU powers down in light sleep", power.report(p))

class ReportTest(unittest.TestCase):
    def test_drain(self) -> None:
        p = {"started": True, "running": False, "secs": 7200, "battery_pct": [100, 96], "battery_mv": [4190, 4050],
             "screen_off": 95.0, "resting": 94.0, "slept": 90.0, "sleeps": 7200, "busy": 1.0}
        text = power.report(p, mah=450)
        self.assertIn("Ran on battery for 2 h 0 min", text)
        self.assertIn("2.0%/h, about 50 h from full, 9 mA average", text)
        self.assertIn("in light sleep 90.0%, 7200 wakes (1.0/s); a core busy 1.0%", text)

    def test_saved_records_old_and_new(self) -> None:
        run = {"started": True, "running": False, "secs": 7200, "battery_pct": [100, 96],
               "battery_mv": [4190, 4050], "screen_off": 95.0, "resting": 94.0, "slept": 90.0, "sleeps": 7200,
               "busy": 1.0}
        heap = {"free": 52000, "largest": 31000, "min": 44000, "resting_min": 65000}
        # Firmware before "heap": no memory line.
        self.assertNotIn("Memory", power.report(run))
        # The first with it saved the retention fields too; now only cpu_pd.
        full = power.report({**run, "heap": {**heap, "retention_free": 9000, "retention_largest": 4000,
                                             "cpu_pd": True}})
        self.assertIn("retention-capable RAM left 8.8 KB (largest block 3.9 KB); CPU powers down", full)
        saved = power.report({**run, "heap": {**heap, "cpu_pd": True}})
        self.assertIn("63.5 KB lowest while resting\n          CPU powers down in light sleep", saved)
        self.assertNotIn("retention", saved)

    def test_not_started(self) -> None:
        self.assertIn("No measurement yet", power.report({"started": False}))


if __name__ == "__main__":
    unittest.main()
