# SPDX-License-Identifier: Apache-2.0
"""Exercise the real board callbacks with host I2C and LVGL fakes."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^static (?:void|esp_err_t) " + name + r"\([^)]*\)\n\{", source, re.M)
    assert match, name
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class Waveshare185bBoard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        temp = Path(cls.tmp.name)
        cls.source = (ROOT / "components/muse/boards/board_waveshare_s3_185b.c").read_text()
        (temp / "waveshare_185b_board_functions.h").write_text(
            function(cls.source, "round_area") + "\n" + function(cls.source, "read_power")
        )
        cls.binary = temp / "185b_board"
        built = subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(temp), "-I", str(ROOT / "tests/pmu_fakes"),
            "-I", str(ROOT / "components/muse"),
            str(ROOT / "tests/waveshare_185b_board_harness.c"), "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stderr)

    def test_update_windows_cover_pixels_without_crossing_panel_edges(self):
        self.assertIn("lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL)", self.source)
        subprocess.run([str(self.binary), "alignment"], check=True, capture_output=True, text=True)

    def test_optional_gauge_allocation_failure_and_recovery(self):
        self.assertIn(".read_power = read_power,", self.source)
        subprocess.run([str(self.binary), "power"], check=True, capture_output=True, text=True)
