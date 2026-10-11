/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

/*
 * Xingzhi Cube 1.54 inch TFT WiFi: ESP32-S3R8 (16 MB flash, 8 MB octal
 * PSRAM), a 240x240 ST7789 on SPI, no touch, three buttons, simplex I2S
 * audio (a digital MEMS mic and a separate speaker amp, no codec chip) and
 * a LiPo read through an ADC divider. Pins, the panel setup, the battery
 * table and the GPIO21 power latch are the vendor values, from the
 * xiaozhi-esp32 board support (boards/nologo/xingzhi-cube-1.54tft-wifi:
 * config.h, the board source and power_manager.h), github.com/78/xiaozhi-esp32.
 */
#include <math.h>

#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_state.h"

static const char *TAG = "board";

#define LCD_W 240
#define LCD_H 240
#define LCD_HOST SPI3_HOST
#define LCD_MOSI GPIO_NUM_10
#define LCD_SCLK GPIO_NUM_9
#define LCD_DC GPIO_NUM_8
#define LCD_CS GPIO_NUM_14
#define LCD_RST GPIO_NUM_18
#define LCD_BL GPIO_NUM_13
#define BL_HZ 25000
#define DRAW_BUF_LINES 32

#define TALK_GPIO GPIO_NUM_0
#define AUX_GPIO GPIO_NUM_40
#define DOWN_GPIO GPIO_NUM_39
#define POWER_LATCH GPIO_NUM_21

#define MIC_SCK GPIO_NUM_5
#define MIC_WS GPIO_NUM_4
#define MIC_DIN GPIO_NUM_6
#define SPK_BCLK GPIO_NUM_15
#define SPK_WS GPIO_NUM_16
#define SPK_DOUT GPIO_NUM_7

#define BATT_ADC_UNIT ADC_UNIT_2
#define BATT_ADC_CHAN ADC_CHANNEL_6
#define BATT_PIN GPIO_NUM_38

static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk, s_aux, s_down;
static adc_oneshot_unit_handle_t s_adc;
static i2s_chan_handle_t s_mic_rx, s_spk_tx;
static bool s_mic_on;
static int s_mic_gain_q8 = 256;
static muse_power_t s_power_cache = { .battery_pct = -1 };

static esp_err_t init(void)
{
    /* The power button turns the board on through a latch; hold it, as the
     * vendor firmware does, so battery power stays on (and survives light
     * sleep, which powers down the RTC pad registers). */
    rtc_gpio_init(POWER_LATCH);
    rtc_gpio_set_direction(POWER_LATCH, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(POWER_LATCH, 1);
    rtc_gpio_hold_en(POWER_LATCH);

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_down, DOWN_GPIO), TAG, "down button");
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_aux.pressed = gpio_get_level(AUX_GPIO) == 0;
    s_down.pressed = gpio_get_level(DOWN_GPIO) == 0;

    const gpio_config_t batt = {
        .pin_bit_mask = BIT64(BATT_PIN), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&batt), TAG, "battery pin");
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = BATT_ADC_UNIT };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    return adc_oneshot_config_channel(s_adc, BATT_ADC_CHAN, &ch_cfg);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BL_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 3,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);
}

/* Audio without a codec: two simplex I2S channels behind esp_codec_dev data
 * interfaces, as on the StickC Plus2. The stream Muse uses is 2-slot; the
 * mic answers on the left slot (its L/R pin is grounded) and the speaker
 * amp reads the left slot too. The mic is a digital MEMS with fixed
 * sensitivity, so gain is software, centred on unity at the default 30 dB
 * (the vendor firmware runs this mic at unity into its recogniser). */
static int mic_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    if (on != s_mic_on) {
        if ((on ? i2s_channel_enable(s_mic_rx) : i2s_channel_disable(s_mic_rx)) != ESP_OK) {
            return ESP_CODEC_DEV_DRV_ERR;
        }
        s_mic_on = on;
    }
    return ESP_CODEC_DEV_OK;
}

static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t got;
    if (i2s_channel_read(s_mic_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    if (s_mic_gain_q8 != 256) {
        int16_t *s = (int16_t *)data;
        for (int i = 0; i < size / 2; i++) {
            int v = s[i] * s_mic_gain_q8 >> 8;
            s[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
        }
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - 30) / 20.0f));
}

static int spk_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    (void)on;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t wrote;
    if (i2s_channel_write(s_spk_tx, data, size, &wrote, portMAX_DELAY) != ESP_OK || wrote != (size_t)size) {
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan, NULL, &s_mic_rx), TAG, "mic channel");
    const i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx, &mic_cfg), TAG, "mic i2s");

    i2s_chan_config_t spk_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spk_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan, &s_spk_tx, NULL), TAG, "speaker channel");
    const i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx, &spk_cfg), TAG, "speaker i2s");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_spk_tx), TAG, "speaker on");

    static const audio_codec_data_if_t spk_if = { .enable = spk_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = mic_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2;
    if (muse_gpio_button_poll(&s_down) & MUSE_BTN_TALK_PRESS) {
        ev |= MUSE_BTN_DOWN;
    }
    return ev;
}

/* The vendor raw-ADC level table for this board (power_manager.h), linearly
 * interpolated. ADC2 is shared with Wi-Fi, so a read can fail; the last
 * good reading is kept then. Millivolts are mapped from the same table:
 * raw 1970 is 0 percent (3.3 V), raw 2430 is 100 percent (4.2 V). The
 * vendor reads the same pin digitally for charging: high means charging. */
static esp_err_t read_power(muse_power_t *out)
{
    static const struct { uint16_t adc; uint8_t level; } levels[] = {
        { 1970, 0 }, { 2062, 20 }, { 2154, 40 }, { 2246, 60 }, { 2338, 80 }, { 2430, 100 },
    };
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHAN, &raw) == ESP_OK) {
            sum += raw;
            n++;
        }
    }
    if (n == 0) {
        *out = s_power_cache;
        return ESP_OK;
    }
    int raw = sum / n;
    muse_power_t p = { .battery_pct = -1, .battery_mv = 0, .charging = false, .usb = false };
    if (raw >= 1800) {
        p.battery_mv = 3300 + (raw - 1970) * 900 / 460;
        if (p.battery_mv > 4400) {
            p.battery_mv = 4400;
        }
        if (raw < levels[0].adc) {
            p.battery_pct = 0;
        } else if (raw >= levels[5].adc) {
            p.battery_pct = 100;
        } else {
            for (int i = 0; i < 5; i++) {
                if (raw >= levels[i].adc && raw < levels[i + 1].adc) {
                    p.battery_pct = levels[i].level +
                        (raw - levels[i].adc) * (levels[i + 1].level - levels[i].level) / (levels[i + 1].adc - levels[i].adc);
                    break;
                }
            }
        }
        p.charging = gpio_get_level(BATT_PIN) == 1 && p.battery_pct < 100;
    }
    s_power_cache = p;
    *out = p;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    rtc_gpio_set_level(POWER_LATCH, 0);
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "talk button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Xingzhi Cube 1.54",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    .diagonal_in = 1.54f,
    .keyboard = false,
    .talk_button = "front",
    .aux_button = "top",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -4 },
    .aux_hint = { LV_ALIGN_TOP_MID, 0, 4 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
