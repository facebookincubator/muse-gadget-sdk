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
 * Waveshare ESP32-C6-Touch-AMOLED-1.8: single-core C6 without PSRAM, 368x448
 * AMOLED (CO5300 or SH8601, the BSP detects which) with touch, one ES8311 for
 * speaker and mic, AXP2101 PMU. BOOT (GPIO9) talks; PWR (on the PMU) is the
 * power button, which also turns the board back on.
 * No PSRAM for Muse's own TLS session or MP3, so voice notes go over Home
 * Link's session and replies come back as text (muse_chat_link.c).
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "muse_audio.h"
#include "muse_audio_dma.h"
#include "muse_board.h"
#include "muse_settings.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define BOOT_GPIO GPIO_NUM_9
#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */
#define FLUSH_WAIT_MS 1000      /* a strip is a few ms on the wire */
/*
 * Two draw buffers, LVGL drawing one while the other goes out over QSPI. Each
 * strip costs a walk of the widget tree and a fresh start on every arc, label
 * and image crossing it, so fewer, taller strips draw a frame much faster.
 * Taller buffers trade RAM for fewer walks: 48 lines use about 47 KB more
 * than 16 across both buffers. Keep normal builds at 16 for offline audio;
 * BLE bench builds default to 48, with height configurable in sdkconfig.
 * Buffers remain adapter-owned for their entire lifetime.
 */
#define DRAW_BUF_LINES CONFIG_MUSE_C6_DRAW_BUF_LINES
#define BOOT_BUF_LINES DRAW_BUF_LINES

static muse_gpio_button_t s_boot;

/* Charge dark (muse_charge_dark.h): the AXP2101 says whether USB or the
 * PWR key turned the board on; BOOT held counts as a key. */
static esp_err_t early_power(muse_cd_reading_t *out)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_pmu_early(bsp_i2c_get_handle(), out), TAG, "pmu");
    gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(BOOT_GPIO);
    out->by_key |= gpio_get_level(BOOT_GPIO) == 0;
    return ESP_OK;
}

/* SAFE mode's key: BOOT (GPIO9), the talk key, read raw: SAFE skips init(). */
static bool safe_key(void)
{
    static bool set;
    if (!set) {
        set = true;
        gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT);
        gpio_pullup_en(BOOT_GPIO);
    }
    return gpio_get_level(BOOT_GPIO) == 0;
}

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    if (muse_pmu_init(bsp_i2c_get_handle(), true) != ESP_OK) {
        ESP_LOGW(TAG, "no PMU: PWR button and battery unavailable");
    }
    return ESP_OK;
}

#if CONFIG_MUSE_PERF
#include "muse_perf.h"

/* Timestamps each touch interrupt for mg.perf, then hands it on to the adapter. */
static esp_lcd_touch_interrupt_callback_t s_touch_irq;

static void IRAM_ATTR touch_irq(esp_lcd_touch_handle_t tp)
{
    muse_perf_touch_irq();
    s_touch_irq(tp);
}

static void watch_touch_irq(esp_lcd_touch_handle_t tp)
{
    if (tp && tp->config.interrupt_callback) {
        s_touch_irq = tp->config.interrupt_callback;
        esp_lcd_touch_register_interrupt_callback_with_data(tp, touch_irq, tp->config.user_data);
    }
}
#endif

/*
 * LVGL runs through esp_lv_adapter like the S3 boards: its task on MUSE_UI_CORE
 * (the only core) at MUSE_UI_PRIORITY, beside the audio and BLE tasks. Two
 * changes for one core:
 *
 * - The draw thread renders while LVGL's task waits for it, so it gets the same
 *   priority (LVGL's Kconfig caps CONFIG_LV_DRAW_THREAD_PRIO at 4); below it,
 *   anything at 4 or 5 (BLE's worker, Link) would stall the frame mid-render.
 * - LVGL waits for each QSPI transfer on a semaphore the transfer-done
 *   interrupt gives. Otherwise it spins on a flag, which on one core takes the
 *   CPU from everything below the UI for every strip in flight.
 */
static lv_display_t *s_disp;
static SemaphoreHandle_t s_flush_done;

static bool IRAM_ATTR flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io;
    (void)ev;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    bool yield = esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
    return yield || woken == pdTRUE;
}

/* LVGL may skip flush_wait if the adapter IRQ already cleared flushing.
 * That leaves this binary semaphore's token unconsumed. FLUSH_START runs
 * after the previous transfer was released and BEFORE submitting the next:
 * discard its old token so it cannot release an in-flight future transfer. */
static void flush_start(lv_event_t *e)
{
    (void)e;
    xSemaphoreTake(s_flush_done, 0);
}

/* One give per flush: LVGL calls this only while a flush is outstanding. */
static void flush_wait(lv_display_t *disp)
{
    (void)disp;
    /* Bounded: a lost transfer-done must not hold LVGL's task (and the
     * adapter lock, with input behind it on one core) for good. A late done
     * is discarded by flush_start. */
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(FLUSH_WAIT_MS)) != pdTRUE) {
        static bool logged;
        if (!logged) {
            logged = true;
            ESP_LOGE(TAG, "a flush never finished in %d ms: going on", FLUSH_WAIT_MS);
        }
    }
}

/* Both panels take whole pixel pairs (the BSP's rounder). */
static void round_area(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    TaskHandle_t draw = xTaskGetHandle("swdraw");   /* LVGL's software draw thread */
    if (draw) {
        vTaskPrioritySet(draw, MUSE_UI_PRIORITY);
    }

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    if (bsp_display_new(NULL, &panel, &io) != ESP_OK) {
        return NULL;
    }
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) {
        return NULL;
    }
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = BOOT_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_disp) {
        return NULL;
    }
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_done };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);
    lv_display_set_flush_wait_cb(s_disp, flush_wait);
    lv_display_add_event_cb(s_disp, flush_start, LV_EVENT_FLUSH_START, NULL);
    /* Draw in the panel's byte order, so each strip goes out as drawn instead
     * of being swapped first (the adapter swaps only plain RGB565). */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_add_event_cb(s_disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK) {
        const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, tp);
        *touch = esp_lv_adapter_register_touch(&tp_cfg);
#if CONFIG_MUSE_PERF
        watch_touch_irq(tp);
#endif
    } else {
        ESP_LOGW(TAG, "no touch controller");
    }
    /* OLED brightness is a QSPI command on the same IO as pixel DMA, not
     * an independent PWM. Finish it before the worker can submit a flush:
     * SPI's bus lock does not serialize two tasks using the SAME device. */
    if (bsp_display_brightness_init() != ESP_OK) {
        return NULL;
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    ESP_LOGI(TAG, "C6 display ready: %d-line buffers", BOOT_BUF_LINES);
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    /* The adapter mutex is recursive: safe both from its LVGL timers and
     * from another task. Keep QSPI parameter commands out of pixel submits. */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        bsp_display_brightness_set(pct);
        esp_lv_adapter_unlock();
    }
}

/*
 * One ES8311 plays and records on a duplex I2S bus with MCLK, its amplifier
 * on the TCA9554 expander. The board makes the channels itself rather than
 * through bsp_audio_init(), which takes no channel config and so leaves
 * ESP-IDF's 6 x 240 frames of DMA (90 ms): live reply audio wants
 * 3 x 160 (30 ms each way, muse_audio_dma.h), which also gives the C6, with
 * no PSRAM, about 8 KB of internal DMA memory back. The codec devices are
 * set up as the BSP's bsp_audio_codec_speaker_init() / _microphone_init()
 * do, and the channels in the format muse_audio opens them in (16-bit stereo
 * at 16 kHz), so opening never reallocates their buffers.
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* Live reply audio: 30 ms of DMA each way, not ESP-IDF's 90 (muse_audio_dma.h). */
    chan_cfg.dma_desc_num = MUSE_AUDIO_DMA_LIVE_DESC;
    chan_cfg.dma_frame_num = MUSE_AUDIO_DMA_LIVE_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
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
    const audio_codec_data_if_t *data_if = NULL;
    const audio_codec_ctrl_if_t *spk_ctrl = NULL, *mic_ctrl = NULL;
    const audio_codec_gpio_if_t *gpio_if = NULL;
    const audio_codec_if_t *spk_codec = NULL, *mic_codec = NULL;
    bool tx_on = false, rx_on = false;
    *spk = NULL;
    *mic = NULL;
    esp_err_t ret = ESP_OK;   /* the ESP_GOTO_ON_* macros set it */
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), fail, TAG, "i2s tx");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), fail, TAG, "i2s rx");
    muse_audio_attach_tx(tx);   /* a barge-in clears its DMA (muse_audio_tx.h); before enable, or ESP-IDF refuses */
    ESP_GOTO_ON_ERROR(i2s_channel_enable(tx), fail, TAG, "i2s tx on");
    tx_on = true;
    ESP_GOTO_ON_ERROR(i2s_channel_enable(rx), fail, TAG, "i2s rx on");
    rx_on = true;

    audio_codec_i2s_cfg_t i2s_cfg = { .port = BSP_I2S_NUM, .rx_handle = rx, .tx_handle = tx };
    data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    spk_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    mic_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    gpio_if = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(i2c_cfg.bus_handle && data_if && spk_ctrl && mic_ctrl && gpio_if, ESP_ERR_NO_MEM, fail, TAG,
                      "codec interfaces");

    /* As the BSP: one ES8311 driver for the speaker's device, one for the mic's. */
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = spk_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,     /* the amplifier is on the expander (below) */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    spk_codec = es8311_codec_new(&es_cfg);
    es_cfg.ctrl_if = mic_ctrl;
    es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    mic_codec = es8311_codec_new(&es_cfg);
    ESP_GOTO_ON_FALSE(spk_codec && mic_codec, ESP_FAIL, fail, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = spk_codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = mic_codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_GOTO_ON_FALSE(*spk && *mic, ESP_ERR_NO_MEM, fail, TAG, "codec devices");
    /* The amplifier last: no failure above leaves it on. */
    ESP_GOTO_ON_ERROR(bsp_audio_poweramp_enable(true), fail, TAG, "amplifier");
    return ESP_OK;

fail:
    /* Everything above, in reverse. */
    if (*spk) {
        esp_codec_dev_delete(*spk);
    }
    if (*mic) {
        esp_codec_dev_delete(*mic);
    }
    *spk = NULL;
    *mic = NULL;
    if (mic_codec) {
        audio_codec_delete_codec_if(mic_codec);
    }
    if (spk_codec) {
        audio_codec_delete_codec_if(spk_codec);
    }
    if (gpio_if) {
        audio_codec_delete_gpio_if(gpio_if);
    }
    if (mic_ctrl) {
        audio_codec_delete_ctrl_if(mic_ctrl);
    }
    if (spk_ctrl) {
        audio_codec_delete_ctrl_if(spk_ctrl);
    }
    if (data_if) {
        audio_codec_delete_data_if(data_if);
    }
    if (rx_on) {
        i2s_channel_disable(rx);
    }
    if (tx_on) {
        i2s_channel_disable(tx);
    }
    i2s_del_channel(rx);
    i2s_del_channel(tx);
    return ret;
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

/* SAFE mode (muse_board_t.safe_display_off): the AMOLED held in reset (it drops its frame and stays dark; no backlight). */
static void safe_display_off(void)
{
    /* Its reset is on the TCA9554 (BSP: IO_EXPANDER_PIN_NUM_4): the bus and the expander, nothing more. */
    if (bsp_i2c_init() != ESP_OK) {
        return;
    }
    esp_io_expander_handle_t e = bsp_io_expander_init();
    if (e) {
        esp_io_expander_set_dir(e, BSP_LCD_RST, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(e, BSP_LCD_RST, false);
    }
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-C6-Touch-AMOLED-1.8",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 1.8f,
    .panel_w_mm = 29.0,   /* derived from the 1.8" diagonal at 368:448 */
    .corner_px = 48,      /* the panel's visible corner radius */
    .talk_button = "BOOT",   /* as printed on the board */
    .aux_button = "PWR",
    /* Both buttons are on the right edge, about 100 px from the top and bottom. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -124 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 126 },
    .frame_ms = 50,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .safe_display_off = safe_display_off,
    .audio_init = audio_init,
    .i2s_dma_desc = MUSE_AUDIO_DMA_LIVE_DESC,   /* the channels as audio_init makes them */
    .i2s_dma_frames = MUSE_AUDIO_DMA_LIVE_FRAMES,
    .i2s_ready_at_init = true,   /* audio_init: 16-bit stereo, the DMA sizes above, as muse_audio opens */
    /* set_mic_gain's 100 (ES8311): the tuned gain, 30 dB, until this board's
     * 30 cm speech clip check is measured; then raised to the measured ceiling. */
    .mic_gain_speech_max_db = MUSE_MIC_GAIN_TUNED,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .safe_key = safe_key,
    .read_power = muse_pmu_read_power,
    .charged = MUSE_CHARGED_DONE,   /* the AXP2101's charge-done state */
    .power_off = muse_pmu_power_off,
    /* Low-battery shutdown: 3.30 V, well above the AXP2101's own
     * VSYS power-off (2.6 V by default). Unmeasured: devices/README.md. */
    .battery_shutdown_mv = 3300,
    /* Charge dark below the low-cell level above: a USB power-up on a
     * low cell boots at battery_shutdown_mv + MUSE_CD_CHARGE_MARGIN_MV on
     * charge (3.6 V). */
    .early_power = early_power,
    .early_power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
