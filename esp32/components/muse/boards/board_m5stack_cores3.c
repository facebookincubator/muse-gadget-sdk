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
 * M5Stack CoreS3 (K128). Pin map, power rails, LCD, touch and codecs come from
 * Espressif's BSP: https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3
 * 320x240 ILI9342C LCD, FT6336U touch, AW88298 speaker, ES7210 dual mic.
 * The AXP2101 PMU powers the LCD backlight, codecs and camera through its LDOs
 * (the BSP switches them on as each part starts) and reports the battery.
 * There is no user button besides PWR, which only the PMU sees: it is talk,
 * read from the PMU's key IRQ latches. The touchscreen runs the menus.
 */
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_audio.h"
#include "muse_mem.h"
#include "muse_pmu.h"

#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */

static const char *TAG = "board";
static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_pmu;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    /* Only the PMU sees PWR: latch its edges for poll_buttons(). Leave its
     * rails alone; the BSP owns them. */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): no talk button or battery status", esp_err_to_name(err));
        return ESP_OK;
    }
    s_pmu = true;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* Two 6.4 KB internal DMA buffers, not the BSP's default full-height one.
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
    *spk = s_spk = bsp_audio_codec_speaker_init();
    *mic = s_mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    if (!s_pmu || tick++ % PMU_KEY_EVERY) {
        return 0;
    }
    unsigned key = muse_pmu_poll_key();
    return (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_TALK_PRESS : 0) |
           (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_TALK_RELEASE : 0);
}

static esp_err_t read_power(muse_power_t *out)
{
    return s_pmu ? muse_pmu_read_power(out) : ESP_ERR_INVALID_STATE;
}

static esp_err_t power_off(void)
{
    ESP_RETURN_ON_FALSE(s_pmu, ESP_ERR_INVALID_STATE, TAG, "no PMU");
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
    /* PWR turns it back on. */
    return muse_pmu_power_off();
}

static const muse_board_t s_board = {
    .name = "M5Stack CoreS3",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "power",
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
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
