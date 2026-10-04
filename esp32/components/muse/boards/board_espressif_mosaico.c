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
 * Espressif ESP-Mosaico: ESP32-S31, square 480x480 CO5300 AMOLED with CST9217
 * touch, and one ES8311 for the speaker and the microphone. Pins, power rails,
 * display, touch and codec come from Espressif's board support package:
 * https://github.com/esp-mosaico/esp-mosaico-bsp (components/esp-mosaico-bsp,
 * pins in include/bsp/esp_mosaico.h).
 * The AI button (GPIO7) talks; BOOT (GPIO61) sleeps, and powers off when held.
 */
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define SHUTDOWN_WAIT_MS 1000   /* how long the supply gets to drop after the shutdown signal */

static muse_gpio_button_t s_ai;
static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_ai, BSP_BUTTON_AI_GPIO), TAG, "AI button");
    return muse_gpio_button_init(&s_boot, BSP_BUTTON_BOOT_GPIO);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* bsp_display_start() starts LVGL's task itself unless it is already
     * there, unpinned and at the adapter's own priority. Muse wants it on the
     * audio core, just below the audio task. */
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_core_id = MUSE_UI_CORE;
    adapter.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter) != ESP_OK) {
        return NULL;
    }
    lv_display_t *disp = bsp_display_start();
    *touch = bsp_display_get_input_dev();
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

/* Display off and on only. bsp_display_off() would also pause LVGL, and this
 * runs in LVGL's own task. */
static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_on_off(bsp_display_get_panel(), !sleep);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* I2S the way muse_audio opens it, instead of the BSP's 48 kHz default. */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = BSP_AUDIO_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "i2s");
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_ai) | muse_gpio_button_poll(&s_boot) << 2;   /* BOOT is aux */
}

static esp_err_t power_off(void)
{
    ESP_RETURN_ON_ERROR(bsp_power_set_shutdown(true), TAG, "shutdown signal");
    vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_WAIT_MS));
    /* Still running, so the board stayed on. Let go of the signal rather than
     * leave a request standing that nobody is waiting for any more. */
    bsp_power_set_shutdown(false);
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Espressif ESP-Mosaico",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    /* diagonal_in is left at 0: the board support package doesn't give the
     * panel's size. Under 2" the settings use the keypad with bigger keys. */
    .talk_button = "AI",
    .aux_button = "boot",
    /* A free spot on the right edge, not beside the button: move it, and add
     * aux_hint, to where the buttons sit on the case. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -124 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    /* No read_power: the BQ27220 gives the charge, but nothing on the board
     * says whether USB power is present, and Muse rests Wi-Fi on battery. */
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
