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
 * ALIENTEK ATK-DNESP32S3-BOX V1.1 (not the BOX0, BOX2 or BOX3): ESP32-S3 with
 * 8 MB PSRAM, 320x240 ST7789 on an 8-bit i80 bus (no touch), one ES8311 for
 * speaker and mic, keys K0 (BOOT, GPIO0), K1 and K2 on an XL9555 expander,
 * which also switches the backlight and the speaker amplifier. Pins follow
 * xiaozhi-esp32's alientek/atk-dnesp32s3-box board; the LCD, ES8311 output
 * and the three keys were checked on a unit by Vibe Buddy.
 * K0 is talk, K1 steps the menu; K2 is unused.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_CS GPIO_NUM_1
#define LCD_DC GPIO_NUM_2
#define LCD_RD GPIO_NUM_41
#define LCD_WR GPIO_NUM_42
#define DRAW_BUF_LINES 24

#define I2C_SDA GPIO_NUM_48
#define I2C_SCL GPIO_NUM_45
#define I2S_WS GPIO_NUM_13
#define I2S_BCLK GPIO_NUM_21
#define I2S_DIN GPIO_NUM_47
#define I2S_DOUT GPIO_NUM_14

#define TALK_GPIO GPIO_NUM_0       /* K0, BOOT */

/* XL9555 port 0. Its pins are inputs until configured. */
#define XL9555_ADDR 0x20
#define XL9555_IN0 0x00
#define XL9555_OUT0 0x02
#define XL9555_CFG0 0x06
#define XL_K2 (1u << 3)
#define XL_K1 (1u << 4)
#define XL_SPK_EN (1u << 5)
#define XL_BL (1u << 7)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_xl;
static esp_lcd_panel_handle_t s_panel;
static esp_codec_dev_handle_t s_spk, s_mic;
static muse_gpio_button_t s_talk;
static uint8_t s_xl_out;           /* last value written to OUT0 */
static SemaphoreHandle_t s_xl_lock; /* the UI and audio tasks both write OUT0 */
/* K1, an expander key, debounced like muse_gpio_button_poll(). */
static bool s_k1_pressed;
static uint8_t s_k1_stable;

static esp_err_t xl_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_xl, buf, sizeof(buf), 50);
}

static esp_err_t xl_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_xl, &reg, 1, val, 1, 50);
}

static esp_err_t xl_set(uint8_t mask, bool on)
{
    xSemaphoreTake(s_xl_lock, portMAX_DELAY);
    uint8_t out = on ? (s_xl_out | mask) : (s_xl_out & ~mask);
    esp_err_t err = xl_write(XL9555_OUT0, out);
    if (err == ESP_OK) {
        s_xl_out = out;
    }
    xSemaphoreGive(s_xl_lock);
    return err;
}

static esp_err_t init(void)
{
    rtc_gpio_deinit(TALK_GPIO);
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
    /* Backlight and amplifier off until the display and audio start. */
    ESP_RETURN_ON_ERROR(xl_read(XL9555_OUT0, &s_xl_out), TAG, "xl9555 read");
    ESP_RETURN_ON_ERROR(xl_set(XL_BL | XL_SPK_EN, false), TAG, "xl9555 outputs");
    uint8_t dir;
    ESP_RETURN_ON_ERROR(xl_read(XL9555_CFG0, &dir), TAG, "xl9555 dir");
    dir = (dir | XL_K1 | XL_K2) & ~(XL_BL | XL_SPK_EN);
    ESP_RETURN_ON_ERROR(xl_write(XL9555_CFG0, dir), TAG, "xl9555 dir");
    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
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
            GPIO_NUM_40, GPIO_NUM_39, GPIO_NUM_38, GPIO_NUM_12,
            GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_46,
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
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = { .dc_data_level = 1 },
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
    /* Landscape, as Vibe Buddy and xiaozhi run it. */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
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

/* The backlight is a switch on the expander, not PWM. */
static void set_brightness(int pct)
{
    xl_set(XL_BL, pct > 0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

/* One ES8311 does both directions over a duplex I2S bus clocked from BCLK, without MCLK. */
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
            .mclk = I2S_GPIO_UNUSED,
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
    /* esp_codec_dev takes the 8-bit address: 0x30 is the ES8311 at 7-bit 0x18. */
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,     /* the amplifier is on the expander */
        .use_mclk = false,
        .mclk_div = 256,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");
    ESP_RETURN_ON_ERROR(xl_set(XL_SPK_EN, true), TAG, "amplifier on");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = s_spk = esp_codec_dev_new(&out_cfg);
    *mic = s_mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* K1, active low on the expander, debounced like muse_gpio_button_poll(). */
static unsigned poll_k1(void)
{
    uint8_t in;
    if (xl_read(XL9555_IN0, &in) != ESP_OK) {
        return 0;
    }
    bool down = (in & XL_K1) == 0;
    if (down == s_k1_pressed) {
        s_k1_stable = 0;
        return 0;
    }
    if (++s_k1_stable < 3) {
        return 0;
    }
    s_k1_stable = 0;
    s_k1_pressed = down;
    return down ? MUSE_BTN_AUX_PRESS : MUSE_BTN_AUX_RELEASE;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | poll_k1();
}

static esp_err_t power_off(void)
{
    /* No power latch: sleep until K0 or RESET. */
    xl_set(XL_BL, false);
    esp_lcd_panel_disp_on_off(s_panel, false);
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    xl_set(XL_SPK_EN, false);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "wake button");
    rtc_gpio_pullup_en(TALK_GPIO);
    rtc_gpio_pulldown_dis(TALK_GPIO);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "ALIENTEK ATK-DNESP32S3-BOX",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = false,
    .diagonal_in = 2.4f,
    .talk_button = "K0",
    .aux_button = "K1",
    /* The keys run along the top edge, K2, K1, K0 from the left. K1's icon
     * sits right of centre, clear of the status line's Wi-Fi icon. */
    .talk_hint = { LV_ALIGN_TOP_RIGHT, -8, 8 },
    .aux_hint = { LV_ALIGN_TOP_MID, 40, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
