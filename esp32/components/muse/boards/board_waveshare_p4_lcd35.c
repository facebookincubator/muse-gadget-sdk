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
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5. Pin map, LCD, touch and codecs come
 * from Waveshare's BSP (waveshare/esp32_p4_wifi6_touch_lcd_3_5):
 * 320x480 ST7796 SPI LCD, FT6336 touch, one ES8311 for speaker and mic.
 * Wi-Fi and BLE run on the onboard ESP32-C6 over SDIO (ESP-Hosted). The
 * AXP2101 reports the battery and sees PWR, which is the aux button; BOOT
 * talks.
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_audio.h"
#include "muse_mem.h"
#include "muse_pmu.h"

#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */

static const char *TAG = "board";
static muse_gpio_button_t s_talk;
static bool s_pmu;
static esp_lcd_panel_handle_t s_panel;
static int s_brightness;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, GPIO_NUM_35), TAG, "BOOT");
    /* Only the PMU sees PWR: latch its edges for poll_buttons(). */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): no PWR button or battery status", esp_err_to_name(err));
        return ESP_OK;
    }
    s_pmu = true;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_core_id = MUSE_UI_CORE;
    cfg.task_priority = MUSE_UI_PRIORITY;
    cfg.task_stack_size = 32 * 1024;
    bsp_lcd_handles_t lcd;
    if (esp_lv_adapter_init(&cfg) != ESP_OK || bsp_display_brightness_init() != ESP_OK
        || bsp_display_new_with_handles(NULL, &lcd) != ESP_OK) {
        return NULL;
    }
    /* A whole frame takes 31 ms over SPI, the limit for a swipe. With two
     * frame buffers LVGL draws the next one meanwhile; the BSP's single
     * 50-row buffer made a swipe wait for both, at 16 FPS. */
    const esp_lv_adapter_display_config_t dc = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
        lcd.panel, lcd.io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
    s_panel = lcd.panel;
    lv_display_t *disp = esp_lv_adapter_register_display(&dc);
    const bsp_touch_config_t tc = { 0 };
    esp_lcd_touch_handle_t tp;
    if (!disp || bsp_touch_new(&tc, &tp) != ESP_OK) {
        return NULL;
    }
    /* Poll it: the FT6336 holds INT low for a whole touch, so interrupt-driven
     * reads got one point per touch and a drag couldn't follow the finger. */
    tp->config.int_gpio_num = GPIO_NUM_NC;
    const esp_lv_adapter_touch_config_t ic = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&ic);
    return *touch && esp_lv_adapter_start() == ESP_OK ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    s_brightness = pct;
    bsp_display_brightness_set(pct);
}

static void panel_sleep(bool sleep)
{
    esp_err_t err = esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel %s: %s", sleep ? "sleep" : "wake", esp_err_to_name(err));
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Muse records two slots; the BSP's no-argument default is 22.05 kHz. */
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
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned edges = muse_gpio_button_poll(&s_talk);
    if (s_pmu && tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        edges |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
                 (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return edges;
}

static esp_err_t read_power(muse_power_t *out)
{
    return s_pmu ? muse_pmu_read_power(out) : ESP_ERR_INVALID_STATE;
}

static esp_err_t power_off(void)
{
    ESP_RETURN_ON_FALSE(s_pmu, ESP_ERR_INVALID_STATE, TAG, "no PMU");
    bsp_display_backlight_off();
    /* PWR turns it back on. */
    esp_err_t err = muse_pmu_power_off();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        err = ESP_FAIL;   /* still running */
    }
    /* Light the screen again for the error: the UI won't, its brightness is unchanged. */
    bsp_display_brightness_set(s_brightness);
    return err;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-P4 3.5",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 3.5f,
    .talk_button = "BOOT",
    .aux_button = "PWR",
    .power_button = "PWR",   /* for Settings: PWR, not BOOT, turns it back on */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -8, 52 },    /* BOOT, on the right edge below the middle */
    .aux_hint = { LV_ALIGN_RIGHT_MID, -8, 162 },    /* PWR, the lowest of the three */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
