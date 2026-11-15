# SPDX-License-Identifier: Apache-2.0
"""BQ27220 samples must never confuse failed reads, absence and zero charge."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Waveshare185bPower(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "185b_power"
        built = subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "tests/pmu_fakes"), "-I", str(ROOT / "components/muse"),
            str(ROOT / "tests/waveshare_185b_power_harness.c"), "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stderr)

    def samples(self, *samples):
        defaults = dict(status=8, operation=32, mv=3876, soc=57, current=0,
                        fail=-1, usb=1, attached=1)
        lines = []
        for sample in samples:
            values = defaults | sample
            values["current"] &= 0xffff
            lines.append(" ".join(str(values[key]) for key in defaults))
        proc = subprocess.run([str(self.binary)], input="\n".join(lines) + "\n",
                              check=True, capture_output=True, text=True)
        return [tuple(map(int, line.split())) for line in proc.stdout.splitlines()]

    def test_percent_and_little_endian_millivolts(self):
        self.assertEqual(self.samples({}), [(0, 57, 3876, 0, 1, 5)])

    def test_current_direction_and_relaxation(self):
        rows = self.samples(dict(current=123), dict(current=-123, usb=0),
                            dict(current=0), dict(current=123, status=9))
        self.assertEqual([row[3] for row in rows], [1, 0, 0, 0])
        self.assertEqual(rows[1][4], 0)

    def test_zero_and_full_are_valid_percentages(self):
        rows = self.samples(dict(soc=0, mv=3000), dict(soc=100, mv=4200, status=0x208))
        self.assertEqual([row[1] for row in rows], [0, 100])
        self.assertEqual([row[3] for row in rows], [0, 0])

    def test_absence_does_not_publish_gauge_voltage(self):
        self.assertEqual(self.samples(dict(status=0)), [(0, -1, 0, 0, 1, 1)])

    def test_uninitialized_or_config_mode_is_unknown(self):
        for operation in (0, 0x400, 0x420):
            with self.subTest(operation=operation):
                row = self.samples(dict(operation=operation))[0]
                self.assertEqual(row[:5], (0x103, -1, 0, 0, 1))

    def test_every_i2c_failure_clears_previous_sample_and_recovers(self):
        for reg in (0x0a, 0x3a, 0x08, 0x2c, 0x0c):
            with self.subTest(register=reg):
                rows = self.samples(dict(current=250), dict(fail=reg, usb=0), dict(soc=56))
                self.assertEqual(rows[1][:5], (0x107, -1, 0, 0, 0))
                self.assertEqual(rows[2], (0, 56, 3876, 0, 1, 5))

    def test_invalid_samples_are_unknown_instead_of_clamped(self):
        for values in (dict(soc=101), dict(soc=65535), dict(mv=0), dict(mv=1999), dict(mv=5001)):
            with self.subTest(values=values):
                self.assertEqual(self.samples(values)[0][:5], (0x108, -1, 0, 0, 1))

    def test_missing_handle_still_updates_usb(self):
        self.assertEqual(self.samples(dict(attached=0, usb=0)), [(0x103, -1, 0, 0, 0, 0)])


if __name__ == "__main__":
    unittest.main()
