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

"""Per-board choices in the status-screen backends and the session profile.

Runs the firmware's own preprocessor blocks with each board's options, so a
change for one board can't silently change another.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LED_C = ROOT / "main" / "led_status.c"
NOISE_CPP = ROOT / "main" / "noise_control.cpp"

ST7789_BACKENDS = (
    "CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789",
    "CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_C6_ST7789",
)
CYD = "CONFIG_HOMEHUB_LED_BACKEND_CYD_ILI9341"
STATUS_SCREENS = (*ST7789_BACKENDS, CYD, "CONFIG_HOMEHUB_LED_BACKEND_SENSECAP_ST7701")


def _cc() -> list[str]:
    return shlex.split(os.environ.get("CC", "cc"))


def _between(source: str, start: str, end: str) -> str:
    i = source.index(start)
    return source[i:source.index(end, i)]


def _preprocess(text: str, defines: dict[str, int]) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "block.c"
        src.write_text(text)
        args = [f"-D{k}={v}" for k, v in defines.items()]
        return subprocess.run([*_cc(), "-E", "-P", *args, str(src)],
                              check=True, capture_output=True, text=True).stdout


def _run_c(text: str, defines: dict[str, int]) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.c"
        exe = Path(tmp) / "probe"
        src.write_text(text)
        args = [f"-D{k}={v}" for k, v in defines.items()]
        subprocess.run([*_cc(), "-std=c11", *args, str(src), "-o", str(exe)],
                       check=True, capture_output=True, text=True)
        return subprocess.run([str(exe)], check=True, capture_output=True,
                              text=True).stdout


class SpiPanelInitTest(unittest.TestCase):
    def setUp(self) -> None:
        source = LED_C.read_text()
        self.init = _between(source, "static esp_err_t lcd_panel_init(void) {",
                             "static esp_err_t lcd_panel_on(void)")

    def test_st7789_boards_keep_st7789_inversion_and_gap(self) -> None:
        for backend in ST7789_BACKENDS:
            with self.subTest(backend=backend):
                out = _preprocess(self.init, {backend: 1})
                self.assertIn("esp_lcd_new_panel_st7789(", out)
                self.assertNotIn("ili9341", out)
                self.assertIn("esp_lcd_panel_invert_color(s_panel, true)", out)
                self.assertIn("esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0)", out)
                self.assertNotIn("esp_lcd_panel_mirror(", out)

    def test_cyd_uses_ili9341_mirrored_without_st7789_settings(self) -> None:
        out = _preprocess(self.init, {CYD: 1})
        self.assertIn("esp_lcd_new_panel_ili9341(", out)
        self.assertNotIn("st7789", out)
        self.assertIn("esp_lcd_panel_mirror(s_panel, true, false)", out)
        self.assertNotIn("esp_lcd_panel_invert_color(", out)
        self.assertNotIn("esp_lcd_panel_set_gap(", out)


class TitleGeometryTest(unittest.TestCase):
    def _geometry(self, backend: str) -> dict[str, int]:
        source = LED_C.read_text()
        block = _between(source, 'static const char *TAG = "link.led";',
                         "static SemaphoreHandle_t s_mutex")
        probe = (
            "#include <stdio.h>\n"
            "#define HAPPY_ANIM_WIDTH 56\n#define HAPPY_ANIM_HEIGHT 59\n"
            "#define PIXEL_FONT_HEIGHT 8\n"
            + block +
            "\nint main(void) {\n"
            '    printf("%d %d %d %d\\n", LCD_TITLE_Y, LCD_TITLE_MAX_SCALE,\n'
            "           LCD_ANIM_SCALE, LCD_BAR_ROWS);\n"
            "    return 0;\n}\n"
        )
        y, title, anim, bars = map(int, _run_c(
            probe, {backend: 1, "CONFIG_HOMEHUB_DISPLAY": 1}).split())
        return {"y": y, "title": title, "anim": anim, "bars": bars}

    def test_title_starts_below_the_top_bar_on_every_status_screen(self) -> None:
        for backend in STATUS_SCREENS:
            with self.subTest(backend=backend):
                g = self._geometry(backend)
                self.assertGreaterEqual(g["y"], g["bars"])

    def test_only_the_cyd_shrinks_the_title(self) -> None:
        for backend in STATUS_SCREENS:
            with self.subTest(backend=backend):
                g = self._geometry(backend)
                if backend == CYD:
                    self.assertEqual(g["title"], 2)
                    self.assertLess(g["title"], g["anim"])
                else:
                    self.assertEqual(g["title"], g["anim"])


class SmallControlSessionTest(unittest.TestCase):
    def _small(self, defines: dict[str, int]) -> int:
        source = NOISE_CPP.read_text()
        start = source.index("#define SMALL_CONTROL_SESSION 1")
        start = source.rindex("#if", 0, start)
        end = source.index("#endif", start) + len("#endif")
        out = _preprocess(source[start:end] + "\nsmall=SMALL_CONTROL_SESSION\n", defines)
        return int(out.split("small=")[1].split()[0])

    def test_muse_boards_without_psram_or_tunnel_keep_the_small_profile(self) -> None:
        # Cardputer ADV, AI Passport and the Waveshare C6 AMOLED boards.
        self.assertEqual(self._small({"CONFIG_MUSE_ENABLED": 1}), 1)

    def test_cyd_takes_the_small_profile(self) -> None:
        self.assertEqual(self._small({CYD: 1}), 1)

    def test_other_boards_keep_the_usual_profile(self) -> None:
        cases = {
            "ideaspark / C6 no PSRAM": {"CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789": 1},
            "Waveshare C6 LCD": {"CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_C6_ST7789": 1},
            "Muse with PSRAM": {"CONFIG_MUSE_ENABLED": 1, "CONFIG_SPIRAM": 1},
            "Muse with tunnel": {"CONFIG_MUSE_ENABLED": 1, "CONFIG_HOMEHUB_TUNNEL": 1},
            "default": {},
        }
        for name, defines in cases.items():
            with self.subTest(board=name):
                self.assertEqual(self._small(defines), 0)


if __name__ == "__main__":
    unittest.main()
