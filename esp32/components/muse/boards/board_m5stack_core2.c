/*
 * Copyright (c) 2026 Patrick McDowell
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
 * M5Stack Core2 v1.0: ESP32-D0WDQ6-V3 (classic ESP32, 16 MB flash, 8 MB quad
 * PSRAM), 2.0" 320x240 ILI9342C SPI LCD (driven by Espressif's ili9341
 * driver), FT6336U capacitive touch, SPM1423 PDM mic, NS4168 I2S speaker amp,
 * AXP192 PMU, CP2104 or CH9102F USB-UART bridge. Push-to-talk is the middle
 * bottom touch zone (M5's BtnB): holding PWR would cut the power in hardware
 * past ~4 s, so PWR is the aux button instead (tap sleeps, the PMU long-press
 * event requests a clean shutdown) and RST resets the chip. Pins, AXP rail setup
 * and the display/touch init come from Espressif's BSP; AXP192 register use
 * (PEK key, battery, shutdown), the mic pins and the touch-button geometry
 * from M5Unified; the panel, touch and audio wiring cross-checked against
 * M5GFX: github.com/espressif/esp-bsp (bsp/m5stack_core_2),
 * github.com/m5stack/M5Unified, github.com/m5stack/M5GFX.
 *
 * Mic and speaker time-share I2S0: the speaker is standard I2S (BCK 12, WS 0,
 * DATA 2) and the mic is PDM (CLK 0, DATA 34), and the two need different
 * port clocks, so one direction is configured at a time. Push-to-talk is
 * half-duplex anyway (record, then play).
 *
 * Core2 v1.1 units use the AXP2101 PMU and are not supported: the PEK,
 * battery, backlight and shutdown code below is specific to the AXP192.
 */
#include <math.h>

#include "sdkconfig.h"

#if !CONFIG_BSP_PMU_AXP192
#error "The Muse Core2 port requires the Core2 v1.0 AXP192 PMU"
#endif

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_state.h"

static const char *TAG = "board";

#define DRAW_BUF_LINES 10       /* 320x10 px double-buffered, in internal RAM:
                                 * the classic ESP32's SPI DMA can't reach PSRAM */
#define AXP_ADDR 0x34
#define AXP_PEK_STATUS 0x46    /* bit 1: short press, bit 0: long press; write to clear */
#define AXP_BATT_MV_LSB 1.1f   /* 12-bit battery ADC: 1.1 mV per step */

#define MIC_CLK GPIO_NUM_0
#define MIC_DATA GPIO_NUM_34
#define MIC_GAIN_OFFSET_DB 6   /* Muse's default 30 dB lands on M5Unified's x16 */

#define TP_ADDR 0x38           /* FT6336U touch controller, M5GFX's 0x38 */
#define TP_Y_ZONE 240          /* controller rows at/above this are M5's buttons (tb_y) */
#define TP_X_THIRD 107         /* 320/3: BtnB is the middle third (tb_k = 65536*3/320) */

static i2c_master_dev_handle_t s_axp, s_tp;
static bool s_talk_pressed;
static i2s_chan_handle_t s_mic_rx, s_spk_tx;
static int s_dir;              /* I2S0's current direction: 0 none, 1 mic, 2 speaker */
static int s_mic_gain_q8 = 256;
static int32_t s_mic_dc[2];    /* each slot's zero level, x256 */
static bool s_mic_settled;     /* s_mic_dc set from a first read */

static esp_err_t axp_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, buf, len, 1000);
}

/* Read-modify-write one AXP192 register, setting the mask bits to value. */
static esp_err_t axp_write_masked(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur;
    ESP_RETURN_ON_ERROR(axp_read(reg, &cur, 1), TAG, "axp read 0x%02x", reg);
    uint8_t out[2] = { reg, (uint8_t)((cur & ~mask) | (value & mask)) };
    return i2c_master_transmit(s_axp, out, sizeof(out), 1000);
}

/*
 * PEK key events, not the physical held state. The bits latch,
 * so a nonzero read is written back to clear it, as M5Unified's AXP192 driver
 * does. Returns the key bits (short = 2, long = 1), or -1 on error.
 */
static int pek_read(void)
{
    uint8_t val;
    if (axp_read(AXP_PEK_STATUS, &val, 1) != ESP_OK) {
        return -1;
    }
    val &= 0x03;
    if (val) {
        uint8_t clear[2] = { AXP_PEK_STATUS, val };
        if (i2c_master_transmit(s_axp, clear, sizeof(clear), 1000) != ESP_OK) {
            return -1;
        }
    }
    return val;
}

/*
 * M5's BtnB: the middle third of the bottom touch strip. Reads the FT6336
 * directly (TD_STATUS plus the first point) so holding it never depends on
 * what the UI has under it. The strip is outside the LCD, so LVGL ignores it;
 * the shared input layer routes a talk press while the menu is open to
 * Select. Returns 1 when BtnB is held, 0 when not, -1 on error (the caller
 * keeps the previous state).
 */
static int tp_zone(void)
{
    uint8_t reg = 0x02, buf[5];
    if (i2c_master_transmit_receive(s_tp, &reg, 1, buf, sizeof(buf), 100) != ESP_OK) {
        return -1;
    }
    if ((buf[0] & 0x0f) == 0) {
        return 0;
    }
    int x = ((buf[1] & 0x0f) << 8) | buf[2];
    int y = ((buf[3] & 0x0f) << 8) | buf[4];
    if (y < TP_Y_ZONE) {
        return 0;
    }
    return x >= TP_X_THIRD && x < 2 * TP_X_THIRD ? 1 : 0;
}

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");
    const i2c_device_config_t axp_cfg = {
        .device_address = AXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &axp_cfg, &s_axp), TAG,
                        "axp init");
    const i2c_device_config_t tp_cfg = {
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &tp_cfg, &s_tp), TAG,
                        "touch init");
    /* Battery ADC on: the BSP's display init does this too, but power reads
     * must work even if the display never starts. */
    const uint8_t adc_on[2] = { 0x82, 0xff };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_axp, adc_on, sizeof(adc_on), 1000), TAG,
                        "axp adc on");
    /* Enable both PEK event sources, as M5Unified does. Clear any event left
     * by the power-on press; an event bit cannot tell us whether PWR is held. */
    ESP_RETURN_ON_ERROR(axp_write_masked(0x42, 0x03, 0x03), TAG, "axp pek enable");
    ESP_RETURN_ON_ERROR(pek_read() < 0 ? ESP_FAIL : ESP_OK, TAG, "axp pek clear");
    /* A finger resting on the screen through boot counts as already down too.
     * The touch controller may still be unpowered here; a failed read just
     * leaves the talk state clear for the first poll to set. */
    s_talk_pressed = tp_zone() == 1;
    return ESP_OK;
}

static lv_indev_read_cb_t s_touch_read;
static lv_point_t s_touch_point;

/* The FT6336 also reports the capacitive strip below the 240-row LCD.
 * Keep those button coordinates out of LVGL, including on release when
 * the BSP may retain the last point. poll_buttons() handles BtnB separately. */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_touch_read(indev, data);
    if (data->point.x < 0 || data->point.x >= BSP_LCD_H_RES ||
        data->point.y < 0 || data->point.y >= BSP_LCD_V_RES) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = s_touch_point;
    } else {
        s_touch_point = data->point;
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* The BSP's defaults, with a 10-line internal draw buffer: its 50-line
     * one costs 64 KB of internal RAM, which Wi-Fi and BLE need. */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * DRAW_BUF_LINES,
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
    if (!*touch) {
        return NULL;
    }
    bsp_display_lock(0);
    s_touch_read = lv_indev_get_read_cb(*touch);
    lv_indev_set_read_cb(*touch, touch_read);
    bsp_display_unlock();
    return disp;
}

static bool display_lock(int timeout_ms)
{
    /* Muse uses -1 for forever; lvgl_port uses 0. */
    return bsp_display_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

static void set_brightness(int pct)
{
    if (pct <= 0) {
        /* The BSP only turns the backlight rail down, never off: cut DCDC3
         * (the backlight rail; LCD logic is LDO2) explicitly. */
        bsp_display_backlight_off();
        axp_write_masked(0x12, 0x02, 0x00);
        return;
    }
    axp_write_masked(0x12, 0x02, 0x02);
    if (bsp_display_brightness_set(pct) != ESP_OK) {
        ESP_LOGW(TAG, "backlight set failed");
    }
}

/*
 * I2S0 carries the mic (PDM) or the speaker (standard I2S), never both: the
 * two need different port clocks, and one shared duplex clock fits neither
 * (capture trickled, playback ran 4x fast). The voice loop is half-duplex
 * anyway (record, then play), so the port is reconfigured on each direction
 * change: the idle direction's channel is deleted first, which also drops
 * the driver's duplex state and unroutes its pins.
 */
/* Configured direction only; enabling is tracked separately so a codec
 * close (disable) followed by open (enable) re-enables without reconfiguring. */
static bool s_mic_on, s_spk_on;

static esp_err_t use_mic(void)
{
    if (s_dir != 1) {
        if (s_spk_tx) {
            if (s_spk_on) {
                ESP_RETURN_ON_ERROR(i2s_channel_disable(s_spk_tx), TAG, "speaker off");
                s_spk_on = false;
            }
            ESP_RETURN_ON_ERROR(i2s_del_channel(s_spk_tx), TAG, "speaker delete");
            s_spk_tx = NULL;
        }
        if (!s_mic_rx) {
            i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
            chan.auto_clear = true;
            ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_mic_rx), TAG, "mic channel");
            /* Only I2S0 does PDM. The mic's clock is 2.048 MHz, as M5Unified runs it. */
            i2s_pdm_rx_config_t pdm_cfg = {
                .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
                .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                .gpio_cfg = { .clk = MIC_CLK, .din = MIC_DATA },
            };
            pdm_cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
            ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_mic_rx, &pdm_cfg), TAG, "mic pdm");
        }
        s_dir = 1;
        s_mic_settled = false;
    }
    if (!s_mic_on) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_mic_rx), TAG, "mic on");
        s_mic_on = true;
    }
    return ESP_OK;
}

static esp_err_t use_speaker(void)
{
    if (s_dir != 2) {
        if (s_mic_rx) {
            if (s_mic_on) {
                ESP_RETURN_ON_ERROR(i2s_channel_disable(s_mic_rx), TAG, "mic off");
                s_mic_on = false;
            }
            ESP_RETURN_ON_ERROR(i2s_del_channel(s_mic_rx), TAG, "mic delete");
            s_mic_rx = NULL;
        }
        if (!s_spk_tx) {
            i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
            chan.auto_clear = true;
            ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_spk_tx, NULL), TAG, "speaker channel");
            i2s_std_config_t std_cfg = {
                .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                .gpio_cfg = {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = BSP_I2S_SCLK,
                    .ws = BSP_I2S_LCLK,
                    .dout = BSP_I2S_DOUT,
                    .din = I2S_GPIO_UNUSED,
                },
            };
            std_cfg.gpio_cfg.invert_flags.ws_inv = true;    /* the BSP's polarity */
            ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx, &std_cfg), TAG, "speaker i2s");
        }
        s_dir = 2;
    }
    if (!s_spk_on) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_spk_tx), TAG, "speaker on");
        s_spk_on = true;
    }
    return ESP_OK;
}

static int mic_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    if (on) {
        return use_mic() == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR;
    }
    if (s_mic_on && s_mic_rx && i2s_channel_disable(s_mic_rx) != ESP_OK) {
        return ESP_CODEC_DEV_DRV_ERR;
    }
    s_mic_on = false;
    return ESP_CODEC_DEV_OK;
}

/*
 * Both PDM slots, as the 2-slot stream muse_audio reads; the mic answers on
 * one. The ESP32's PDM-to-PCM filter leaves a large DC offset, which the gain
 * would clip: each slot's zero level is tracked (a 10 Hz high-pass; M5Unified
 * tracks it too) and taken off first.
 */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    if (use_mic() != ESP_OK) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    size_t got;
    if (i2s_channel_read(s_mic_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    int16_t *s = (int16_t *)data;
    int n = size / 2;
    if (!s_mic_settled) {
        int32_t sum[2] = { 0, 0 };
        for (int i = 0; i < n; i++) {
            sum[i & 1] += s[i];
        }
        s_mic_dc[0] = sum[0] / (n / 2) * 256;
        s_mic_dc[1] = sum[1] / (n / 2) * 256;
        s_mic_settled = true;
    }
    for (int i = 0; i < n; i++) {
        int32_t *dc = &s_mic_dc[i & 1];
        *dc += (s[i] * 256 - *dc) >> 8;
        int v = (s[i] - (*dc >> 8)) * s_mic_gain_q8 >> 8;
        s[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
}

static int spk_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    if (on) {
        return use_speaker() == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR;
    }
    if (s_spk_on && s_spk_tx && i2s_channel_disable(s_spk_tx) != ESP_OK) {
        return ESP_CODEC_DEV_DRV_ERR;
    }
    s_spk_on = false;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    if (use_speaker() != ESP_OK) {
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    size_t wrote;
    return i2s_channel_write(s_spk_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK &&
            wrote == (size_t)size ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Power the amp: AXP192 GPIO2, through the BSP's rail setup. I2S channels
     * are created lazily by use_mic()/use_speaker(), one direction at a time. */
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_SPEAKER, true), TAG, "speaker rail");

    static const audio_codec_data_if_t spk_if = { .enable = spk_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = mic_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* BtnB reports real held edges. The AXP192 reports completed short/long
 * presses instead: a short press is an aux click, and a long press requests
 * shutdown directly, before the PMU's ~4 s hardware cut. */
static unsigned poll_buttons(void)
{
    unsigned out = 0;
    int zone = tp_zone();
    if (zone >= 0) {
        bool pressed = zone == 1;
        if (pressed && !s_talk_pressed) {
            out |= MUSE_BTN_TALK_PRESS;
        } else if (!pressed && s_talk_pressed) {
            out |= MUSE_BTN_TALK_RELEASE;
        }
        s_talk_pressed = pressed;
    }
    int pek = pek_read();
    if (pek >= 0) {
        if (pek & 0x01) {
            muse_input_request_power_off();
        } else if (pek & 0x02) {
            out |= MUSE_BTN_AUX_PRESS | MUSE_BTN_AUX_RELEASE;
        }
    }
    return out;
}

/* Battery voltage (12-bit ADC, 1.1 mV steps), charger and USB state. */
static esp_err_t read_power(muse_power_t *out)
{
    uint8_t r00, r01, batt[2];
    ESP_RETURN_ON_ERROR(axp_read(0x00, &r00, 1), TAG, "axp status");
    ESP_RETURN_ON_ERROR(axp_read(0x01, &r01, 1), TAG, "axp mode");
    ESP_RETURN_ON_ERROR(axp_read(0x78, batt, sizeof(batt)), TAG, "axp battery");
    out->usb = (r00 & 0x20) != 0;       /* VBUS present */
    out->charging = (r00 & 0x04) != 0;
    if ((r01 & 0x20) == 0) {
        out->battery_mv = 0;
        out->battery_pct = -1;
        return ESP_OK;
    }
    int v = (int)(((batt[0] << 4) | (batt[1] & 0x0f)) * AXP_BATT_MV_LSB);
    out->battery_mv = v;
    if (v < 2500) {
        out->battery_pct = -1;
        return ESP_OK;
    }
    /* The Watcher's LiPo curve, as on the other M5Stack boards. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/*
 * Screen dark, then the AXP192 cuts every rail; PWR turns the board back on.
 * The PEK event register has no physical held state to wait on.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    ESP_RETURN_ON_ERROR(axp_write_masked(0x32, 0x80, 0x80), TAG, "axp shutdown");
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "M5Stack Core2",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "middle",
    .aux_button = "side",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -30 },   /* BtnB, bottom strip centre */
    .aux_hint = { LV_ALIGN_LEFT_MID, 4, -98 },       /* PWR, on the left edge */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0,              /* M5Unified's slot: the ESP32 stores each pair swapped */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
