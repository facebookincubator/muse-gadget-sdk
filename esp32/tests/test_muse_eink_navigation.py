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

"""Actual pinned FreeInk routing and SPI-bus reuse against hardware-free adapters."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MuseEinkNavigationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(os.environ.get("FREEINK_SOURCE_DIR", ROOT / "managed_components/freeink-sdk"))
        ui_root = source / "libs/ui/FreeInkUI"
        if not (ui_root / "include/FreeInkApp.h").is_file():
            raise AssertionError("Pinned FreeInk source missing: set FREEINK_SOURCE_DIR to its checkout")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        ui = (ROOT / "components/muse/eink/muse_eink_ui.cpp").read_text()
        settings = (ROOT / "components/muse/eink/muse_eink_settings.cpp").read_text()
        board = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        spi = (source / "idf/src/ArduinoCompat.cpp").read_text()

        def section(text, start, end):
            begin = text.index(start)
            return text[begin:text.index(end, begin)]

        parts = {
            "navigation_types.inc": section(ui, "struct TouchEvent {", "static QueueHandle_t")
                + section(settings, "enum Page {", "static Page s_page"),
            "keyboard_snapshot.inc": section(settings, "static const freeink::ui::KeyboardLayout *s_keyboard_frame;", "static const char *sleep_name("),
            "keyboard_dispatch.inc": "static void keyboard_action(const ActionEvent &e, void *) {\n"
                "    if (e.action == ACT_KB_KEY) key_actions++;\n    switch (e.action) {\n"
                + section(settings, "    case ACT_KB_KEY:", "    default: break;")
                + "    default: assert(false);\n    }\n    settings_tick();\n}\n",
            "navigation_epoch.inc": section(ui, "void navigation_changed(", "void request_draw("),
            "navigation_settings.inc": section(settings, "static void show(", "bool settings_in_subpage(")
                + section(settings, "bool settings_back(", "void settings_home(")
                + section(settings, "bool settings_typing(", "void settings_scroll("),
            "navigation_handlers.inc": section(settings, "static void on_wifi_pass(", "static void on_hatch_host_done(")
                + section(settings, "static void text_ready(", "static void on_action("),
            "navigation_input.inc": section(ui, "static void handle_touch(", "static void ui_task("),
            "sticky_sd_bus.inc": section(board, "static esp_err_t sd_bus_init(", "static void sd_task("),
            "freeink_spi_begin.inc": section(spi, "void SPIClass::begin(", "void SPIClass::end("),
        }
        for name, content in parts.items():
            (out / name).write_text(content)
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        sanitizer = os.environ.get("MUSE_EINK_SANITIZERS", "address,undefined")
        if sanitizer not in ("address,undefined", "thread"):
            raise AssertionError("Sanitizer checks must remain enabled")
        cls.binary = out / "navigation"
        result = subprocess.run(
            [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-pthread", "-g",
             "-fno-omit-frame-pointer", f"-fsanitize={sanitizer}", "-fno-sanitize-recover=all",
             "-I", str(out), "-I", str(ui_root / "include"),
             str(ROOT / "tests/muse_eink_navigation_harness.cpp"), str(ui_root / "src/FreeInkUI.cpp"),
             "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=120,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_queued_ap_double_tap_does_not_reopen_or_type_into_editor(self):
        self.run_case("double_tap")

    def test_editor_return_page_is_explicit_even_on_reentry(self):
        self.run_case("explicit_back")

    def test_other_ssid_to_password_preserves_return_page_and_wipes_text(self):
        self.run_case("ssid_password_chain")

    def test_rapid_same_page_keys_are_drained_without_loss(self):
        self.run_case("rapid_keys")

    def test_tap_sampled_during_navigation_cannot_become_a_new_page_key(self):
        self.run_case("during_transition")

    def test_orphan_release_cannot_dispatch_old_coordinates(self):
        self.run_case("orphan_release")

    def test_swipe_navigation_fences_remaining_old_page_taps(self):
        self.run_case("swipe_then_tap")

    def test_sd_miso_survives_real_freeink_spi_begin(self):
        self.run_case("shared_bus")

    def test_sd_bus_failure_is_propagated(self):
        self.run_case("bus_failure")

    def test_panel_without_sd_keeps_its_existing_bus_contract(self):
        self.run_case("panel_only")

    def test_shift_then_letter_uses_rebuilt_keyboard_without_losing_tap(self):
        self.run_case("shift_key")

    def test_repeated_shift_toggles_preserve_queued_letter(self):
        self.run_case("repeat_shift")

    def test_automatic_shift_release_rebuilds_before_next_queued_letter(self):
        self.run_case("auto_shift")

    def test_cyrillic_shift_uses_actual_keyboard_ids(self):
        self.run_case("cyrillic_shift")

    def test_accented_shift_uses_actual_keyboard_ids(self):
        self.run_case("accented_shift")

    def test_abc_and_symbols_transitions_fence_old_geometry(self):
        self.run_case("symbols")

    def test_symbol_page_shift_fences_old_geometry(self):
        self.run_case("symbol_shift")

    def test_language_transition_fences_old_geometry(self):
        self.run_case("language")

    def test_queued_backspace_space_and_enter_use_actual_keyboard_actions(self):
        self.run_case("controls")

    def test_video_option_excludes_freeink_without_changing_lcd_targets(self):
        config = (ROOT / "components/muse/Kconfig").read_text()
        block = config[config.index("    config MUSE_VIDEO\n"):config.index("    config MUSE_VIDEO_WIFI\n")]
        dependency = re.search(r"depends on (.+)", block).group(1)
        self.assertIn("!MUSE_UI_FREEINK", dependency)
        self.assertIn("MUSE_ENABLED && SPIRAM", dependency)
        self.assertIn("IDF_TARGET_ESP32S3 || IDF_TARGET_ESP32S31", dependency)

    def test_ci_freeink_source_matches_dependency_pin_and_read_only_checkout(self):
        manifest = (ROOT / "components/muse/idf_component.yml").read_text()
        pin = re.search(r"freeink-sdk:\s+git:.*?\s+version: ([0-9a-f]{40})", manifest).group(1)
        workflow = (ROOT.parent / ".github/workflows/esp32.yml").read_text()
        self.assertIn(f"ref: {pin}", workflow)
        self.assertIn("repository: Free-Ink/freeink-sdk", workflow)
        self.assertIn("FREEINK_SOURCE_DIR:", workflow)
        self.assertIn("persist-credentials: false", workflow)


if __name__ == "__main__":
    unittest.main()
