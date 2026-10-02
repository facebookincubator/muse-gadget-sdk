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
 * ESP32-S3-BOX-3, not the original BOX or BOX-Lite. Pin map, LCD/touch
 * revision detection and codecs come from Espressif's BSP:
 * https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3
 * 320x240 LCD, TT21100 or GT911 touch, ES8311 speaker, ES7210 dual mic.
 * BOOT/CONFIG (GPIO0) is talk; the hardware mute switch is not repurposed.
 */
#include "bsp/esp-bsp.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_audio.h"
#include "muse_mem.h"

static const char *TAG = "board";
static muse_gpio_button_t s_boot;
static esp_codec_dev_handle_t s_spk, s_mic;

static esp_err_t init(void)
{
    /* Release the amplifier pin held low by power_off(). */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(BSP_POWER_AMP_IO);
    rtc_gpio_deinit(BSP_BUTTON_CONFIG_IO);
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    return muse_gpio_button_init(&s_boot, BSP_BUTTON_CONFIG_IO);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* Two 6.4 KB internal DMA buffers, not the BSP's default 64 KB buffer.
     * Its LVGL port supplies byte swapping and the panel/touch orientation. */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    cfg.lvgl_port_cfg.task_affinity = MUSE_UI_CORE;
    cfg.lvgl_port_cfg.task_priority = MUSE_UI_PRIORITY;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        return NULL;
    }
    *touch = bsp_display_get_input_dev();
    return *touch ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    /* Muse uses -1 for forever; esp_lvgl_port uses 0. */
    return bsp_display_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Muse records two slots; the BSP's no-argument default is mono. */
    const i2s_std_config_t cfg = {
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
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "duplex audio init");
    *spk = s_spk = bsp_audio_codec_speaker_init();
    *mic = s_mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    db = (db / 3) * 3;   /* ES7210 PGA steps; 33 maps to the 34.5 dB step. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static esp_err_t power_off(void)
{
    /* There is no main-unit power latch. Sleep until BOOT/CONFIG or RESET;
     * dock peripherals can remain powered. Do not reset the LCD, whose
     * reset line is shared with touch. */
    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "backlight off");
    bsp_display_lock(0);
    esp_err_t err = lvgl_port_stop();
    bsp_display_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "display stop");
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    gpio_set_level(BSP_POWER_AMP_IO, 0);
    while (gpio_get_level(BSP_BUTTON_CONFIG_IO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BSP_BUTTON_CONFIG_IO, 0), TAG, "wake button");
    rtc_gpio_pullup_en(BSP_BUTTON_CONFIG_IO);
    rtc_gpio_pulldown_dis(BSP_BUTTON_CONFIG_IO);
    gpio_hold_en(BSP_POWER_AMP_IO);
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Espressif ESP32-S3-BOX-3",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.4f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_TOP_LEFT, 8, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
