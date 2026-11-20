# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Exercise Sticky PDM cleanup, failures and readback with the real backend."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class StickyPowerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        (tmp / "muse_board.h").touch()
        cls.binary = tmp / "sticky"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(tmp), "-I", str(ROOT / "tests/power_test_fakes"),
                        "-I", str(ROOT / "components/muse"),
                        str(ROOT / "tests/sticky_power_test_harness.c"), "-o", str(cls.binary)], check=True)

    def test_backend_lifecycle_and_faults(self):
        for scenario in ("round_trip", "init_failure", "display_failure", "init_stop_failure", "init_mic_failure",
                         "enable_failure", "read_failure", "rail_drift", "disable_failure", "usb_during_gauge",
                         "panel_round_trip", "panel_start_failure", "panel_service_failure", "panel_stop_failure"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.binary), scenario], check=True, capture_output=True, timeout=5)

    def test_display_refresh_evidence_and_cleanup(self):
        tmp = Path(self.tmp.name)
        for header in ("FreeInkDisplay.h", "driver/gpio.h", "esp_timer.h",
                       "freertos/FreeRTOS.h", "freertos/task.h"):
            path = tmp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        binary = tmp / "display"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(tmp), "-I", str(ROOT / "tests/power_test_fakes"),
                        "-I", str(ROOT / "components/muse"),
                        str(ROOT / "tests/sticky_power_test_display_harness.cpp"),
                        "-o", str(binary)], check=True)
        for scenario in ("full", "fast", "no_buffer", "no_async", "missing_busy",
                         "stuck_busy", "abort", "blocking_fallback", "seed_failure"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(binary), scenario], check=True, timeout=5)

    def test_uart_and_isolation_overlay(self):
        from test_power_test_config import overlay_settings
        settings = overlay_settings(ROOT / "devices/sdkconfig.reterminal-sticky-power-test")
        self.assertEqual(settings["MUSE_POWER_TEST"], "y")
        self.assertEqual(settings["ESP_CONSOLE_UART_BAUDRATE"], "115200")
        self.assertEqual(settings["USJ_NO_AUTO_LS_ON_CONNECTION"], "n")
        self.assertEqual(settings["MUSE_STICKY_SD"], "n")
        self.assertEqual(settings["ESP_CONSOLE_SECONDARY_NONE"], "y")
        self.assertEqual(settings["ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG"], "n")

    def test_gauge_evidence_requires_valid_reads_and_preserves_signed_current(self):
        source = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        fn = "static esp_err_t read_power" + source.split("static esp_err_t read_power", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
        tmp = Path(self.tmp.name)
        driver = tmp / "gauge.c"
        driver.write_text('''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#define CONFIG_MUSE_POWER_TEST 1
#define GPIO_NUM_9 9
#define ESP_LOGI(...) ((void)0)
typedef struct { bool usb, charging; int battery_pct, battery_mv; } muse_power_t;
static bool s_gauge = true;
static int s_gauge_log_us;
static uint16_t voltage = 4200, percent = 100, current;
static int fail_reg = -1;
static char emitted[96];
static int gpio_get_level(int pin) { (void)pin; return 1; }
static esp_err_t gauge_connect(void) { return ESP_OK; }
static esp_err_t gauge_read(uint8_t reg, uint16_t *out) {
    if (reg == fail_reg) return ESP_FAIL;
    *out = reg == 0x08 ? voltage : reg == 0x2c ? percent : current;
    return ESP_OK;
}
static void muse_ptest_board_console_write(const char *s, size_t n) {
    assert(n < sizeof(emitted)); memcpy(emitted, s, n); emitted[n] = 0;
}
''' + fn + '''
int main(void) {
    muse_power_t out;
    current = (uint16_t)(int16_t)-3;
    assert(read_power(&out) == ESP_OK);
    assert(!strcmp(emitted, "STICKY_GAUGE soc_pct=100 voltage_mv=4200 current_ma=-3\\n"));
    assert(!out.charging && out.battery_pct == 100);
    current = 400;
    assert(read_power(&out) == ESP_OK && out.charging);
    assert(strstr(emitted, "current_ma=400\\n"));
    for (unsigned i = 0; i < 3; ++i) {
        fail_reg = (int[]){0x08, 0x2c, 0x0c}[i]; emitted[0] = 0;
        assert(read_power(&out) == ESP_OK && out.battery_pct == -1 && !emitted[0]);
    }
    fail_reg = -1; percent = 101; emitted[0] = 0;
    assert(read_power(&out) == ESP_OK && out.battery_pct == -1 && !emitted[0]);
    percent = 100; voltage = 0;
    assert(read_power(&out) == ESP_OK && out.battery_pct == -1 && !emitted[0]);
}
''')
        binary = tmp / "gauge"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "tests/power_test_fakes"), str(driver), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)

if __name__ == "__main__":
    unittest.main()
