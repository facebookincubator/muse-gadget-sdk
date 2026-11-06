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

/*
 * Waveshare ESP32-C6-Touch-AMOLED-2.06, schematic V1.0:
 * https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.06
 * See Schematic/ESP32-C6-Touch-AMOLED-2.06-Schematic-V1.0.pdf and the
 * waveshare/esp32_c6_touch_amoled_2_06 v2.0.0 BSP for pins and drivers.
 * 410x502 QSPI AMOLED, FT3168 touch, ES8311 speaker, ES7210 dual mics,
 * AXP2101 PMU. BOOT is GPIO9; read PWR edges from the PMU.
 * No PSRAM: voice notes use Link's session and replies are text.
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_check.h"
#include "esp_log.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define BOOT_GPIO GPIO_NUM_9
#define PMU_KEY_EVERY 2         /* poll latched PWR edges every 20 ms */

static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    if (muse_pmu_init(bsp_i2c_get_handle(), true) != ESP_OK) {
        ESP_LOGW(TAG, "no PMU: PWR button and battery unavailable");
    }
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    /* One core: drawing a 410x502 frame takes longer than a frame period, so
     * keep LVGL at the bottom or it starves app_main and Wi-Fi setup. */
    port_cfg.task_priority = 1;
    /* BSP v2.0.0 takes its buffer height from Kconfig, ignoring the buffer
     * fields here. The overlay sets CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=8. */
    const bsp_display_cfg_t cfg = { .lvgl_port_cfg = port_cfg };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    *touch = bsp_display_get_input_dev();
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return bsp_display_lock(timeout_ms < 0 ? 0 : timeout_ms);
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Set up I2S the way muse_audio opens it, instead of the BSP's mono 22 kHz default. */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "i2s");
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned ev = muse_gpio_button_poll(&s_boot);
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-C6-Touch-AMOLED-2.06",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.06f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* Both buttons are on the right edge, BOOT above PWR. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -140 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 140 },
    .frame_ms = 50,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,             /* ES7210 MIC1 and MIC2: mix the two microphones */
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
