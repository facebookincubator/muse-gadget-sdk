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

"""The ESP32-C3 0.42 inch OLED status backend in led_status.c.

Each status backend must define the led_hw_* functions exactly once, and the
OLED's drawing and I2C writes run on the host against a fake bus.
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LED_C = ROOT / "main" / "led_status.c"
CC = shlex.split(os.environ.get("CC", "cc"))

# Backend selections that compile led_status.c, with what Kconfig turns on
# alongside each.
BACKENDS = {
    "PWM_RGB": [],
    "DEVKIT_GPIO27": [],
    "VOICE_RING": [],
    "RESPEAKER_LITE": [],
    "IDEASPARK_ST7789": ["CONFIG_HOMEHUB_DISPLAY"],
    "WAVESHARE_C6_ST7789": ["CONFIG_HOMEHUB_DISPLAY"],
    "SENSECAP_ST7701": ["CONFIG_HOMEHUB_DISPLAY"],
    "MUSE": [],
    "C3_SSD1306": [],
    "NONE": [],
}

HW_FUNCS = {
    "led_hw_init": r"static bool led_hw_init\(void\)\s*\{",
    "led_hw_set_color": r"static void led_hw_set_color\(rgb_t c\)\s*\{",
    "led_hw_set_connected": r"static void led_hw_set_connected\(void\)\s*\{",
    "led_hw_set_title": r"static void led_hw_set_title\(const char \*text\)\s*\{",
}

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "led_status.h"
#include "pixel_font.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
static const char *TAG = "test";
typedef struct { uint8_t r, g, b; } rgb_t;
typedef struct fake_dev *i2c_master_dev_handle_t;
#define CONFIG_HOMEHUB_BUTTON_LABEL "BOOT"

/* Every transfer the backend makes, and whether the next ones fail. */
static uint8_t g_tx[64][80];
static size_t g_tx_len[64];
static int g_tx_count;
static int g_fail_from = -1;
static struct fake_dev { int unused; } g_dev;

static esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf,
                                     size_t len, int timeout_ms) {
    (void)timeout_ms;
    assert(dev == &g_dev);
    assert(len <= sizeof(g_tx[0]));
    int n = g_tx_count++;
    assert(n < 64);
    memcpy(g_tx[n], buf, len);
    g_tx_len[n] = len;
    return g_fail_from >= 0 && n >= g_fail_from ? ESP_FAIL : ESP_OK;
}

#include "oled_defs.inc"
#include "oled_impl.inc"

static void reset_tx(void) { g_tx_count = 0; g_fail_from = -1; }

static bool lit(const uint8_t *fb, int x, int y) {
    return fb[(y / 8) * OLED_WIDTH + x] >> (y % 8) & 1;
}

/* One full frame: the window, then one data transfer per page. */
static void check_frame(int first) {
    assert(g_tx_count - first == 1 + OLED_XFER_PAGES);
    static const uint8_t window[] = {0x00, 0x21, 30, 101, 0x22, 1, 6};
    assert(g_tx_len[first] == sizeof(window));
    assert(memcmp(g_tx[first], window, sizeof(window)) == 0);
    for (int p = 0; p < OLED_XFER_PAGES; p++) {
        assert(g_tx_len[first + 1 + p] == 1 + OLED_WIDTH);
        assert(g_tx[first + 1 + p][0] == 0x40);
    }
}

int main(void) {
    assert(OLED_COLS == 12 && OLED_XFER_PAGES == 6 && OLED_FIRST_PAGE == 1);
    s_oled = &g_dev;

    /* The status draws once and isn't written again while it stays. */
    reset_tx();
    oled_show_state(LED_STATE_BOOT);
    check_frame(0);
    oled_show_state(LED_STATE_BOOT);
    assert(g_tx_count == 1 + OLED_XFER_PAGES);

    /* The frame buffer's top row lands 6 rows down, in the first page sent. */
    memset(s_oled_fb, 0, sizeof(s_oled_fb));
    s_oled_fb[0] = 0x01;                      /* x 0, y 0 */
    s_oled_fb[4 * OLED_WIDTH + 71] = 0x80;    /* x 71, y 39 */
    s_oled_dirty = true;
    reset_tx();
    oled_flush();
    check_frame(0);
    assert(g_tx[1][1 + 0] == 1 << 6);
    assert(g_tx[1 + 5][1 + 71] == 1 << 5);    /* row 39 + 6 = 45: page 5, bit 5 */

    /* A failed write is retried on the next frame, then stops. */
    memset(s_oled_fb, 0, sizeof(s_oled_fb));
    s_oled_status = NULL;
    reset_tx();
    g_fail_from = 2;
    oled_show_state(LED_STATE_PAIRING_CONFIRM_REQUIRED);
    assert(g_tx_count == 3 && s_oled_dirty && s_oled_failed);
    g_fail_from = -1;
    int before = g_tx_count;
    oled_show_state(LED_STATE_PAIRING_CONFIRM_REQUIRED);
    check_frame(before);
    assert(!s_oled_dirty && !s_oled_failed);
    before = g_tx_count;
    oled_show_state(LED_STATE_PAIRING_CONFIRM_REQUIRED);
    assert(g_tx_count == before);

    /* The confirm prompt says which button, wrapped at a space. */
    static uint8_t want[sizeof(s_oled_fb)], got[sizeof(s_oled_fb)];
    memcpy(got, s_oled_fb, sizeof(got));
    memset(s_oled_fb, 0, sizeof(s_oled_fb));
    oled_draw_line(0, "Press BOOT", 10);
    oled_draw_line(1, "to confirm", 10);
    memcpy(want, s_oled_fb, sizeof(want));
    assert(memcmp(got, want, sizeof(got)) == 0);

    /* Each state has its own words; the confirm prompt differs from setup progress. */
    assert(strcmp(OLED_LABELS[LED_STATE_PAIRING_CONFIRM_REQUIRED], "Press BOOT to confirm") == 0);
    assert(strcmp(OLED_LABELS[LED_STATE_WIFI_CONNECTING], "Connecting") == 0);
    for (int st = LED_STATE_BOOT; st <= LED_STATE_ERROR; st++) {
        const char *label = OLED_LABELS[st];
        assert(label && *label);
        int len = (int)strlen(label);
        assert(len <= 2 * OLED_COLS);
    }

    /* The name goes on lines 2 and 3 and leaves the status alone; a long or
       non-ASCII name is cut off and shown as '?', never drawn out of bounds. */
    memset(s_oled_fb, 0, sizeof(s_oled_fb));
    oled_draw_line(0, "Connected", 9);
    memcpy(want, s_oled_fb, 2 * OLED_WIDTH);
    reset_tx();
    led_hw_set_title("A very long agent name that does not fit \xe2\x9c\x93");
    check_frame(0);
    assert(memcmp(s_oled_fb, want, 2 * OLED_WIDTH) == 0);
    bool any = false;
    for (int y = 2 * OLED_LINE_H; y < OLED_HEIGHT; y++) {
        for (int x = 0; x < OLED_WIDTH; x++) any |= lit(s_oled_fb, x, y);
    }
    assert(any);
    memcpy(got, s_oled_fb, sizeof(got));
    memset(s_oled_fb, 0, sizeof(s_oled_fb));
    oled_draw_line(0, "Connected", 9);
    oled_draw_line(2, "A very long", 11);
    oled_draw_line(3, "agent name t", 12);
    assert(memcmp(got, s_oled_fb, sizeof(got)) == 0);
    oled_clear_lines(2, 2);
    oled_draw_line(2, "\xe2\x9c\x93", 3);
    memcpy(got, s_oled_fb, sizeof(got));
    oled_clear_lines(2, 2);
    oled_draw_line(2, "???", 3);
    assert(memcmp(got, s_oled_fb, sizeof(got)) == 0);

    /* Clearing the name blanks lines 2 and 3. */
    led_hw_set_title("");
    for (int y = 2 * OLED_LINE_H; y < OLED_HEIGHT; y++) {
        for (int x = 0; x < OLED_WIDTH; x++) assert(!lit(s_oled_fb, x, y));
    }
    puts("ok");
    return 0;
}
"""


def _between(source: str, start: str, end: str) -> str:
    i = source.index(start)
    return source[i:source.index(end, i)]


class LinkOledStatusTest(unittest.TestCase):
    def test_each_backend_defines_its_hw_functions_once(self) -> None:
        source = LED_C.read_text()
        # Only the #if structure matters here: drop includes and the pin
        # checks that need real chip headers.
        body = "\n".join(line for line in source.splitlines()
                         if not line.lstrip().startswith(("#include", "#error")))
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "led_status_structure.c"
            src.write_text(body)
            for backend, extra in BACKENDS.items():
                defines = [f"-DCONFIG_HOMEHUB_LED_BACKEND_{backend}=1"]
                defines += [f"-D{name}=1" for name in extra]
                out = subprocess.run([*CC, "-E", "-P", *defines, str(src)],
                                     capture_output=True, text=True)
                self.assertEqual(out.returncode, 0, out.stderr)
                for name, pattern in HW_FUNCS.items():
                    count = len(re.findall(pattern, out.stdout))
                    self.assertEqual(count, 1, f"{backend}: {name} defined {count} times")

    def test_oled_drawing_and_writes(self) -> None:
        source = LED_C.read_text()
        defs = _between(source, "// ESP32-C3 with a 0.42 inch SSD1306 OLED",
                        "#elif CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789")
        impl = _between(source, "// The LED task does all the drawing",
                        "static bool led_hw_init(void) {")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "oled_defs.inc").write_text(defs)
            (out / "oled_impl.inc").write_text(impl)
            harness = out / "oled_harness.c"
            harness.write_text(HARNESS)
            exe = out / "oled"
            cmd = [*CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-I", str(out), "-I", str(ROOT / "main"), str(harness),
                   str(ROOT / "main" / "pixel_font.c"), "-o", str(exe)]
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_board_wiring(self) -> None:
        overlay = (ROOT / "devices" / "sdkconfig.c3-oled-042").read_text()
        for setting in ('CONFIG_IDF_TARGET="esp32c3"',
                        "CONFIG_HOMEHUB_LED_BACKEND_C3_SSD1306=y",
                        "CONFIG_HOMEHUB_BUTTON_GPIO=9",
                        "CONFIG_SPIRAM=n",
                        "CONFIG_HOMEHUB_TUNNEL=n",
                        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_c3_4mb.csv"'):
            self.assertIn(setting, overlay)
        # The agent's name needs GET /identity, which only runs when this is on.
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text()
        name = kconfig[kconfig.index("config HOMEHUB_SHOW_AGENT_NAME"):]
        self.assertIn("default y if HOMEHUB_DISPLAY || HOMEHUB_LED_BACKEND_C3_SSD1306",
                      name[:name.index("help")])
        helper = (ROOT / "tools" / "board.sh").read_text()
        self.assertRegex(helper, r"\n  c3-oled-042\)\n    TARGET=esp32c3\n")
        workflow = (ROOT.parent / ".github" / "workflows" / "esp32.yml").read_text()
        self.assertIn('overlays: ";devices/sdkconfig.c3-oled-042"', workflow)

    def test_helper_parses(self) -> None:
        helper = ROOT / "tools" / "board.sh"
        result = subprocess.run(["bash", "-n", str(helper)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        usage = subprocess.run(["bash", str(helper), "--help"], capture_output=True, text=True)
        self.assertEqual(usage.returncode, 0, usage.stderr)
        self.assertIn("c3-oled-042", usage.stdout)
        self.assertIn("waveshare-s3-epaper-154g", usage.stdout)


if __name__ == "__main__":
    unittest.main()
