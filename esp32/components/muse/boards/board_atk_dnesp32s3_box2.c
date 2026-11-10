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
 * ALIENTEK ATK-DNESP32S3-BOX2 (Wi-Fi), not the V1.1 box: ESP32-S3 with 8 MB
 * octal PSRAM, a 240x320 ST7789 on an 8-bit i80 bus run as 320x240 landscape
 * (xiaozhi runs it portrait; no touch), one ES8389 for speaker and mic clocked
 * from MCLK, a PWM backlight on GPIO21 and a battery divider on GPIO1. A 16-bit
 * XL9555 holds the power latch, the speaker amplifier, the analog and 4G
 * rails, the charger lines and keys L, M and Q; key R is GPIO0 (BOOT). Pins,
 * the panel's init table, the expander's lines and the battery table follow
 * xiaozhi-esp32's alientek/atk-dnesp32s3-box2-wifi board (config.h,
 * atk_dnesp32s3_box2.cc, power_manager.h). M is talk, R steps the menu, L
 * turns the speaker on and off and Q is power: a tap sleeps or wakes, a 2 s
 * hold powers off. An SC7A20 accelerometer at 0x19, found on a unit (xiaozhi
 * doesn't use it), turns the picture the right way up when the box is turned
 * over.
 */
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_H_RES 320           /* landscape: the 240x320 panel turned */
#define LCD_V_RES 240
#define LCD_CS GPIO_NUM_14
#define LCD_DC GPIO_NUM_12
#define LCD_RD GPIO_NUM_10
#define LCD_WR GPIO_NUM_11
#define LCD_BL GPIO_NUM_21
/* Two internal-RAM strips of 320 x 8 (10 KB): with 32 lines, Link and Muse's
 * sessions ran out of internal RAM and Muse couldn't connect. */
#define DRAW_BUF_LINES 8

#define I2C_SDA GPIO_NUM_48
#define I2C_SCL GPIO_NUM_47
#define I2S_MCLK GPIO_NUM_38
#define I2S_WS GPIO_NUM_42
#define I2S_BCLK GPIO_NUM_40
#define I2S_DIN GPIO_NUM_39
#define I2S_DOUT GPIO_NUM_41

#define AUX_GPIO GPIO_NUM_0        /* key R, BOOT */
#define XL_INT_GPIO GPIO_NUM_2     /* the expander's open-drain interrupt */
#define BATT_ADC ADC_CHANNEL_0     /* GPIO1 */

/* XL9555, a TCA9555 at 0x20. A set CFG bit is an input. */
#define XL9555_ADDR 0x20
#define XL9555_IN0 0x00
#define XL9555_OUT0 0x02
#define XL9555_OUT1 0x03
#define XL9555_CFG0 0x06
#define XL9555_CFG1 0x07
/* Port 0: P0.3 and P0.4 (SBU2, SBU1) are outputs, the rest inputs. */
#define XL_CFG0 0xE7
#define XL_KEY_L (1u << 5)         /* active low */
#define XL_KEY_Q (1u << 6)         /* active low on USB and battery; in xiaozhi's list, unused there */
#define XL_KEY_M (1u << 7)         /* active high */
#define POWER_HOLD_US 2000000      /* Q held this long powers off */
#define KEYS_UP_WAIT_MS 5000       /* the longest power_off() waits for the keys to come up */
/* Port 1: P1.0 to P1.5 outputs, P1.6 and P1.7 inputs. */
#define XL_CFG1 0xC0
#define XL_USB_SEL (1u << 0)
#define XL_SPK_EN (1u << 1)
#define XL_SYS_POW (1u << 2)       /* power latch: low cuts battery power */
#define XL_VBUS_EN (1u << 3)
#define XL_EN_4G (1u << 4)
#define XL_EN_3V3A (1u << 5)
#define XL_CHG_CTRL (1u << 6)      /* input, driven low before power off */
#define XL_CHRG (1u << 7)          /* low while charging */

/* SC7A20 (Silan), LIS2DH registers. Measured on a unit: Y reads -1 g with
 * the picture upright and +1 g turned over, and Z +1 g lying face up. */
#define ACC_ADDR 0x19
#define ACC_WHO_AM_I 0x0F
#define ACC_ID 0x11
#define ACC_CTRL1 0x20
#define ACC_CTRL4 0x23
#define ACC_OUT_X_L 0x28
#define ACC_AUTO_INC 0x80
#define TURN_MG 700                /* past this on Y the box is upright or turned over */
#define TURN_POLLS 50              /* poll_buttons() calls between readings: 0.5 s */
#define AVATAR_PX 192

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_xl;
static i2c_master_dev_handle_t s_acc;   /* NULL: no accelerometer answered */
static esp_lcd_panel_handle_t s_panel;
static esp_codec_dev_handle_t s_spk, s_mic;
static adc_oneshot_unit_handle_t s_adc;
static muse_gpio_button_t s_aux;
static uint8_t s_xl_out1;           /* last value written to OUT1 */
static uint8_t s_xl_cfg1;           /* last value written to CFG1 */
static SemaphoreHandle_t s_xl_lock; /* the UI, input and audio tasks all write port 1 */
/* Expander keys, debounced like muse_gpio_button_poll(). */
typedef struct {
    bool pressed;
    uint8_t stable;
} xl_key_t;
static xl_key_t s_m, s_l, s_q;
static int64_t s_q_down_us;         /* when Q went down, for the power-off hold */
static bool s_q_long;               /* and the hold's been reported */
static bool s_flipped;              /* the picture turned 180 degrees */

static esp_err_t xl_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_xl, buf, sizeof(buf), 50);
}

/* Both ports: the register pointer steps to the port 1 register. */
static esp_err_t xl_read2(uint8_t reg, uint8_t val[2])
{
    return i2c_master_transmit_receive(s_xl, &reg, 1, val, 2, 50);
}

static esp_err_t xl_set(uint8_t mask, bool on)
{
    xSemaphoreTake(s_xl_lock, portMAX_DELAY);
    uint8_t out = on ? (s_xl_out1 | mask) : (s_xl_out1 & ~mask);
    esp_err_t err = xl_write(XL9555_OUT1, out);
    if (err == ESP_OK) {
        s_xl_out1 = out;
    }
    xSemaphoreGive(s_xl_lock);
    return err;
}

static bool m_down(uint8_t p0)
{
    return (p0 & XL_KEY_M) != 0;
}

static bool l_down(uint8_t p0)
{
    return (p0 & XL_KEY_L) == 0;
}

static bool q_down(uint8_t p0)
{
    return (p0 & XL_KEY_Q) == 0;
}

/* A key held at boot (M switched the box on, or a key woke it) is a press
 * already under way: letting it go isn't a press, and Q's hold doesn't
 * count toward powering off. */
static void keys_at_boot(uint8_t p0)
{
    s_m.pressed = m_down(p0);
    s_l.pressed = l_down(p0);
    s_q.pressed = q_down(p0);
    s_q_long = s_q.pressed;
}

static esp_err_t init(void)
{
    /* power_off() holds the backlight low through deep sleep; let it go. */
    gpio_hold_dis(LCD_BL);
    gpio_deep_sleep_hold_dis();
    rtc_gpio_deinit(AUX_GPIO);
    rtc_gpio_deinit(XL_INT_GPIO);
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t xl_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &xl_cfg, &s_xl), TAG, "xl9555");
    s_xl_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_xl_lock, ESP_ERR_NO_MEM, TAG, "xl9555 lock");
    /* The power latch first: on battery the board stays on only while the
     * key that turned it on is held until SYS_POW is driven high. Levels go
     * in before the pins become outputs. The analog rail, the 4G rail and
     * USB_SEL are set as xiaozhi sets them; the amplifier stays off until
     * the audio starts. */
    uint8_t out[2];
    ESP_RETURN_ON_ERROR(xl_read2(XL9555_OUT0, out), TAG, "xl9555 read");
    s_xl_out1 = (out[1] | XL_SYS_POW | XL_EN_3V3A | XL_EN_4G | XL_USB_SEL) & ~(XL_SPK_EN | XL_VBUS_EN | XL_CHG_CTRL);
    ESP_RETURN_ON_ERROR(xl_write(XL9555_OUT1, s_xl_out1), TAG, "xl9555 outputs");
    s_xl_cfg1 = XL_CFG1;
    ESP_RETURN_ON_ERROR(xl_write(XL9555_CFG1, s_xl_cfg1), TAG, "xl9555 dir");
    ESP_RETURN_ON_ERROR(xl_write(XL9555_CFG0, XL_CFG0), TAG, "xl9555 dir");
    uint8_t in[2];
    ESP_RETURN_ON_ERROR(xl_read2(XL9555_IN0, in), TAG, "xl9555 read");
    keys_at_boot(in[0]);

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    /* Without the accelerometer the picture just stays one way up. */
    const i2c_device_config_t acc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACC_ADDR,
        .scl_speed_hz = 400000,
    };
    uint8_t reg = ACC_WHO_AM_I, who = 0;
    if (i2c_master_bus_add_device(s_i2c, &acc_cfg, &s_acc) == ESP_OK &&
        i2c_master_transmit_receive(s_acc, &reg, 1, &who, 1, 50) == ESP_OK && who == ACC_ID) {
        const uint8_t ctrl1[2] = { ACC_CTRL1, 0x47 };   /* 50 Hz, X, Y and Z on */
        const uint8_t ctrl4[2] = { ACC_CTRL4, 0x00 };   /* +-2 g */
        if (i2c_master_transmit(s_acc, ctrl1, 2, 50) != ESP_OK || i2c_master_transmit(s_acc, ctrl4, 2, 50) != ESP_OK) {
            who = 0;
        }
    }
    if (who != ACC_ID && s_acc) {
        ESP_LOGW(TAG, "no SC7A20 accelerometer (0x%02x): the picture won't turn", who);
        i2c_master_bus_rm_device(s_acc);
        s_acc = NULL;
    }
    return muse_gpio_button_init(&s_aux, AUX_GPIO);
}

/* Whether the box is turned over, or false lying flat, on a side or unread. */
static bool read_flip(bool *flip)
{
    uint8_t reg = ACC_OUT_X_L | ACC_AUTO_INC, raw[6];
    if (!s_acc || i2c_master_transmit_receive(s_acc, &reg, 1, raw, sizeof(raw), 50) != ESP_OK) {
        return false;
    }
    int y = (int16_t)(raw[2] | raw[3] << 8) / 16;   /* left-justified, about 1 mg a count */
    if (abs(y) <= TURN_MG) {
        return false;
    }
    *flip = y > 0;
    return true;
}

/* Landscape, turned as the V1.1 box turns the same panel; turned over, both
 * mirrors change. */
static void set_flip(bool flipped)
{
    esp_lcd_panel_mirror(s_panel, !flipped, flipped);
    s_flipped = flipped;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
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
        ESP_LOGE(TAG, "backlight pwm");
        return NULL;
    }
    /* RD is unused: hold it high. */
    const gpio_config_t rd = {
        .pin_bit_mask = 1ULL << LCD_RD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&rd);
    gpio_set_level(LCD_RD, 1);

    esp_lcd_i80_bus_handle_t bus;
    const esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = LCD_DC,
        .wr_gpio_num = LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            GPIO_NUM_13, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_7,
            GPIO_NUM_6, GPIO_NUM_5, GPIO_NUM_4, GPIO_NUM_3,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_H_RES * DRAW_BUF_LINES * 2,
        .dma_burst_size = 64,
    };
    if (esp_lcd_new_i80_bus(&bus_cfg, &bus) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    /* No byte swap here: the LVGL adapter swaps RGB565 for this interface. */
    const esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = { .dc_idle_level = 1, .dc_data_level = 1 },
    };
    if (esp_lcd_new_panel_io_i80(bus, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    /* ALIENTEK's power, porch and gamma settings, from xiaozhi-esp32's
     * atk_dnesp32s3_box2.cc (MIT; devices/licenses/xiaozhi-esp32.txt). Its
     * MADCTL write is left out: swap_xy and mirror below set it. */
    static const struct {
        uint8_t cmd, len, data[14];
    } tuning[] = {
        { 0xCF, 3, { 0x00, 0x83, 0x30 } },
        { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 } },
        { 0xE8, 3, { 0x85, 0x01, 0x79 } },
        { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 } },
        { 0xF7, 1, { 0x20 } },
        { 0xEA, 2, { 0x00, 0x00 } },
        { 0xBB, 1, { 0x20 } },
        { 0xC3, 1, { 0x00 } },
        { 0xC4, 1, { 0x20 } },
        { 0xC5, 1, { 0x20 } },
        { 0xC6, 1, { 0x10 } },
        { 0xC7, 1, { 0xB0 } },
        { 0x3A, 1, { 0x55 } },
        { 0xB1, 2, { 0x00, 0x1B } },
        { 0xF2, 1, { 0x08 } },
        { 0x26, 1, { 0x01 } },
        { 0xE0, 14, { 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17 } },
        { 0xE1, 14, { 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E } },
        { 0xB7, 1, { 0x07 } },
    };
    for (size_t i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++) {
        esp_lcd_panel_io_tx_param(io, tuning[i].cmd, tuning[i].data, tuning[i].len);
    }
    /* Landscape, the right way up for how the box is held at boot. */
    bool flip = false;
    read_flip(&flip);
    esp_lcd_panel_swap_xy(s_panel, true);
    set_flip(flip);
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
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
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
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

/* One ES8389 does both directions over a duplex I2S bus clocked from MCLK. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    /* esp_codec_dev takes the 8-bit address: 0x20 is the ES8389 at 7-bit 0x10. */
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8389_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8389_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,     /* the amplifier is on the expander */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8389_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8389 not responding");
    ESP_RETURN_ON_ERROR(xl_set(XL_SPK_EN, true), TAG, "amplifier on");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = s_spk = esp_codec_dev_new(&out_cfg);
    *mic = s_mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* The amplifier, off while the codec rests closed. */
static void audio_power(bool on)
{
    xl_set(XL_SPK_EN, on);
}

/* One expander key: an edge once it's held its new level for three polls. */
static unsigned debounce(xl_key_t *k, bool down, unsigned press, unsigned release)
{
    if (down == k->pressed) {
        k->stable = 0;
        return 0;
    }
    if (++k->stable < 3) {
        return 0;
    }
    k->stable = 0;
    k->pressed = down;
    return down ? press : release;
}

/* M talks, L toggles the speaker, Q is power: a tap sleeps or wakes, a hold
 * powers off (muse_input.c's dedicated buttons). */
static unsigned poll_expander(void)
{
    uint8_t in[2];
    if (xl_read2(XL9555_IN0, in) != ESP_OK) {
        s_q_down_us = esp_timer_get_time();   /* a failed read never counts toward the hold */
        return 0;
    }
    unsigned ev = debounce(&s_m, m_down(in[0]), MUSE_BTN_TALK_PRESS, MUSE_BTN_TALK_RELEASE);
    ev |= debounce(&s_l, l_down(in[0]), MUSE_BTN_SPEAKER_PRESS, MUSE_BTN_SPEAKER_RELEASE);
    unsigned q = debounce(&s_q, q_down(in[0]), MUSE_BTN_POWER_PRESS, MUSE_BTN_POWER_RELEASE);
    if (q & MUSE_BTN_POWER_PRESS) {
        s_q_down_us = esp_timer_get_time();
        s_q_long = false;
    }
    if (s_q.pressed && q_down(in[0]) && !s_q_long && esp_timer_get_time() - s_q_down_us >= POWER_HOLD_US) {
        s_q_long = true;
        q |= MUSE_BTN_POWER_LONG;
    }
    return ev | q;
}

/* Every half second: turns the picture once the box has been the other way
 * up for two readings in a row. Lying flat or on a side keeps it as it is. */
static void check_flip(void)
{
    static int polls, votes;
    bool flip;
    if (!s_acc || ++polls < TURN_POLLS) {
        return;
    }
    polls = 0;
    if (!read_flip(&flip) || flip == s_flipped) {
        votes = 0;
        return;
    }
    if (++votes < 2 || !display_lock(100)) {
        return;   /* not yet, or LVGL is busy: the next reading tries again */
    }
    votes = 0;
    set_flip(flip);
    lv_obj_invalidate(lv_display_get_screen_active(NULL));
    lv_obj_invalidate(lv_display_get_layer_top(NULL));
    lv_obj_invalidate(lv_display_get_layer_sys(NULL));
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "picture turned %s", flip ? "over" : "upright");
}

static unsigned poll_buttons(void)
{
    check_flip();
    return poll_expander() | muse_gpio_button_poll(&s_aux) << 2;
}

/* Battery through a divider on GPIO1; raw 12 dB counts calibrated by xiaozhi. */
static esp_err_t read_power(muse_power_t *out)
{
    static const struct {
        int raw, pct;
    } levels[] = { { 2696, 0 }, { 2724, 20 }, { 2861, 40 }, { 3038, 60 }, { 3150, 80 }, { 3280, 100 } };
    enum { N = sizeof(levels) / sizeof(levels[0]) };
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &v), TAG, "adc read");
        sum += v;
    }
    int raw = sum / 8;
    int pct = raw <= levels[0].raw ? 0 : 100;
    for (int i = 0; i + 1 < N; i++) {
        if (raw >= levels[i].raw && raw < levels[i + 1].raw) {
            pct = levels[i].pct + (raw - levels[i].raw) * (levels[i + 1].pct - levels[i].pct) /
                                      (levels[i + 1].raw - levels[i].raw);
        }
    }
    uint8_t in[2];
    ESP_RETURN_ON_ERROR(xl_read2(XL9555_IN0, in), TAG, "charge state");
    bool charging = (in[1] & XL_CHRG) == 0;
    out->charging = charging && pct < 100;
    /* The charger stops once the battery is full, but a computer still answers. */
    out->usb = charging || usb_serial_jtag_is_connected();
    out->battery_pct = pct;
    out->battery_mv = 0;   /* uncalibrated counts */
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    panel_sleep(true);
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    xl_set(XL_SPK_EN, false);
    /* Wait for the keys to come up, so the latch isn't held by M or Q, but
     * not forever: a key still held keeps battery power on only until it's
     * let go. */
    uint8_t in[2] = { 0 };
    for (int waited = 0; waited < KEYS_UP_WAIT_MS; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(AUX_GPIO) != 0 &&
            (xl_read2(XL9555_IN0, in) != ESP_OK || (!m_down(in[0]) && !q_down(in[0])))) {
            break;
        }
    }
    /* As xiaozhi powers off: CHG_CTRL low, then the latch open. That cuts
     * power, on USB too (checked on a unit); M switches the box back on. */
    xSemaphoreTake(s_xl_lock, portMAX_DELAY);
    s_xl_out1 &= ~XL_CHG_CTRL;
    s_xl_cfg1 &= ~XL_CHG_CTRL;
    xl_write(XL9555_OUT1, s_xl_out1);
    xl_write(XL9555_CFG1, s_xl_cfg1);
    xSemaphoreGive(s_xl_lock);
    vTaskDelay(pdMS_TO_TICKS(100));
    xl_set(XL_SYS_POW, false);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Still here only if the latch didn't cut power: deep sleep until R or a
     * change on the expander (M, Q, L or CHRG, which pull its interrupt low).
     * CHG_CTRL goes back to an input, as xiaozhi leaves it, so the battery
     * charges meanwhile; reading the inputs then clears the interrupt. */
    ESP_LOGI(TAG, "still powered after the latch: deep sleep");
    xSemaphoreTake(s_xl_lock, portMAX_DELAY);
    s_xl_cfg1 |= XL_CHG_CTRL;
    xl_write(XL9555_CFG1, s_xl_cfg1);
    xSemaphoreGive(s_xl_lock);
    vTaskDelay(pdMS_TO_TICKS(20));
    xl_read2(XL9555_IN0, in);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 0);
    gpio_hold_en(LCD_BL);
    gpio_deep_sleep_hold_en();
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(AUX_GPIO) | BIT64(XL_INT_GPIO), ESP_EXT1_WAKEUP_ANY_LOW),
                        TAG, "wake keys");
    rtc_gpio_pullup_en(AUX_GPIO);
    rtc_gpio_pulldown_dis(AUX_GPIO);
    rtc_gpio_pullup_en(XL_INT_GPIO);
    rtc_gpio_pulldown_dis(XL_INT_GPIO);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "ALIENTEK ATK-DNESP32S3-BOX2",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = false,
    .diagonal_in = 2.4f,
    .talk_button = "M",
    .aux_button = "R",
    .wake_button = "M",
    .speaker_button = "L",
    .power_button = "Q",
    /* Keys L, M and R, left to right. */
    .talk_hint = { LV_ALIGN_TOP_MID, 0, 8 },
    .aux_hint = { LV_ALIGN_TOP_RIGHT, -8, 8 },
    .avatar_px = AVATAR_PX,
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* the mic is on the left slot; the right carries the DAC reference */
    .audio_power = audio_power,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
