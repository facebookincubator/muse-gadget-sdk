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
"""esp_lvgl_adapter's pause handshake fix (cmake/esp_lvgl_adapter_fix.cmake).

espressif/esp_lvgl_adapter 0.6.4's esp_lv_adapter_pause() marks the adapter
paused, then clears any pending acknowledgement: one the LVGL worker posts in
between is cleared away and the pause waits for good (forever with -1; with
a timeout it returns, but the worker stays asleep in its paused wait). The
build compiles a copy with the order fixed and a timed-out pause waking the
worker.

This runs the build's own edit (`cmake -P`) on upstream's pause function
(tests/vectors/esp_lvgl_adapter/pause_0.6.4.c, verbatim): applied to text it
wasn't made for, it stops the build.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "cmake/esp_lvgl_adapter_fix.cmake"
VECTOR = ROOT / "tests/vectors/esp_lvgl_adapter/pause_0.6.4.c"

APPLY = r'''
include("${FIX}")
file(READ "${IN}" code)
muse_lvgl_adapter_fix_text("${code}" code)
file(WRITE "${OUT}" "${code}")
'''


class LvglAdapterPauseFix(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = shutil.which("cmake")
        if not cls.cmake:
            raise AssertionError("cmake is needed (the firmware build needs it too)")
        cls.tmp = Path(tempfile.mkdtemp(prefix="lvgl_adapter_fix_"))
        (cls.tmp / "apply.cmake").write_text(APPLY)
        (cls.tmp / "fixed").mkdir()
        cls.apply(VECTOR, cls.tmp / "fixed/pause.c")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def apply(cls, src: Path, out: Path) -> subprocess.CompletedProcess:
        r = subprocess.run([cls.cmake, f"-DFIX={FIX}", f"-DIN={src}", f"-DOUT={out}", "-P", str(cls.tmp / "apply.cmake")],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)
        return r

    def test_the_edit_refuses_text_it_was_not_made_for(self) -> None:
        # applied twice (or to a fixed upstream) it stops the build instead of guessing
        once = self.tmp / "fixed/pause.c"
        r = subprocess.run([self.cmake, f"-DFIX={FIX}", f"-DIN={once}", f"-DOUT={self.tmp / 'twice.c'}", "-P",
                            str(self.tmp / "apply.cmake")], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("re-check its pause handshake", r.stderr.replace("\n", " ").replace("  ", " "))


HOLD_STUBS = {
    "esp_log.h": "#pragma once\n#define ESP_LOGW(t, ...) ((void)(t))\n#define ESP_LOGE(t, ...) ((void)(t))\n",
    "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY 0xffffffffu\ntypedef int portMUX_TYPE;\n"
                           "#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(m) ((void)(m))\n"
                           "#define portEXIT_CRITICAL(m) ((void)(m))\n",
    "freertos/semphr.h": "#pragma once\ntypedef int *SemaphoreHandle_t;\ntypedef int StaticSemaphore_t;\nstatic int s_sem;\n"
                         "static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) { (void)b; return &s_sem; }\n"
                         "static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) { (void)s; (void)t; return 1; }\n"
                         "static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }\n",
}
ADAPTER_STUB = ("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\nextern int pauses, resumes, last_ms;\n"
                "static inline esp_err_t esp_lv_adapter_pause(int ms) { pauses++; last_ms = ms; return ESP_OK; }\n"
                "static inline esp_err_t esp_lv_adapter_resume(void) { resumes++; return ESP_OK; }\n")
HOLD_MAIN = r"""
#include <stdio.h>
#include "muse_lvgl_hold.h"
int pauses, resumes, last_ms;
int main(void)
{
    int held = muse_lvgl_hold(MUSE_LVGL_HOLD_VIDEO, true);
    int stopped = muse_lvgl_hold_stopped();
    int still = muse_lvgl_hold(MUSE_LVGL_HOLD_VIDEO, false);
    printf("held %d stopped %d after %d pauses %d resumes %d ms %d\n", held, stopped, still, pauses, resumes, last_ms);
    return 0;
}
"""


class LvglHoldWithoutTheAdapter(unittest.TestCase):
    """muse_lvgl_hold.c is in every Muse build; esp_lvgl_adapter only on the boards listed in
    components/muse/idf_component.yml. Without it (the BOX-3, CoreS3, Core2, C6 2.06,
    JC3248W535, S3 LCD 7: their builds would stop at its include) it compiles, counts the
    holders and pauses nothing; with it, the bounded pause and the resume as before."""

    def run_hold(self, adapter: bool) -> str:
        cc = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory() as d:
            inc = Path(d) / "inc"
            for name, text in HOLD_STUBS.items():
                (inc / name).parent.mkdir(parents=True, exist_ok=True)
                (inc / name).write_text(text)
            if adapter:
                (inc / "esp_lv_adapter.h").write_text(ADAPTER_STUB)
            main = Path(d) / "main.c"
            main.write_text(HOLD_MAIN)
            exe = Path(d) / "hold"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(inc),
                            "-I", str(ROOT / "components/muse"), str(main),
                            str(ROOT / "components/muse/muse_lvgl_hold.c"), "-o", str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip()

    def test_without_the_adapter_it_builds_and_pauses_nothing(self) -> None:
        self.assertEqual(self.run_hold(False), "held 1 stopped 0 after 0 pauses 0 resumes 0 ms 0")

    def test_with_the_adapter_unchanged(self) -> None:
        self.assertEqual(self.run_hold(True), "held 1 stopped 1 after 0 pauses 1 resumes 1 ms 500")


if __name__ == "__main__":
    unittest.main()
