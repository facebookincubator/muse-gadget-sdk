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
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "muse_lvgl_hold.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_audio_dma.h"
#include "muse_board.h"
#include "muse_settings.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"
#if CONFIG_MUSE_MOTION_GESTURES
#include "qmi8658_motion.h"

static esp_err_t motion_enable(bool enable)
{
    return muse_qmi8658_enable(bsp_i2c_get_handle(), enable);
}
#endif

static const char *TAG = "board";

#define DRAW_BUF_LINES 118      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_TE GPIO_NUM_13      /* the panel's tearing effect, on both boards */
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

/* Charge dark (muse_charge_dark.h): the AXP2101 says whether USB or the
 * PWR key turned the board on; BOOT (GPIO0) held counts as a key. */
static esp_err_t early_power(muse_cd_reading_t *out)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    ESP_RETURN_ON_ERROR(muse_pmu_early(bsp_i2c_get_handle(), out), TAG, "pmu");
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_pullup_en(GPIO_NUM_0);
    out->by_key |= gpio_get_level(GPIO_NUM_0) == 0;
    return ESP_OK;
}

/* SAFE mode's key: BOOT (GPIO0), read raw: SAFE skips init(). */
static bool safe_key(void)
{
    static bool set;
    if (!set) {
        set = true;
        gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
        gpio_pullup_en(GPIO_NUM_0);
    }
    return gpio_get_level(GPIO_NUM_0) == 0;
}

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
#if !CONFIG_MUSE_BOARD_WAVESHARE_S3_175
    /* PWR talks: the 10 s hold-off back on, whatever a reset in a held talk
     * left (a 16 s shutdown cleared then stays off, the datasheet default). */
    (void)muse_pmu_set_key_shutdown(true);   /* logs if it can't */
#endif
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
    muse_lcd_bands_set_gap(6, 0);   /* the BSP's panel gap, for video's direct writes */
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
    /* Tearing effect on (V-blank only), as the 1.75C's BSP already asks. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x35 << 8), (uint8_t[]){ 0x00 }, 1);
    muse_lcd_bands_set_te(LCD_TE);

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
        muse_lvgl_hold(MUSE_LVGL_HOLD_SCREEN, true);   /* shared with video */
        esp_lcd_panel_io_tx_param(s_tp->io, 0xD1, (uint8_t[]){ 0x05 }, 1);
    } else {
        gpio_set_level(BSP_LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(BSP_LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));   /* as the driver waits after its reset */
        muse_lvgl_hold(MUSE_LVGL_HOLD_SCREEN, false);   /* runs again unless video holds it */
    }
}

/*
 * ES8311 plays and the ES7210 records (MIC1 and MIC2, two slots of plain
 * I2S: with fewer than three mics esp_codec_dev keeps it out of TDM), on one
 * duplex I2S bus with MCLK. The board makes the channels itself rather than
 * through bsp_audio_init(), which takes no channel config and so leaves
 * ESP-IDF's 6 x 240 frames of DMA (90 ms): live reply audio wants
 * 3 x 160 (30 ms each way, muse_audio_dma.h). The codecs are set up as the
 * BSP's bsp_audio_codec_speaker_init() / _microphone_init() do, and the
 * channels in the format muse_audio opens them in (16-bit stereo at 16 kHz),
 * so opening never reallocates their buffers.
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    muse_audio_attach_tx(tx);   /* a barge-in clears its DMA (muse_audio_tx.h); before enable, or ESP-IDF refuses */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    audio_codec_i2s_cfg_t i2s_cfg = { .port = CONFIG_BSP_I2S_NUM, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    audio_codec_i2c_cfg_t adc_i2c = { .port = BSP_I2C_NUM, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(i2c && data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t es7210_cfg = { .ctrl_if = adc_ctrl };   /* MIC1 and MIC2, as the BSP's */
    const audio_codec_if_t *adc = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
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
    unsigned pwr = muse_gpio_button_poll(&s_pwr);
    /* A talk can outlast the PMU's hold-off: held, PWR mustn't power off. */
    if (pwr & MUSE_BTN_TALK_PRESS) {
        (void)muse_pmu_set_key_shutdown(false);   /* logs if it can't */
    }
    if (pwr & MUSE_BTN_TALK_RELEASE) {
        (void)muse_pmu_set_key_shutdown(true);    /* logs if it can't */
    }
    return pwr | muse_gpio_button_poll(&s_boot) << 2;   /* BOOT is aux */
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_pwr, &s_boot }, 2, timeout_ms);
}
#endif

/* SAFE mode (muse_board_t.safe_display_off): the AMOLED held in reset (it drops its frame and stays dark; no backlight). */
static void safe_display_off(void)
{
    muse_board_pin_hold(BSP_LCD_RST, 0);   /* GPIO1 on the 1.75C, GPIO39 on the 1.75 (their BSPs) */
}

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
    .safe_display_off = safe_display_off,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .draw_frame = muse_lcd_bands_frame,
    .draw_windows = muse_lcd_bands_windows,   /* only what changed of a streamed picture */
    .panel_writes = muse_lcd_bands_writes,
    .panel_stack_free = muse_lcd_bands_stack_free,
    .audio_init = audio_init,
    .i2s_dma_desc = MUSE_AUDIO_DMA_LIVE_DESC,   /* the channels as audio_init makes them */
    .i2s_dma_frames = MUSE_AUDIO_DMA_LIVE_FRAMES,
    .i2s_ready_at_init = true,   /* audio_init: 16-bit stereo, the DMA sizes above, as muse_audio opens */
    /* set_mic_gain's 100 (ES7210): the tuned gain, 30 dB, until this board's
     * 30 cm speech clip check is measured; then raised to the measured ceiling. */
    .mic_gain_speech_max_db = MUSE_MIC_GAIN_TUNED,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .safe_key = safe_key,
#if !CONFIG_MUSE_BOARD_WAVESHARE_S3_175
    .wait_buttons = wait_buttons,   /* the 1.75's PWR is on the PMU, so it's polled */
#endif
    .read_power = muse_pmu_read_power,
#if CONFIG_MUSE_MOTION_GESTURES
    .motion_enable = motion_enable,
    .motion_read = muse_qmi8658_read,
#endif
    .charged = MUSE_CHARGED_DONE,   /* the AXP2101's charge-done state */
    .power_off = muse_pmu_power_off,
    /* Low-battery shutdown: 3.30 V, as the C6 1.8 with the same
     * AXP2101, well above its own 2.6 V VSYS power-off. Not yet measured
     * against a meter: devices/README.md, Low battery. One low-cell level
     * for the board: the charge-dark hold boots 300 mV above it on
     * charge. */
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
