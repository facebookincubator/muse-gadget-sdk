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

"""Exercise the 1.32 build/port entry points without IDF or serial access."""
import csv
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("muse_132_ports", ROOT / "tools/muse/ports.py")
ports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ports)


class Waveshare132BuildEntry(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="muse-132-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        script = self.root / "tools/muse/board.sh"
        script.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "tools/muse/board.sh", script)
        self.script = script
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        fake_idf = fake_bin / "idf.py"
        fake_idf.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@" > "$MUSE_TEST_ARGS"\n'
            'exit "${MUSE_TEST_BUILD_RC:-0}"\n'
        )
        fake_idf.chmod(0o755)
        self.args_path = self.root / "idf-args"
        self.env = {k: v for k, v in os.environ.items()
                    if k not in ("IDF_EXPORT", "MUSE_VIDEO", "MUSE_BENCH")}
        self.env.update(PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                        MUSE_TEST_ARGS=str(self.args_path))
        # Do not use board.sh's shared /tmp log while another real build runs.
        script.write_text(script.read_text().replace(
            "log=/tmp/muse_build_$board.log", 'log="$MUSE_TEST_ARGS.log"'))

    def run_board(self, action, **env):
        return subprocess.run(["bash", str(self.script), action, "s3-132"],
                              env={**self.env, **env}, text=True, capture_output=True)

    def test_build_selects_s3_and_own_eight_mb_overlay(self):
        result = self.run_board("build")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        args = self.args_path.read_text().splitlines()
        self.assertEqual(args[-1], "build")
        self.assertEqual(args[args.index("-B") + 1], "build-muse-waveshare-s3-132")
        self.assertIn("-DIDF_TARGET=esp32s3", args)
        self.assertIn("-DSDKCONFIG=build-muse-waveshare-s3-132/sdkconfig", args)
        self.assertIn("-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;"
                      "devices/sdkconfig.muse-waveshare-s3-132", args)

    def test_bench_has_separate_config_and_last_overlay(self):
        result = self.run_board("build", MUSE_BENCH="1")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        args = self.args_path.read_text().splitlines()
        self.assertEqual(args[args.index("-B") + 1], "build-muse-waveshare-s3-132-bench")
        self.assertIn("-DSDKCONFIG=build-muse-waveshare-s3-132-bench/sdkconfig", args)
        defaults = next(arg for arg in args if arg.startswith("-DSDKCONFIG_DEFAULTS="))
        self.assertTrue(defaults.endswith(";devices/sdkconfig.muse-bench"), defaults)

    def test_build_failure_is_not_reported_as_success(self):
        result = self.run_board("build", MUSE_TEST_BUILD_RC="17")
        self.assertEqual(result.returncode, 17, result.stderr + result.stdout)

    def test_unknown_action_stops_before_toolchain_or_flash(self):
        result = self.run_board("buidl")
        self.assertEqual(result.returncode, 2, result.stderr + result.stdout)
        self.assertIn("unknown action", result.stderr)
        self.assertFalse(self.args_path.exists())


class Waveshare132FlashLayout(unittest.TestCase):
    def test_selected_partition_table_fits_real_flash_and_has_two_ota_slots(self):
        overlay = (ROOT / "devices/sdkconfig.muse-waveshare-s3-132").read_text()
        settings = dict(line.split("=", 1) for line in overlay.splitlines()
                        if line.startswith("CONFIG_"))
        self.assertEqual(settings["CONFIG_ESPTOOLPY_FLASHSIZE"], '"8MB"')
        table = ROOT / settings["CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"].strip('"')
        rows = list(csv.reader(line for line in table.read_text().splitlines()
                               if line and not line.startswith("#")))
        last_end = 0x11000  # partition table is at 0x10000
        slots = []
        for row in rows:
            name, kind, subtype, offset, size = (v.strip() for v in row[:5])
            offset, size = int(offset, 0), int(size, 0)
            self.assertGreaterEqual(offset, last_end, name)
            last_end = offset + size
            self.assertLessEqual(last_end, 8 * 1024 * 1024, name)
            if kind == "app":
                self.assertEqual(offset % 0x10000, 0, name)
                slots.append((subtype, size))
        self.assertEqual([subtype for subtype, _ in slots], ["ota_0", "ota_1"])
        self.assertEqual(slots[0][1], slots[1][1])


class Waveshare132UsbSelection(unittest.TestCase):
    @staticmethod
    def port(device, serial, usb=ports.USJ):
        return SimpleNamespace(device=device, serial_number=serial, vid=usb[0], pid=usb[1])

    def test_ignores_unrelated_usb_bridge(self):
        attached = [self.port("/dev/cu.usbmodem101", "02:00:00:AA:01:32"),
                    self.port("/dev/cu.usbserial-1", "bridge", ports.CH9102)]
        self.assertEqual(ports.find("s3-132", ports=attached), "/dev/cu.usbmodem101")

    def test_native_usb_descriptor_does_not_disambiguate_two_boards(self):
        attached = [self.port("/dev/cu.usbmodem101", "02:00:00:AA:01:32"),
                    self.port("/dev/cu.usbmodem102", "AA:BB:CC:DD:EE:FF")]
        with self.assertRaises(ports.NotFound):
            ports.find("s3-132", ports=attached)
        self.assertEqual(ports.find("s3-132", "02:00:00:aa:01:32", attached),
                         "/dev/cu.usbmodem101")

    def test_missing_board_is_an_error(self):
        with self.assertRaises(ports.NotFound):
            ports.find("s3-132", ports=[])


if __name__ == "__main__":
    unittest.main()
