# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

"""Run the 1.32 board's touch, panel and battery paths against host fakes.

These exercise the actual C functions, not hardware; firmware compilation and
on-board orientation/audio/power checks are separate acceptance steps.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "components/muse/boards/board_waveshare_s3_132.c"


def function(source, name):
    match = re.search(r"^static (?:void|esp_err_t) " + name + r"\([^)]*\)\n\{", source, re.M)
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define TAG "test"
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { int err_ = (expr); if (err_) return err_; } while (0)
#define pdMS_TO_TICKS(n) (n)
typedef int esp_err_t;
'''


class WaveshareS3132(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.board = BOARD.read_text()

    def run_c(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "board_test.c"
            binary = Path(tmp) / "board_test"
            src.write_text(COMMON + source)
            cc = shlex.split(os.environ.get("CC", "cc"))
            build = subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                         str(src), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_touch_snapshot_and_invalid_frames(self):
        code = r'''
#define LCD_H_RES 466
#define LCD_V_RES 466
#define TOUCH_REG_POINTS 0x02
#define LV_INDEV_STATE_RELEASED 0
#define LV_INDEV_STATE_PRESSED 1
typedef int lv_coord_t;
typedef void lv_indev_t;
typedef struct { int state; struct { int x, y; } point; } lv_indev_data_t;
static uint8_t frame[5];
static int failed, reads;
static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    assert(reg == 2 && len == 5); ++reads;
    memcpy(buf, frame, 5); return failed ? -7 : ESP_OK;
}
''' + function(self.board, "touch_read_cb") + r'''
static lv_indev_data_t sample(int points, int event, int x, int y) {
    frame[0] = points;
    frame[1] = (event << 6) | (x >> 8); frame[2] = x;
    frame[3] = y >> 8; frame[4] = y;
    lv_indev_data_t d = { .state = LV_INDEV_STATE_PRESSED };
    reads = 0; touch_read_cb(NULL, &d); assert(reads == 1); return d;
}
int main(void) {
    lv_indev_data_t d = sample(1, 0, 100, 200);
    assert(d.state == LV_INDEV_STATE_PRESSED && d.point.x == 366 && d.point.y == 266);
    d = sample(1, 2, 0, 466);
    assert(d.state == LV_INDEV_STATE_PRESSED && d.point.x == 465 && d.point.y == 0);
    /* Every valid raw pixel stays inside LVGL's 0..465 domain. */
    for (int i = 0; i <= 466; ++i) {
        d = sample(1, 2, i, 466 - i);
        assert(d.state == LV_INDEV_STATE_PRESSED);
        assert(d.point.x >= 0 && d.point.x < 466 && d.point.y >= 0 && d.point.y < 466);
    }
    assert(sample(0, 0, 100, 200).state == LV_INDEV_STATE_RELEASED);
    assert(sample(2, 0, 100, 200).state == LV_INDEV_STATE_RELEASED);
    assert(sample(1, 1, 100, 200).state == LV_INDEV_STATE_RELEASED);
    assert(sample(1, 3, 100, 200).state == LV_INDEV_STATE_RELEASED);
    assert(sample(1, 0, 467, 100).state == LV_INDEV_STATE_RELEASED);
    assert(sample(1, 0, 100, 4095).state == LV_INDEV_STATE_RELEASED);
    failed = 1;
    assert(sample(1, 0, 100, 200).state == LV_INDEV_STATE_RELEASED);
    return 0;
}
'''
        self.run_c(code)

    def test_panel_commands_are_serialized_and_sleep_settles(self):
        code = r'''
static void *s_io = (void *)1, *s_panel = (void *)2;
static bool on_sender;
static int delay_ms, commands, level, display_off;
static uint32_t last_cmd;
static void muse_lcd_bands_run(void (*fn)(void *), void *arg) {
    assert(!on_sender); on_sender = true; fn(arg); on_sender = false;
}
static esp_err_t esp_lcd_panel_io_tx_param(void *io, uint32_t cmd, const void *data, size_t len) {
    assert(on_sender && io == s_io); last_cmd = cmd; ++commands;
    if (cmd == 0x02005100) { assert(len == 1); level = *(uint8_t *)data; }
    else { assert(len == 0 && data == NULL); }
    return ESP_OK;
}
static esp_err_t esp_lcd_panel_disp_on_off(void *panel, bool on) {
    assert(on_sender && panel == s_panel && !on); ++display_off; return ESP_OK;
}
static void vTaskDelay(int ms) { assert(!on_sender); delay_ms = ms; }
''' + "\n".join(function(self.board, name) for name in (
            "send_brightness", "set_brightness", "send_sleep", "panel_sleep", "send_display_off")) + r'''
int main(void) {
    panel_sleep(true); assert(last_cmd == 0x02001000 && delay_ms == 120);
    panel_sleep(false); assert(last_cmd == 0x02001100 && delay_ms == 120 && commands == 2);
    set_brightness(-1); assert(level == 0);
    set_brightness(50); assert(level == 127);
    set_brightness(100); assert(level == 255);
    set_brightness(200); assert(level == 255);
    muse_lcd_bands_run(send_display_off, NULL); assert(display_off == 1);
    return 0;
}
'''
        self.run_c(code)

    def test_battery_calibration_and_errors(self):
        code = r'''
#define VBAT_ADC 3
typedef struct { int battery_pct, battery_mv; bool charging, usb; } muse_power_t;
static bool s_adc_calibrated, host_connected;
static void *s_adc, *s_adc_cali;
static int adc_error, cali_error, adc_reads, calibrated_mv;
static bool usb_serial_jtag_is_connected(void) { return host_connected; }
static esp_err_t adc_oneshot_read(void *adc, int channel, int *raw) {
    assert(adc == s_adc && channel == VBAT_ADC); ++adc_reads; *raw = 2048; return adc_error;
}
static esp_err_t adc_cali_raw_to_voltage(void *cali, int raw, int *mv) {
    assert(cali == s_adc_cali && raw == 2048); *mv = calibrated_mv; return cali_error;
}
''' + function(self.board, "read_power") + r'''
int main(void) {
    muse_power_t p = { 100, 4200, true, false };
    host_connected = true;
    assert(read_power(&p) == ESP_OK);
    assert(p.battery_mv == 0 && p.battery_pct == -1 && p.usb && !p.charging && adc_reads == 0);
    /* USB can drive the charger's VBAT node with no battery connected.
     * Even a calibrated reading must not be presented as a cell level. */
    s_adc_calibrated = true; calibrated_mv = 2086;
    assert(read_power(&p) == ESP_OK);
    assert(p.battery_mv == 0 && p.battery_pct == -1 && p.usb && !p.charging && adc_reads == 0);
    host_connected = false; calibrated_mv = 1875;
    assert(read_power(&p) == ESP_OK);
    assert(p.battery_mv == 3750 && p.battery_pct == 50 && !p.usb && !p.charging);
    /* Reconnecting the host clears an earlier battery measurement, and
     * irrelevant ADC failures cannot hide the new USB-powered state. */
    host_connected = true; adc_error = -7;
    int reads_before_usb = adc_reads;
    assert(read_power(&p) == ESP_OK);
    assert(p.battery_mv == 0 && p.battery_pct == -1 && p.usb && adc_reads == reads_before_usb);
    host_connected = false; adc_error = 0; calibrated_mv = 1650;
    assert(read_power(&p) == ESP_OK && p.battery_pct == 0 && !p.usb);
    calibrated_mv = 2100;
    assert(read_power(&p) == ESP_OK && p.battery_pct == 100 && p.battery_mv == 4200);
    calibrated_mv = 2200;
    assert(read_power(&p) == ESP_OK && p.battery_pct == 100);
    adc_error = -7;
    assert(read_power(&p) == -7 && p.battery_mv == 0 && p.battery_pct == -1);
    adc_error = 0; cali_error = -8;
    assert(read_power(&p) == -8 && p.battery_mv == 0 && p.battery_pct == -1);
    return 0;
}
'''
        self.run_c(code)

    def test_power_off_names_the_button_that_wakes_it(self):
        # muse_menu.c tells the user to press wake_button (else aux_button)
        # to turn the board back on; power_off arms ext0 wake on one GPIO.
        wake_pin = re.search(r"esp_sleep_enable_ext0_wakeup\((\w+), 0\)", self.board).group(1)
        self.assertEqual(wake_pin, "PWR_BUTTON")
        self.assertRegex(self.board, r'\.wake_button = "pwr",')
        self.assertRegex(self.board, r'\.aux_button = "boot",')

    def test_power_off_keeps_wake_pull_up_and_cuts_rail_after_release(self):
        code = r'''
#define PWR_BUTTON 17
#define VBAT_ENABLE 18
#define AUDIO_PA 46
enum { EXT0 = 1, PULLUP, NOPULLDOWN, DARK, RELEASED, PA_OFF, RAIL_OFF, SLEEP };
static int events[16], n, presses_left = 3, pullup, rail = 1;
static void ev(int e) { assert(n < 16); events[n++] = e; }
static esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level) {
    assert(gpio == PWR_BUTTON && level == 0); ev(EXT0); return ESP_OK;
}
static void rtc_gpio_pullup_en(int gpio) { assert(gpio == PWR_BUTTON); pullup = 1; ev(PULLUP); }
static void rtc_gpio_pulldown_dis(int gpio) { assert(gpio == PWR_BUTTON); ev(NOPULLDOWN); }
static void set_brightness(int pct) { assert(pct == 0); }
static void esp_lv_adapter_pause(int ms) { (void)ms; }
static void send_display_off(void *arg) { (void)arg; }
static void muse_lcd_bands_run(void (*fn)(void *), void *arg) { fn(arg); }
static void panel_sleep(bool sleep) { assert(sleep); ev(DARK); }
static int gpio_get_level(int gpio) {
    assert(gpio == PWR_BUTTON && rail);
    if (presses_left == 0) { ev(RELEASED); return 1; }
    --presses_left; return 0;
}
static void vTaskDelay(int ms) { (void)ms; }
static void gpio_set_level(int gpio, int level) {
    if (gpio == AUDIO_PA) { assert(level == 0); ev(PA_OFF); return; }
    assert(gpio == VBAT_ENABLE && level == 0); rail = 0; ev(RAIL_OFF);
}
static void esp_deep_sleep_start(void) { ev(SLEEP); }
''' + function(self.board, "power_off") + r'''
int main(void) {
    assert(power_off() == ESP_FAIL);
    int want[] = { EXT0, PULLUP, NOPULLDOWN, DARK, RELEASED, PA_OFF, RAIL_OFF, SLEEP };
    assert(n == (int)(sizeof(want) / sizeof(want[0])));
    for (int i = 0; i < n; ++i) assert(events[i] == want[i]);
    assert(pullup && !rail);
    return 0;
}
'''
        self.run_c(code)


if __name__ == "__main__":
    unittest.main()
