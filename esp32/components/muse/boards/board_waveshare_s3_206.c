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
 * Waveshare ESP32-S3-Touch-AMOLED-2.06, schematic V1.0:
 * https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06
 * See Schematic/ESP32-S3-Touch-AMOLED-2.06-Schematic-V1.0.pdf and the
 * waveshare/esp32_s3_touch_amoled_2_06 v2.0.0 BSP for pins and drivers.
 * 410x502 CO5300 QSPI AMOLED (driven as an SH8601, 22-column gap), FT3168
 * touch, ES8311 speaker + ES7210 dual mic, AXP2101 PMU. ESP32-S3R8 (8 MB
 * octal PSRAM) with 32 MB flash. BOOT (Key1, GPIO0) is talk; PWR (Key3)
 * reaches only the PMU, whose IRQ isn't wired to the ESP32, so it's aux and
 * polled over I2C. Both sit on the right edge, BOOT above PWR, as on the C6
 * version of this watch. The vibration motor (GPIO18), RTC, IMU and SD slot
 * are not used.
 */

#include "esp_err.h" /* this BSP's display.h uses esp_err_t without including it */

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define DRAW_BUF_LINES 126 /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (BSP_LCD_H_RES * 8 * 2)

#define PMU_KEY_EVERY 2 /* poll latched PWR edges every 20 ms */

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");

    /* PWR reaches only the PMU: latch its key IRQs and poll them over I2C.
     * Drain the latch; the press that turned the board on may still be held.
     * Rails are left as the PMU boots them: Waveshare's AXP2101 example
     * doesn't switch any, and the schematic feeds the codecs and RTC from
     * ALDOs whose use isn't confirmed on hardware here. */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): PWR button and battery disabled", esp_err_to_name(err));
        return ESP_OK;
    }
    (void)muse_pmu_poll_key();
    return ESP_OK;
}

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/*
 * The BSP's bsp_display_start() draws through esp_lvgl_port from internal
 * buffers whose flushes need fresh DMA memory once Wi-Fi and BLE are up. As
 * on the 2.16, only the panel comes from the BSP; LVGL draws bands in PSRAM
 * and they go out through two small internal buffers (muse_lcd_bands.h).
 */
static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

    /* Resets the panel, runs Waveshare's init table, sets the 22-column gap
     * and turns the display on. */
    esp_lcd_panel_handle_t panel;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (bsp_display_new(&panel_cfg, &panel, &s_io) != ESP_OK) {
        return NULL;
    }

    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    /* Touch reports in panel coordinates, as the BSP maps it at ROTATE_0. */
    if (bsp_touch_new(NULL, &s_tp) != ESP_OK) {
        return NULL;
    }

    const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* CO5300 "write display brightness" (0x51), as the BSP sends it. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the 2.16. Panel
 * (GPIO8) and touch (GPIO9) resets are separate lines here too. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120)); /* settle before the next command */
}

/*
 * Screen off: LVGL stops, so nothing polls the touch controller and the
 * FT3168 drops to its own monitor mode. Its hibernate (0xA5 = 3) would need
 * a reset to leave, which also loses the thresholds the ft5x06 driver
 * writes at init.
 */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
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
    unsigned ev = muse_gpio_button_poll(&s_boot);
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}

static void wait_buttons(int timeout_ms)
{
    /* The PMU's IRQ line isn't wired to the ESP32, so PWR can't wake the chip
     * from light sleep; BOOT can. The PMU handles power on/off itself. */
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-2.06",
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
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1, /* ES7210 MIC1 and MIC2: mix the two microphones */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
