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
 * Waveshare ESP32-S3-Touch-AMOLED-1.8: ESP32-S3R8 with 16 MB flash, 368x448
 * AMOLED with touch (SH8601 and FT3168, or CO5300 and CST820 on the V2; the
 * BSP detects which), one ES8311 for speaker and mic, AXP2101 PMU, TCA9554 IO
 * expander. BOOT (GPIO0) talks; PWR (on the PMU) is the power button, which
 * also turns the board back on.
 *
 * Pins are the BSP's (waveshare/esp32_s3_touch_amoled_1_8). The expander's
 * part comes from Waveshare's examples (waveshareteam/ESP32-S3-Touch-AMOLED-1.8,
 * examples/arduino/examples/02_Drawing_board) and xiaozhi-esp32's board
 * (main/boards/waveshare/esp32-s3-touch-amoled-1.8): EXIO0-2 are the panel and
 * touch reset and power lines, pulsed low at start.
 */
#include "bsp/esp-bsp.h"    /* first: the BSP's display.h doesn't include esp_err.h itself */
#include "bsp/display.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
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

#define BOOT_GPIO GPIO_NUM_0
#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */
#define DRAW_BUF_LINES 112      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (BSP_LCD_H_RES * 8 * 2)
#define EXIO_RESET (IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_boot;

/* The expander's pins are inputs after power-on, which leaves the panel and
 * touch out of reset, but a restart of the ESP32 alone doesn't reset them. */
static esp_err_t reset_panel_and_touch(void)
{
    esp_io_expander_handle_t exio = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(exio, ESP_FAIL, TAG, "io expander");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(exio, EXIO_RESET, IO_EXPANDER_OUTPUT), TAG, "exio dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(exio, EXIO_RESET, 0), TAG, "exio low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(exio, EXIO_RESET, 1), TAG, "exio high");
    vTaskDelay(pdMS_TO_TICKS(300));   /* the touch controller answers I2C again */
    return ESP_OK;
}

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    ESP_RETURN_ON_ERROR(reset_panel_and_touch(), TAG, "panel reset");
    if (muse_pmu_init(bsp_i2c_get_handle(), true) != ESP_OK) {
        ESP_LOGW(TAG, "no PMU: PWR button and battery unavailable");
    }
    return ESP_OK;
}

/* The SH8601 and CO5300 need even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/*
 * bsp_display_start(), less its draw buffers and esp_lvgl_port: the BSP draws
 * from one 74 KB internal buffer, which Wi-Fi and BLE can't spare. The bands
 * are drawn in PSRAM and go out through two fixed 6 KB internal buffers
 * instead, as on the 1.75C.
 */
static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

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

    /* Finds which touch controller is fitted, and sets the panel's column
     * offset to match: the V2's CO5300 starts 16 columns in. */
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
    /* "Write display brightness" (0x51), as the BSP sends it. */
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

/* Plain SLPIN/SLPOUT over the QSPI command path. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops. The touch controller is left as it is: its reset
 * line is on the expander with the panel's. */
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

/* PWR reaches only the PMU, so it's polled over I2C and there's no
 * wait_buttons: the input task polls every 50 ms while the display is paused. */
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
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.8",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 1.8f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* Both buttons are on the right edge, placed as on the C6 board. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -124 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 126 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
