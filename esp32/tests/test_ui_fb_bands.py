#!/usr/bin/env python3
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
"""A two-band UI stream whose bottom band is narrower than
its top. muse_ui_fb.c (the UI frame buffer) reads both bands at the copy's
width; a 464-wide stream with a 464x16 top and a 16x16 bottom read
up to byte 703 of a 431-byte picture. Under ASan the helper must refuse it with
nothing read, and draw a valid two-band picture as before; muse_video.c's
decode task must fail such a picture before the show task sees it."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

FAKES = {
    "esp_heap_caps.h": "#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
                       "static inline void *heap_caps_malloc(size_t n, unsigned c) { (void)c; return malloc(n); }\n"
                       "static inline void *heap_caps_calloc(size_t a, size_t n, unsigned c) { (void)c; return calloc(a, n); }\n",
    "esp_log.h": "#pragma once\n#include <stdio.h>\n#define ESP_LOGE(t, ...) (printf(__VA_ARGS__), printf(\"\\n\"))\n"
                 "#define ESP_LOGW ESP_LOGE\n#define ESP_LOGI ESP_LOGE\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nstatic inline int64_t esp_timer_get_time(void) { return 0; }\n",
    "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY 0xffffffffu\n",
    "freertos/semphr.h": "#pragma once\ntypedef int *SemaphoreHandle_t;\nstatic int s_mutex;\n"
                         "static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex; }\n"
                         "static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) { (void)s; (void)t; return 1; }\n"
                         "static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }\n",
    "sdkconfig.h": "#pragma once\n",
}


class UiFbBands(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shutil.which("cc") or shutil.which("clang")
        if not cc:
            raise unittest.SkipTest("no C compiler")
        cls.tmp = Path(tempfile.mkdtemp(prefix="ui_fb_bands_"))
        for name, text in FAKES.items():
            (cls.tmp / name).parent.mkdir(parents=True, exist_ok=True)
            (cls.tmp / name).write_text(text)
        cls.exe = cls.tmp / "harness"
        subprocess.run([cc, "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(cls.tmp),
                        "-I", str(MUSE), str(ROOT / "tests/ui_fb_bands_harness.c"), str(MUSE / "muse_ui_fb.c"),
                        "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_case(self, which: str) -> str:
        r = subprocess.run([str(self.exe), which], capture_output=True, text=True,
                           env={"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0"})
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("AddressSanitizer", r.stderr)
        return r.stdout

    def test_report_shape_refused(self):
        out = self.run_case("bad")
        self.assertIn("bad: -1 rects", out)        # 464x16 over 16x16: nothing read
        self.assertIn("one band 448 under 464: -1", out)

    def test_valid_two_bands_draw(self):
        out = self.run_case("good")
        self.assertRegex(out, r"good: [1-9]\d* rects, 58 of 58 macroblocks")   # 29 x 2, all new

    def test_a_keyframe_keeps_unchanged_still_patches(self):
        # all (a keyframe, a woken screen) converts every macroblock except a lossless still patch's that the
        # picture didn't change: it keeps the patch and is still drawn (all redraws everything). (The patch path
        # needs the ROM's inflate, so this is the source's shape; ui_fb_bands_harness has no patches.)
        fb = (MUSE / "muse_ui_fb.c").read_text()
        self.assertIn("if (mb_changed(p0, p1, h0, mx, my, all && !(*f & MB_PATCHED))) {", fb)
        keep = fb.index("if (!conv && all && (f & MB_PATCHED)) {")
        self.assertLess(keep, fb.index("if (conv) {", keep))
        self.assertNotIn("muse_yuv_1x_hq_cols", fb[keep:fb.index("if (conv) {", keep)])

    def test_decode_task_fails_unequal_bands(self):
        video = (MUSE / "muse_video.c").read_text()
        self.assertIn("(sl->p1.w != pic.w || pic.w > (int)((s_params.width + 15) & ~15u) ||", video)
        self.assertIn("!muse_pic_covers(&pic, pic.w, h) || !muse_pic_covers(&sl->p1, pic.w, s_band.h))) {", video)
        fb = (MUSE / "muse_ui_fb.c").read_text()
        i = fb.index("int muse_ui_fb_picture(")
        self.assertLess(fb.index("muse_pic_covers(p0, s_w", i), fb.index("mb_changed(p0, p1", i))


if __name__ == "__main__":
    unittest.main()
