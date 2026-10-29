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

"""Behavioral e-ink regressions: actual firmware functions, fake panel/RTOS."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EINK = ROOT / "components/muse/eink"


class MuseEinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        ui = (EINK / "muse_eink_ui.cpp").read_text()
        settings = (EINK / "muse_eink_settings.cpp").read_text()

        def section(source, start, end):
            begin = source.index(start)
            return source[begin:source.index(end, begin)]

        parts = {
            "eink_epoch.inc": section(ui, "void navigation_changed(", "void request_draw("),
            "eink_touch.inc": section(ui, "struct TouchEvent {", "static void touch_task("),
            "eink_face.inc": section(ui, "static void face_screen(", "static void draw_dots("),
            "eink_image.inc": section(ui, "static SemaphoreHandle_t s_image_mutex;", "/* ─── The loop"),
            "eink_pages.inc": section(ui, "static bool s_settings;", "void navigation_changed(")
                + section(ui, "static void root_screen(", "static void on_muse(")
                + section(ui, "static void set_page(", "/* Touch events since"),
            "eink_handle_touch.inc": section(ui, "static void handle_touch(", "static void ui_task("),
            "eink_done.inc": section(ui, 'extern "C" void muse_ui_image_done(', 'extern "C" void muse_ui_image_hide('),
            "eink_fit.inc": section(settings, "static void fit(", "static void note("),
        }
        for name, content in parts.items():
            (out / name).write_text(content)
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        sanitizer = os.environ.get("MUSE_EINK_SANITIZERS", "address,undefined")
        if sanitizer not in ("address,undefined", "thread"):
            raise AssertionError("Use address,undefined or thread; sanitizer checks must remain enabled")
        cls.binary = out / "muse_eink"
        compiled = subprocess.run(
            [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-g",
             "-fno-omit-frame-pointer", f"-fsanitize={sanitizer}", "-fno-sanitize-recover=all",
             "-I", str(out), "-I", str(EINK),
             str(ROOT / "tests/muse_eink_harness.cpp"), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=120,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_image_hide_waits_for_both_planes(self):
        self.run_case("image_hide")

    def test_image_replace_waits_for_both_planes(self):
        self.run_case("image_replace")

    def test_image_hidden_before_presentation(self):
        self.run_case("image_empty")

    def test_image_allocation_failure_keeps_lifetime_lock(self):
        self.run_case("image_allocation_failure")

    def test_visible_face_mic_owns_press_until_release(self):
        self.run_case("touch_face")

    def test_settings_tap_is_not_an_invisible_mic(self):
        self.run_case("touch_settings")

    def test_image_tap_is_not_an_invisible_mic(self):
        self.run_case("touch_image")

    def test_page_change_releases_held_mic_without_settings_tap(self):
        self.run_case("touch_hold_modal")

    def test_wake_and_pairing_keep_physical_press_semantics(self):
        self.run_case("touch_wake_pairing")

    def test_queued_press_duration_uses_physical_edge_timestamps(self):
        self.run_case("touch_longpress")

    def test_utf8_width_and_capacity_truncation_preserve_codepoints(self):
        self.run_case("utf8")


if __name__ == "__main__":
    unittest.main()
