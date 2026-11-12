/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "boards/muse_power_test_board.h"

typedef struct { bool usb, charging; int battery_mv; } muse_power_t;
typedef struct {
    esp_err_t (*init)(void), (*mic_init)(void), (*mic_power)(bool);
    esp_err_t (*mic_read)(int16_t *, size_t);
    esp_err_t (*read_power)(muse_power_t *);
} muse_board_t;
enum { GPIO_NUM_9 = 9, GPIO_NUM_38 = 38 };
static bool rail, usb = true, hw_mic_on;
static int usb_after_gauge = -1;
static int init_error, display_error, mic_init_error, enable_error, disable_error, read_error, rail_error;
static unsigned mic_inits, gauge_reads, panel_frames;
static bool panel_on, panel_fast;
static int panel_start_error, panel_service_error, panel_stop_error;
static esp_err_t board_init(void) { rail = false; return init_error; }
static esp_err_t mic_init(void) { mic_inits++; return mic_init_error; }
static esp_err_t mic_power(bool on) {
    if (on) { rail = true; hw_mic_on = true; return enable_error; }
    if (disable_error) { return disable_error; }
    rail = hw_mic_on = false; return ESP_OK;
}
static esp_err_t mic_read(int16_t *pcm, size_t n) {
    assert(hw_mic_on && rail); memset(pcm, 0, n * sizeof(*pcm)); return read_error;
}
static esp_err_t power(muse_power_t *out) {
    gauge_reads++; out->usb = usb; out->charging = usb; out->battery_mv = 4200;
    if (usb_after_gauge >= 0) { usb = usb_after_gauge; }
    return ESP_OK;
}
static const muse_board_t board = {board_init, mic_init, mic_power, mic_read, power};
static const muse_board_t *muse_board_get(void) { return &board; }
static esp_err_t gpio_set_level(int pin, int level) {
    assert(pin == GPIO_NUM_38); if (rail_error) { return rail_error; } rail = level; return ESP_OK;
}
static int gpio_get_level(int pin) { return pin == GPIO_NUM_9 ? usb : rail; }
static esp_err_t gpio_input_enable(int pin) { assert(pin == GPIO_NUM_38); return ESP_OK; }
esp_err_t muse_sticky_ptest_display_init(void) { return display_error; }
esp_err_t muse_sticky_ptest_display_start(bool fast) { panel_on = true; panel_fast = fast; panel_frames = 0; return panel_start_error; }
esp_err_t muse_sticky_ptest_display_service(void) { assert(panel_on); panel_frames++; return panel_service_error; }
esp_err_t muse_sticky_ptest_display_stop(void) { panel_on = false; return panel_stop_error; }
uint32_t muse_sticky_ptest_display_frames(void) { return panel_frames; }
#include "../components/muse/boards/board_reterminal_sticky_power_test.c"

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    if (!strcmp(scenario, "init_failure")) { init_error = ESP_FAIL; }
    if (!strcmp(scenario, "display_failure")) { display_error = ESP_FAIL; }
    if (!strcmp(scenario, "init_stop_failure")) { rail_error = ESP_FAIL; }
    esp_err_t e = muse_ptest_board_init();
    if (init_error || display_error || rail_error) {
        assert(e == ESP_FAIL && !rail);
        assert(muse_ptest_board_apply(&s_matrix[0]) == ESP_ERR_INVALID_STATE);
        muse_ptest_readback_t out = {0};
        assert(muse_ptest_board_power(&out) == ESP_ERR_INVALID_STATE);
        return 0;
    }
    assert(e == ESP_OK && !rail);
    if (!strcmp(scenario, "panel_round_trip")) {
        for (unsigned row = 7; row <= 9; row += 2) {
            assert(s_matrix[row].load == MUSE_PTEST_EPAPER);
            assert(muse_ptest_board_apply(&s_matrix[row]) == ESP_OK && panel_on && !rail);
            assert(panel_fast == (row == 9));
            assert(muse_ptest_board_service(MUSE_PTEST_EPAPER) == ESP_OK);
            muse_ptest_readback_t r = {0};
            assert(muse_ptest_board_readback(&r) == ESP_OK && r.io_frames == 1);
            assert(muse_ptest_board_apply(&s_matrix[row+1]) == ESP_OK && !panel_on);
            assert(muse_ptest_board_readback(&r) == ESP_OK && !r.io_frames);
        }
        return 0;
    }
    if (!strcmp(scenario, "panel_start_failure")) {
        panel_start_error = ESP_FAIL;
        assert(muse_ptest_board_apply(&s_matrix[7]) == ESP_FAIL && !panel_on && !rail);
        return 0;
    }
    if (!strcmp(scenario, "panel_service_failure")) {
        assert(muse_ptest_board_apply(&s_matrix[7]) == ESP_OK);
        panel_service_error = ESP_ERR_TIMEOUT;
        assert(muse_ptest_board_service(MUSE_PTEST_EPAPER) == ESP_ERR_TIMEOUT);
        assert(muse_ptest_board_stop() == ESP_OK && !panel_on && !rail);
        return 0;
    }
    if (!strcmp(scenario, "panel_stop_failure")) {
        panel_stop_error = ESP_ERR_TIMEOUT;
        assert(muse_ptest_board_stop() == ESP_ERR_TIMEOUT && !rail);
        return 0;
    }
    gauge_reads = 0;
    size_t count = 999;
    const muse_ptest_state_t *matrix = muse_ptest_board_matrix("peripheral", &count);
    assert(matrix && count == 19 && matrix[count - 1].load == MUSE_PTEST_IDLE);
    assert(!muse_ptest_board_matrix("sleep", &count) && count == 0);
    const muse_ptest_state_t *mic = &s_matrix[15], *off = &s_matrix[16];
    muse_ptest_readback_t out = {0};
    assert(muse_ptest_board_power(&out) == ESP_OK && out.usb && !gauge_reads);
    usb = false;
    assert(muse_ptest_board_power(&out) == ESP_OK && !out.usb && !out.charging && !gauge_reads);
    if (!strcmp(scenario, "usb_during_gauge")) {
        usb = true; usb_after_gauge = 0;
        assert(muse_ptest_board_readback(&out) == ESP_OK && !out.usb && !out.charging);
        usb_after_gauge = 1;
        assert(muse_ptest_board_readback(&out) == ESP_OK && out.usb && !out.charging);
        usb_after_gauge = -1;
        assert(muse_ptest_board_readback(&out) == ESP_OK && out.usb && out.charging);
        return 0;
    }
    usb = true;
    if (!strcmp(scenario, "init_mic_failure")) {
        mic_init_error = ESP_FAIL;
        assert(muse_ptest_board_apply(mic) == ESP_FAIL && !rail);
        assert(muse_ptest_board_apply(mic) == ESP_FAIL && mic_inits == 1);
        assert(muse_ptest_board_apply(off) == ESP_OK);
        return 0;
    }
    if (!strcmp(scenario, "enable_failure")) {
        enable_error = ESP_FAIL;
        assert(muse_ptest_board_apply(mic) == ESP_FAIL && !rail && !hw_mic_on);
        return 0;
    }
    assert(muse_ptest_board_apply(mic) == ESP_OK && rail);
    if (!strcmp(scenario, "read_failure")) {
        read_error = ESP_ERR_TIMEOUT;
        assert(muse_ptest_board_service(MUSE_PTEST_MIC) == ESP_ERR_TIMEOUT);
        assert(muse_ptest_board_stop() == ESP_OK && !rail);
        return 0;
    }
    assert(muse_ptest_board_service(MUSE_PTEST_MIC) == ESP_OK);
    assert(muse_ptest_board_readback(&out) == ESP_OK && out.io_frames == 160);
    assert(!out.codecs_open && !out.codec_state && out.battery_mv == 4200 && out.charging);
    if (!strcmp(scenario, "rail_drift")) {
        rail = false;
        assert(muse_ptest_board_readback(&out) == ESP_FAIL);
        assert(muse_ptest_board_stop() == ESP_OK);
        return 0;
    }
    if (!strcmp(scenario, "disable_failure")) {
        disable_error = ESP_FAIL;
        assert(muse_ptest_board_stop() == ESP_FAIL && !rail);
        assert(muse_ptest_board_apply(off) == ESP_FAIL && !rail);
        disable_error = 0;
    }
    assert(muse_ptest_board_apply(off) == ESP_OK && !rail && !hw_mic_on);
    assert(muse_ptest_board_readback(&out) == ESP_OK && !out.io_frames);
    assert(muse_ptest_board_apply(mic) == ESP_OK && mic_inits == 1);
    assert(muse_ptest_board_apply(&s_matrix[11]) == ESP_ERR_NOT_SUPPORTED && !rail);
    assert(muse_ptest_board_service(MUSE_PTEST_MIC) == ESP_ERR_INVALID_STATE);
    assert(muse_ptest_board_prepare_deep(false) == ESP_ERR_NOT_SUPPORTED);
    muse_ptest_codec_regs_t regs;
    memset(&regs, 0xff, sizeof(regs)); muse_ptest_board_codec_regs(&regs);
    assert(!regs.dac_mask && !regs.adc_mask && !regs.expected);
    return 0;
}
