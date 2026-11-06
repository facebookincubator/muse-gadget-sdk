# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Behavioral tests for the SenseCAP Watcher power-test board hooks.

The real BSP function bodies (pin knobs, expander ownership, apply ordering,
readback, service and the vendor codec shutdown) are compiled against modeled
ESP-IDF drivers in power_test_board_harness.c. Each scenario asserts inside the
harness and prints "ok"; a failed CHECK names its line on stderr.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from power_test_fixture import function_body

BSP = ROOT / "components/muse/boards/board_sensecap_watcher.c"


def board_functions(source):
    """The power-test hooks, verbatim, minus panel/ADC/audio bring-up."""
    def between(start, end):
        first = source.index(start)
        return source[first:source.index(end, first)]

    pieces = [
        between("#define LCD_RES", "static i2c_master_bus_handle_t s_i2c;"),
        function_body(source, "static esp_err_t exp_write(")[0],
        function_body(source, "static esp_err_t exp_read(")[0],
        between("static bool s_ptest_lcd_on", "static bool ptest_draw_complete("),
    ]
    start = source.index("esp_err_t muse_ptest_board_apply(")
    _, end = function_body(source, "esp_err_t muse_ptest_board_service(")
    pieces.append(source[start:end])
    return "\n\n".join(pieces)


class WatcherPowerTestBoardHooks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="watcher-power-board-")
        directory = Path(cls.tmp.name)
        (directory / "power_test_board_functions.inc").write_text(board_functions(BSP.read_text()))
        cls.binary = directory / "harness"
        compilation = subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "tests/power_test_fakes"), "-I", str(ROOT / "components/muse"),
             "-I", str(directory), str(ROOT / "tests/power_test_board_harness.c"), "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compilation.returncode:
            raise AssertionError(compilation.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, number):
        result = subprocess.run([str(self.binary), str(number)], capture_output=True, text=True, timeout=5)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "ok\n", ""))

    def test_every_knob_parks_reads_back_and_restores_exact_pad_state(self):
        self.scenario(0)

    def test_i2s_high_z_parks_and_restores(self):
        self.scenario(1)

    def test_rejected_knobs_change_no_pad(self):
        # Conflicting I2S modes, open codecs, UART on USB, SD/AI rails on.
        self.scenario(2)

    def test_uart_parks_only_after_tx_drain_and_restore_retries(self):
        self.scenario(3)

    def test_knob_readback_catches_each_kind_of_drift(self):
        self.scenario(4)

    def test_failed_snapshot_or_restore_keeps_recoverable_state(self):
        self.scenario(5)

    def test_revert_releases_deep_holds_owned_by_marker_or_mask(self):
        self.scenario(6)

    def test_expander_writes_only_owned_rails_and_decodes_vbus(self):
        self.scenario(7)

    def test_apply_guards_and_amp_off_before_codec_work(self):
        self.scenario(8)

    def test_codec_policies_run_constructor_only_or_open_close(self):
        self.scenario(9)

    def test_readback_checks_expander_duty_pins_and_codec_transport(self):
        self.scenario(10)

    def test_service_counts_whole_frames_bounded_sine_and_1hz_adc(self):
        self.scenario(11)

    def test_vendor_shutdown_runs_whole_sequence_and_keeps_first_error(self):
        self.scenario(12)

    def test_retained_codec_history_never_claims_colder_hardware(self):
        self.scenario(13)


if __name__ == "__main__":
    unittest.main()
