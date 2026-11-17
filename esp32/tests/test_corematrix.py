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

"""The LED matrix status (main/matrix_status.c) on the M5Stack CoreMatrix's
board support (components/m5stack_corematrix), against a fake I2C bus: the
chips' setup and the bits left alone, the TM1680 frame layout, the status
pictures, image thresholds and bounds, the setup button, and what happens
when the matrix or the power chip doesn't answer."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "components/m5stack_corematrix"

FAKES = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
const char *esp_err_to_name(esp_err_t err);
""",
    "esp_log.h": """#pragma once
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW ESP_LOGE
#define ESP_LOGI ESP_LOGE
""",
    "esp_check.h": """#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t err_ = (x); (void)(tag); if (err_ != ESP_OK) return err_; } while (0)
#define ESP_RETURN_ON_FALSE(c, e, tag, ...) do { (void)(tag); if (!(c)) return (e); } while (0)
""",
    "driver/i2c_master.h": """#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_dev_handle_t;
typedef void *i2c_master_bus_handle_t;
#define I2C_ADDR_BIT_LEN_7 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_NUM_0 0
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
typedef struct { int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
                 struct { bool enable_internal_pullup; } flags; } i2c_master_bus_config_t;
esp_err_t i2c_master_transmit_receive(void *d, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn, int ms);
esp_err_t i2c_master_transmit(void *d, const uint8_t *tx, size_t n, int ms);
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, void **out);
esp_err_t i2c_master_bus_add_device(void *bus, const i2c_device_config_t *c, void **out);
esp_err_t i2c_master_bus_rm_device(void *d);
""",
    "esp_lcd_types.h": """#pragma once
typedef struct esp_lcd_panel_t esp_lcd_panel_t;
typedef esp_lcd_panel_t *esp_lcd_panel_handle_t;
""",
    "esp_lcd_panel_interface.h": """#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#ifndef __containerof
#define __containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
struct esp_lcd_panel_t {
    esp_err_t (*init)(esp_lcd_panel_t *panel);
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *panel, int x0, int y0, int x1, int y1, const void *data);
};
""",
    "esp_lcd_panel_ops.h": """#pragma once
#include "esp_lcd_panel_interface.h"
static inline esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { return p->init(p); }
static inline esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t p) { return p->del(p); }
static inline esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0, int x1, int y1,
                                                  const void *data) {
    if (x0 >= x1 || y0 >= y1) return ESP_ERR_INVALID_ARG;
    return p->draw_bitmap(p, x0, y0, x1, y1, data);
}
""",
    "freertos/FreeRTOS.h": "#pragma once\n#define pdTRUE 1\n#define pdPASS 1\n#define portMAX_DELAY 0xffffffffu\n"
                           "#define pdMS_TO_TICKS(x) (x)\n",
    "freertos/task.h": "#pragma once\nvoid vTaskDelay(unsigned ticks);\n"
                       "int xTaskCreate(void (*f)(void *), const char *name, int stack, void *arg, int prio, void *out);\n",
    "freertos/semphr.h": "#pragma once\ntypedef void *SemaphoreHandle_t;\nvoid *xSemaphoreCreateMutex(void);\n"
                         "int xSemaphoreTake(void *m, unsigned wait);\nvoid xSemaphoreGive(void *m);\n",
}

HARNESS = r"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "matrix_status.c"

static uint8_t pm[256], ioe[256], ram[33];
static bool pm_missing, matrix_missing, fail_reads;
static int starts, frames, held, tasks;
static button_read_cb reader;

void button_set_reader(button_read_cb read) { reader = read; }
void *xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(void *m, unsigned wait) { (void)wait; assert(m && !held); held = 1; return pdTRUE; }
void xSemaphoreGive(void *m) { assert(m && held); held = 0; }
void vTaskDelay(unsigned ticks) { (void)ticks; }
int xTaskCreate(void (*f)(void *), const char *name, int stack, void *arg, int prio, void *out) {
    (void)f; (void)name; (void)stack; (void)arg; (void)prio; (void)out;
    tasks++;
    return pdPASS;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, void **out) {
    assert(c->sda_io_num == 0 && c->scl_io_num == 1);
    *out = (void *)1;
    return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(void *bus, const i2c_device_config_t *c, void **out) {
    assert(bus && c->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    *out = (void *)(uintptr_t)c->device_address;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(void *d) { assert(d); return ESP_OK; }
static uint8_t *regs(void *d) {
    if ((uintptr_t)d == 0x6e) return pm;
    assert((uintptr_t)d == 0x4f);   // the TM1680 is never read
    return ioe;
}
esp_err_t i2c_master_transmit_receive(void *d, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn, int ms) {
    assert(tn == 1 && ms == 50);
    if (fail_reads || ((uintptr_t)d == 0x6e && pm_missing)) return ESP_FAIL;
    memcpy(rx, regs(d) + tx[0], rn);
    return ESP_OK;
}
esp_err_t i2c_master_transmit(void *d, const uint8_t *tx, size_t n, int ms) {
    assert(ms == 50);
    if ((uintptr_t)d == 0x72) {
        if (matrix_missing) return ESP_FAIL;
        if (n == 6) {
            static const uint8_t start[] = {0x80, 0xa4, 0x98, 0x81, 0xb1, 0x83};
            assert(!memcmp(tx, start, 6));
            starts++;
        } else {
            assert(n == 33 && tx[0] == 0x00);
            memcpy(ram, tx, 33);
            frames++;
        }
        return ESP_OK;
    }
    assert(n == 2);
    regs(d)[tx[0]] = tx[1];
    return ESP_OK;
}

// Pixel (x, y) as the TM1680 was sent it (M5GFX Panel_TM1680.inl).
static int lit(int x, int y) {
    int k = x & 7, bit = k < 4 ? k + 4 : k - 4;
    return (ram[1 + 2 * y + (x >> 3)] >> bit) & 1;
}

static int count_lit(void) {
    int n = 0;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) n += lit(x, y);
    return n;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];
    pm_missing = !strcmp(mode, "no-pm1");
    matrix_missing = !strcmp(mode, "no-matrix");
    pm[0x00] = 0x50; pm[0x01] = 0x20; pm[0x12] = 0xff;
    pm[0x09] = 0x55; pm[0x0a] = 0x55; pm[0x16] = 0xff; pm[0x10] = 0xff;
    // IOE1: IO3 high (charge current 180 mA) and IO5/IO6 outputs to keep.
    ioe[0x03] = 0x34; ioe[0x05] = 0x04; ioe[0x13] = 0xff;
    memset(ram, 0xff, sizeof(ram));   // the last image, kept over a restart

    bool ok = led_status_init();
    assert(!held);
    if (pm_missing) {
        assert(!ok && !reader && starts == 0);
        return 0;
    }
    // PM1: awake, no watchdog, G0-G2 GPIO inputs, its other bits unchanged.
    assert(pm[0x09] == 0 && pm[0x0a] == 0 && pm[0x16] == 0xc0 && pm[0x10] == 0xf8);

    // BtnA is the setup button whether or not the matrix works.
    assert(reader);
    bool pressed = true;
    assert(reader(&pressed) && !pressed);
    pm[0x12] = 0xfe;
    assert(reader(&pressed) && pressed);
    pm[0x12] = 0xfd;                        // BtnB isn't the setup button
    assert(reader(&pressed) && !pressed);
    pm[0x12] = 0xff;
    pressed = true;
    fail_reads = true;
    assert(!reader(&pressed) && pressed);   // unread: left alone
    fail_reads = false;

    int w = 0, h = 0;
    if (matrix_missing) {
        assert(!ok && tasks == 0);
        // Matrix power is turned back off; the charge-current pin is kept.
        assert(!(ioe[0x05] & 0x08) && (ioe[0x05] & 0x04));
        static const uint8_t white[2] = {0xff, 0xff};
        assert(!led_status_display_info(&w, &h) && led_status_display_bits() == 0);
        assert(!led_status_draw_rect(0, 0, 1, 1, (const uint16_t *)white) && frames == 0);
        led_status_show_animation();
        return 0;
    }
    assert(ok && tasks == 1);
    // IO4 output, push-pull, high; nothing else on the expander changed.
    assert(ioe[0x03] == 0x3c && ioe[0x05] == 0x0c && ioe[0x13] == 0xf7 && ioe[0x23] == 0);
    // Started once, cleared, then the boot face.
    assert(starts == 1 && frames == 2);
    assert(lit(3, 4) && lit(4, 5) && lit(10, 4) && lit(11, 5) && lit(5, 12) && lit(10, 12));
    assert(!lit(0, 0) && !lit(15, 0) && !lit(0, 15) && !lit(15, 15) && !lit(7, 4));
    led_status_set_state(LED_STATE_VM_OK);

    // Images: 16x16, one bit.
    assert(led_status_display_info(&w, &h) && w == 16 && h == 16 && led_status_display_bits() == 1);
    uint8_t px[16 * 16 * 2];
    memset(px, 0xff, sizeof(px));
    assert(led_status_draw_rect(0, 0, 16, 16, (const uint16_t *)px) && count_lit() == 256);
    // A later part changes only its own pixels.
    static const uint8_t black[2] = {0x00, 0x00}, white[2] = {0xff, 0xff};
    assert(led_status_draw_rect(0, 0, 1, 1, (const uint16_t *)black));
    assert(!lit(0, 0) && lit(1, 0) && lit(0, 1) && count_lit() == 255);
    // Each byte's pixels land where M5's driver puts them.
    led_status_show_animation();
    assert(led_status_draw_rect(4, 0, 1, 1, (const uint16_t *)white) && ram[1] == 0x01 && count_lit() == 1);
    assert(led_status_draw_rect(0, 0, 1, 1, (const uint16_t *)white) && ram[1] == 0x11);
    assert(led_status_draw_rect(8, 0, 1, 1, (const uint16_t *)white) && ram[2] == 0x10);
    assert(led_status_draw_rect(15, 15, 1, 1, (const uint16_t *)white) && ram[32] == 0x08);
    // Brighter than mid-gray lights; darker doesn't.
    static const uint8_t light[2] = {0x84, 0x10}, dark[2] = {0x7b, 0xef};
    assert(led_status_draw_rect(3, 3, 1, 1, (const uint16_t *)dark) && !lit(3, 3));
    assert(led_status_draw_rect(3, 3, 1, 1, (const uint16_t *)light) && lit(3, 3));
    // Outside the matrix changes nothing.
    uint8_t saved[33];
    memcpy(saved, ram, sizeof(saved));
    int before = frames;
    assert(!led_status_draw_rect(16, 0, 1, 1, (const uint16_t *)px));
    assert(!led_status_draw_rect(0, 16, 1, 1, (const uint16_t *)px));
    assert(!led_status_draw_rect(0, 0, 17, 1, (const uint16_t *)px));
    assert(!led_status_draw_rect(1, 0, 16, 1, (const uint16_t *)px));
    assert(!led_status_draw_rect(-1, 0, 1, 1, (const uint16_t *)px));
    assert(!led_status_draw_rect(0, 0, 0x7fffffff, 1, (const uint16_t *)px));
    assert(!led_status_draw_rect(0, 0, 1, 1, NULL));
    assert(frames == before && !memcmp(saved, ram, sizeof(saved)) && !held);
    led_status_draw_done();
    led_status_show_animation();
    assert(!held);

    // The status pictures.
    frame_t face, check, f;
    draw_status(face, LED_STATE_BLE_ADVERTISING, 0);
    draw_status(check, LED_STATE_WS_CONNECTED, 0);
    assert(memcmp(face, check, sizeof(face)) != 0);
    draw_status(f, LED_STATE_PAIRING_CONFIRM_REQUIRED, 0);
    for (int y = 0; y < 16; y++) assert(!f[y][0] && !f[y][1]);   // "!" flashes
    draw_status(f, LED_STATE_PAIRING_CONFIRM_REQUIRED, 1);
    assert(f[3][0] && f[12][0]);
    for (unsigned phase = 0; phase < 16; phase++) {
        draw_status(f, LED_STATE_WIFI_CONNECTING, phase);
        int n = 0;
        for (int y = 0; y < 16; y++) n += __builtin_popcount(f[y][0]) + __builtin_popcount(f[y][1]);
        assert(n == 12);   // three 2x2 dots
    }
    return 0;
}
"""



class CoreMatrixTest(unittest.TestCase):
    def test_status_matrix_and_button(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for name, text in FAKES.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            (tmp / "harness.c").write_text(HARNESS)
            cc = shlex.split(os.environ.get("CC", "cc"))
            result = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-DCONFIG_HOMEHUB_LED_MATRIX=1", "-DCONFIG_HOMEHUB_LED_BACKEND_M5_COREMATRIX=1",
                 "-I", str(tmp), "-I", str(ROOT / "main"), "-I", str(BOARD), "-I", str(BOARD / "include"),
                 str(tmp / "harness.c"), str(BOARD / "m5stack_corematrix.c"), str(BOARD / "tm1680.c"),
                 "-o", str(tmp / "harness")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in ("ok", "no-matrix", "no-pm1"):
                run = subprocess.run([str(tmp / "harness"), mode], capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, f"{mode}\n{run.stderr}")

    def test_profile(self):
        overlay = (ROOT / "devices/sdkconfig.m5stack-corematrix").read_text()
        for line in ('CONFIG_IDF_TARGET="esp32c61"', "CONFIG_HOMEHUB_LED_BACKEND_M5_COREMATRIX=y"):
            self.assertIn(line, overlay)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        self.assertIn("select HOMEHUB_LED_MATRIX", kconfig)
        self.assertIn("default y if HOMEHUB_LED_MATRIX", kconfig.split("config HOMEHUB_DISPLAY_COMMANDS")[1][:200])
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn('if(CONFIG_HOMEHUB_LED_MATRIX)\n    list(APPEND GADGET_SRCS "matrix_status.c")', cmake)
        # main uses the board only in matrix_status.c's start-up.
        for path in (ROOT / "main").glob("*.[ch]*"):
            if path.name != "matrix_status.c":
                text = path.read_text()
                for name in ("m5stack_corematrix.h", "corematrix_", "M5_COREMATRIX"):
                    self.assertNotIn(name, text, path.name)


if __name__ == "__main__":
    unittest.main()
