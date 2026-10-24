# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and limitations.

"""Positive SDK text-validation and network-row correctness checks; no device/network IO."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EINK = ROOT / "components/muse/eink"


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


class _CompiledFixture(unittest.TestCase):
    @classmethod
    def compile(cls, harness, parts):
        source = Path(os.environ.get("FREEINK_SOURCE_DIR", ROOT / "managed_components/freeink-sdk"))
        ui = source / "libs/ui/FreeInkUI"
        if not (ui / "include/FreeInkApp.h").is_file():
            raise AssertionError("Set FREEINK_SOURCE_DIR to the unchanged pinned FreeInk checkout")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        for name, content in parts.items():
            (out / name).write_text(content)
        sanitizer = os.environ.get("MUSE_EINK_SANITIZERS", "address,undefined")
        if sanitizer not in ("address,undefined", "thread"):
            raise AssertionError("Required sanitizer checks must remain enabled")
        cls.binary = out / "fixture"
        result = subprocess.run(
            [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-UNDEBUG", "-pthread", "-g", "-fno-omit-frame-pointer", f"-fsanitize={sanitizer}",
             "-fno-sanitize-recover=all", "-I", str(out), "-I", str(EINK), "-I", str(ui / "include"),
             str(ROOT / "tests" / harness), str(ui / "src/FreeInkUI.cpp"), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class MuseEinkTextValidationTest(_CompiledFixture):
    @classmethod
    def setUpClass(cls):
        ui = (EINK / "muse_eink_ui.cpp").read_text()
        settings = (EINK / "muse_eink_settings.cpp").read_text()
        cls.compile("muse_eink_text_harness.cpp", {
            "drawing_pipeline.inc": section(ui, "int line_height(", "static void dot("),
            "drawing_fit.inc": section(settings, "static void fit(", "static void note("),
        })

    def test_incomplete_text_is_replaced_for_display(self):
        self.run_case("truncated")

    def test_invalid_scalar_encodings_are_replaced_for_display(self):
        self.run_case("invalid")

    def test_long_text_uses_bounded_chunks(self):
        self.run_case("long")

    def test_valid_unicode_and_original_input_bytes_are_preserved(self):
        self.run_case("valid")

    def test_byte_classes_have_nonexpanding_display_output(self):
        self.run_case("bytes")


class MuseEinkWifiOwnershipTest(_CompiledFixture):
    @classmethod
    def setUpClass(cls):
        ui = (EINK / "muse_eink_ui.cpp").read_text()
        settings = (EINK / "muse_eink_settings.cpp").read_text()
        header = (ROOT / "components/muse/muse_wifi.h").read_text()
        max_aps = re.search(r"#define MAX_APS (\d+)", settings).group(1)
        globals_text = section(settings, "/* Wi-Fi. */", "/* Muse. */")
        globals_text = globals_text.replace("static char s_mac[18];", "[[maybe_unused]] static char s_mac[18];")
        cls.compile("muse_eink_wifi_harness.cpp", {
            "wifi_types.inc": "#define MAX_APS " + max_aps + "\n" + section(header, "typedef enum {", "esp_err_t muse_wifi_start("),
            "wifi_page_types.inc": section(settings, "enum Page {", "static Page s_page"),
            "wifi_touch_types.inc": section(ui, "struct TouchEvent {", "static QueueHandle_t"),
            "wifi_epoch.inc": section(ui, "void navigation_changed(", "void request_draw("),
            "wifi_globals.inc": globals_text,
            "wifi_tick.inc": section(settings, "static bool is_saved(", "static void tick_hatch("),
            "wifi_input.inc": section(ui, "static void handle_touch(", "static void ui_task("),
            "wifi_actions.inc": section(settings, "    case ACT_WIFI_AP:", "    case ACT_WIFI_OTHER:"),
        })

    def test_queued_confirmations_only_remove_the_visible_saved_record(self):
        self.run_case("delete_owner")

    def test_reordered_saved_rows_reset_confirmation_and_old_taps(self):
        self.run_case("saved_reorder")

    def test_replaced_saved_rows_reset_confirmation(self):
        self.run_case("saved_membership")

    def test_reordered_scan_rows_drop_old_coordinates(self):
        self.run_case("scan_reorder")

    def test_signal_updates_keep_same_record_taps_valid(self):
        self.run_case("scan_status_only")

    def test_unpublished_row_indexes_are_ignored(self):
        self.run_case("bounds")


if __name__ == "__main__":
    unittest.main()
