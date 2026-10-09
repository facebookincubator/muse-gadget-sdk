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

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkEpaperStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        def extract(panel):
            filename = "epaper_154g_status.c" if panel == "154g" else "epaper_status.c"
            source = (ROOT / "main" / filename).read_text()
            start = source.index("// ---- Pixels (host-tested)")
            pixels = source[start:source.index("// ---- Panel", start)]
            if panel == "154g":
                pixels += "\n#define dither_color(rgb,codes,w,h,err) ((void)(err),map_frame(rgb,codes,w,h))\n"
            (out / "epaper_pixels.inc").write_text(pixels)
        cc = shlex.split(os.environ.get("CC", "cc"))
        # The Spectra 6's 4-bit ink codes, then the 1.54G's 2-bit ones.
        for bits in (4, 2):
            extract("154g" if bits == 2 else "reterminal")
            cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-DEPD_INK_BITS={bits}",
                   "-I", str(out), str(ROOT / "tests/link_epaper_status_harness.c"),
                   "-o", str(out / f"epaper{bits}")]
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_pixels(self):
        for bits in (4, 2):
            with self.subTest(ink_bits=bits):
                result = subprocess.run([str(self.out / f"epaper{bits}")], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_draw_url_states_the_bit_depth(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_DISPLAY_COMMANDS")
        block = noise[start:noise.index('"display.show_animation"', start)]
        self.assertIn("int bits = led_status_display_bits();", block)
        self.assertIn("1 bit per pixel", block)
        self.assertIn("16 bits per pixel (RGB565)", block)

    def test_draw_url_states_the_colour_inks(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_DISPLAY_COMMANDS")
        block = noise[start:noise.index('"display.show_animation"', start)]
        self.assertIn("bits == 4", block)
        self.assertIn("six-colour e-paper screen (E Ink Spectra 6)", block)
        self.assertIn("4 bits per pixel", block)
        self.assertIn("bits == 2", block)
        self.assertIn("four-colour e-paper screen, 2 bits per pixel", block)
        # Every ink each colour panel dithers to is named with its exact colour.
        driver = (ROOT / "main/epaper_status.c").read_text() + (ROOT / "main/epaper_154g_status.c").read_text()
        tables = re.findall(r"s_inks\[\] = \{(.*?)\n\};", driver, re.S)
        inks = [re.findall(r"\{\d, (\d+), (\d+), (\d+)\},\s+// (\w+)", t) for t in tables]
        self.assertEqual(sorted(len(t) for t in inks), [4, 6])
        text = re.sub(r'"\s*"', "", block)
        for table in inks:
            for r, g, b, name in table:
                self.assertIn(f"{name} #{int(r):02x}{int(g):02x}{int(b):02x}", text)
        self.assertIn("led_status_display_bits", driver)
        self.assertIn("return s_panel->bits_per_pixel;", driver)
        self.assertIn("return EPD_INK_BITS;", driver)
        for panel, bits in (("epd_spectra6.c", 4), ("epd_uc8179.c", 1), ("epd_ssd1681.c", 1)):
            self.assertRegex((ROOT / "main" / panel).read_text(),
                             rf"\.bits_per_pixel\s*=\s*{bits},", panel)

    def test_epaper_replaces_led_status_and_refreshes_after_each_image(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        # Each e-paper backend builds the status screen with its panel driver.
        self.assertRegex(cmake, r'if\(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_uc8179.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_spectra6.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_SSD1681\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_ssd1681.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_154g_status.c"\)\s*'
                                r'else\(\)\s*'
                                r'list\(APPEND GADGET_SRCS "led_status.c"\)')
        # Both implementations provide the whole display interface.
        header = (ROOT / "main/led_status.h").read_text()
        for name in re.findall(r"^\w[\w ]*?\b(led_status_\w+)\(", header, re.M):
            if name in ("led_status_set_voice", "led_status_set_level", "led_status_show_volume"):
                continue
            for impl in ("led_status.c", "epaper_status.c", "epaper_154g_status.c"):
                self.assertRegex((ROOT / "main" / impl).read_text(), rf"\n\w[\w ]*\b{name}\(",
                                 f"{impl} lacks {name}")
        fetch = (ROOT / "main/image_fetch.c").read_text()
        task = fetch[fetch.index("static void fetch_task("):]
        self.assertLess(task.index("draw_raw(f, &rows)"), task.index("led_status_draw_done()"))


if __name__ == "__main__":
    unittest.main()
