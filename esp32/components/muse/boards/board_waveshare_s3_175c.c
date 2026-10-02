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
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C: round 466 px CO5300 AMOLED with
 * CST9217 touch, ES8311 speaker + ES7210 dual mic, AXP2101 PMU. The top
 * button (PWR) is wired to the PMU and, through an inverter, to GPIO3; the
 * bottom one is BOOT (GPIO0).
 *
 * The ESP32-S3-Touch-AMOLED-1.75 (CONFIG_MUSE_BOARD_WAVESHARE_S3_175) runs
 * this driver too. Its BSP moves the panel and touch resets to GPIO 39 and 40
 * and MCLK to 42. GPIO 1 to 3 go to its SD slot, so PWR is read from the PMU's
 * key latch, and BOOT talks: held long, PWR makes the PMU cut power.
 */
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define DRAW_BUF_LINES 118      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (BSP_LCD_H_RES * 8 * 2)
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175
#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */
#else
#define PWR_GPIO GPIO_NUM_3    /* high while PWR is held (a BSS138 inverts it) */
#endif

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_boot;
#if !CONFIG_MUSE_BOARD_WAVESHARE_S3_175
static muse_gpio_button_t s_pwr;
#endif

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175
    /* Only the PMU sees PWR: latch its edges for poll_buttons(). */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), true);
#else
    ESP_RETURN_ON_ERROR(muse_gpio_button_init_high(&s_pwr, PWR_GPIO), TAG, "pwr button");
    /* PWR turned the board on and may still be held; don't count that as a press. */
    s_pwr.pressed = gpio_get_level(PWR_GPIO) == 1;
    /* GPIO3 gives the key, so the PMU needn't latch it. Its IRQ line isn't
     * wired to the ESP32. */
    esp_err_t err = muse_pmu_init(bsp_i2c_get_handle(), false);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): battery status disabled", esp_err_to_name(err));
        return ESP_OK;
    }
    /* Only DCDC1 (VCC3V3) and ALDO1 (A3V3, for the codecs) feed anything; the
     * schematic leaves the rest unconnected. Waveshare's AXP2101 example and
     * xiaozhi's board turn them off too. */
    err = muse_pmu_keep_rails(BIT(0), BIT(0));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unused rails left on (%s)", esp_err_to_name(err));
    }
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
 * bsp_display_start(), less its draw buffers: the BSP's are PSRAM, and every
 * flush from them needs a fresh 46 KB internal DMA bounce buffer, which can't
 * be had once Wi-Fi and BLE are up. The bands go out through two fixed 7 KB
 * internal buffers instead; 9 KB ones left 1 KB free while Wi-Fi joined.
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

    const bsp_display_cfg_t touch_cfg = {
        .touch_flags = { .mirror_x = 1, .mirror_y = 1 },
    };
    if (bsp_touch_new(&touch_cfg, &s_tp) != ESP_OK) {
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

/* Plain SLPIN/SLPOUT over the QSPI command path. The driver's own sleep also
 * enters deep standby, whose wake pulses the reset line shared with touch. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/*
 * Screen off: LVGL stops, and the CST9217 goes from scanning to deep sleep
 * (command 0xD105), where it only answers its reset line. That line is touch
 * only; the panel's reset is GPIO 1.
 */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        esp_lcd_panel_io_tx_param(s_tp->io, 0xD1, (uint8_t[]){ 0x05 }, 1);
    } else {
        gpio_set_level(BSP_LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(BSP_LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));   /* as the driver waits after its reset */
        esp_lv_adapter_resume();
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
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

#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175
static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned ev = muse_gpio_button_poll(&s_boot);   /* BOOT talks, PWR is aux */
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}
#else
static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_pwr) | muse_gpio_button_poll(&s_boot) << 2;   /* BOOT is aux */
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_pwr, &s_boot }, 2, timeout_ms);
}
#endif

static const muse_board_t s_board = {
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.75",
#else
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.75C",
#endif
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.75f,
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175
    .talk_button = "boot",
    .aux_button = "pwr",
    /* The same side buttons as the 1.75C, but BOOT (below) talks. */
    .talk_hint = { LV_ALIGN_CENTER, 153, 129 },
    .aux_hint = { LV_ALIGN_CENTER, 153, -129 },
#else
    .talk_button = "top",
    .aux_button = "bottom",
    /* Side buttons: PWR (talk) above BOOT (sleep/off), on the right. */
    .talk_hint = { LV_ALIGN_CENTER, 153, -129 },    /* 40 degrees above/below 3 o'clock */
    .aux_hint = { LV_ALIGN_CENTER, 153, 129 },
#endif
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
#if !CONFIG_MUSE_BOARD_WAVESHARE_S3_175
    .wait_buttons = wait_buttons,   /* the 1.75's PWR is on the PMU, so it's polled */
#endif
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
