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
 * M5Stack Cardputer ADV, ESP32-S3FN8, 8 MB flash, no PSRAM.
 * Hardware references (vendor sources):
 * https://github.com/m5stack/M5GFX/blob/master/src/M5GFX.cpp
 * https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.inl
 * https://github.com/m5stack/M5Cardputer/tree/master/src/utility/Keyboard
 * ST7789: native 135x240, landscape rotation 1, offsets 52/40.
 * ES8311: I2C SDA8/SCL9; duplex I2S BCLK41/WS43/OUT42/IN46, no MCLK.
 * TCA8418: 0x34, IRQ11, 7x8 matrix. Space is event 68, backtick event 1.
 * GO or Space: talk/confirm. Esc, arrows and Enter navigate the menu.
 * Uses the SDK's no-PSRAM voice-input/text-reply mode.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
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
#include "muse_lvgl_hold.h"

static const char *TAG = "board";

#define LCD_WIDTH 240
#define LCD_HEIGHT 135
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_36
#define LCD_MOSI GPIO_NUM_35
#define LCD_CS GPIO_NUM_37
#define LCD_DC GPIO_NUM_34
#define LCD_RST GPIO_NUM_33
#define LCD_BL GPIO_NUM_38
#define DRAW_BUF_LINES 4 /* two DMA strips: 3.75 KB; leave RAM for concurrent TLS RX/TX */

#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_9
#define I2S_MCLK GPIO_NUM_NC
#define I2S_WS GPIO_NUM_43
#define I2S_BCLK GPIO_NUM_41
#define I2S_DIN GPIO_NUM_46
#define I2S_DOUT GPIO_NUM_42
#define PA_EN GPIO_NUM_NC

#define TALK_GPIO GPIO_NUM_0
#define KEY_IRQ GPIO_NUM_11

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_keyboard;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;
static bool s_space, s_talk_down;

static esp_err_t key_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = { reg, value };
    return i2c_master_transmit(s_keyboard, data, sizeof(data), 20);
}

static esp_err_t key_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_keyboard, &reg, 1, value, 1, 20);
}

static esp_err_t keyboard_init(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &cfg, &s_keyboard), TAG, "keyboard bus");
    /* GPIO inputs, 7 row / 8 column keypad, debounce and pull-ups enabled. */
    const uint8_t regs[][2] = {
        {0x01, 0x00}, {0x23, 0x00}, {0x24, 0x00}, {0x25, 0x00},
        {0x1d, 0x7f}, {0x1e, 0xff}, {0x1f, 0x00},
        {0x29, 0x00}, {0x2a, 0x00}, {0x2b, 0x00},
        {0x2c, 0x00}, {0x2d, 0x00}, {0x2e, 0x00},
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        ESP_RETURN_ON_ERROR(key_write(regs[i][0], regs[i][1]), TAG, "keyboard config");
    }
    uint8_t event;
    for (int i = 0; i < 10; ++i) {
        ESP_RETURN_ON_ERROR(key_read(0x04, &event), TAG, "keyboard flush");
        if (!event) break;
    }
    ESP_RETURN_ON_ERROR(key_write(0x02, 0x1f), TAG, "keyboard clear");
    ESP_RETURN_ON_ERROR(key_write(0x01, 0x09), TAG, "keyboard event/overflow IRQ");
    const gpio_config_t irq = {
        .pin_bit_mask = 1ULL << KEY_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&irq);
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    return keyboard_init();
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 256, /* match M5GFX's Cardputer ADV backlight timing */
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
        .max_transfer_sz = LCD_WIDTH * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
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
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, true) != ESP_OK ||
        esp_lcd_panel_swap_xy(s_panel, true) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, true, false) != ESP_OK ||
        esp_lcd_panel_set_gap(s_panel, 40, 53) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
    }

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
            .hor_res = LCD_WIDTH,
            .ver_res = LCD_HEIGHT,
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

static void display_pause(bool pause)
{
    if (pause) {
        muse_lvgl_hold(MUSE_LVGL_HOLD_SCREEN, true);   /* bounded, shared with video */
    } else {
        muse_lvgl_hold(MUSE_LVGL_HOLD_SCREEN, false);
    }
}

/* One ES8311 handles both directions, clocked from BCLK like M5Unified. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PA_EN,
        .use_mclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    muse_gpio_button_poll(&s_talk);
    unsigned ev = 0;
    /* One queued key per tick preserves a rapid press/release as separate edges. */
    if (gpio_get_level(KEY_IRQ) == 0) {
        uint8_t status = 0, event = 0;
        if (key_read(0x02, &status) != ESP_OK || (status & 0x08)) {
            /* Lost events must not leave a talk/reset button held forever. */
            s_space = false;
            for (int i = 0; i < 10; ++i) {
                if (key_read(0x04, &event) != ESP_OK || !event) break;
            }
            key_write(0x02, 0x1f);
        } else if (key_read(0x04, &event) == ESP_OK) {
            switch (event & 0x7f) {
            case 68: s_space = (event & 0x80) != 0; break;
            /* Arrow legends share punctuation keys; no Fn is needed in this
             * menu-only keyboard mode. TCA8418 matrix mapping from M5Cardputer:
             * ; = row5/col6, . = row5/col7, , = row5/col3, / = row6/col3. */
            case 1:  if (event & 0x80) ev |= MUSE_BTN_ESCAPE; break;
            case 57: if (event & 0x80) ev |= MUSE_BTN_UP; break;
            case 58: if (event & 0x80) ev |= MUSE_BTN_DOWN; break;
            case 54: if (event & 0x80) ev |= MUSE_BTN_LEFT; break;
            case 64: if (event & 0x80) ev |= MUSE_BTN_RIGHT; break;
            case 67: if (event & 0x80) ev |= MUSE_BTN_ENTER; break;
            default: break;
            }
            if (!event) key_write(0x02, 0x01);
        } else {
            s_space = false;
        }
    }
    bool talk = s_talk.pressed || s_space;
    ev |= talk != s_talk_down ?
        (talk ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE) : 0;
    s_talk_down = talk;
    return ev;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    /* The ADV has a physical power switch, no software power latch.
     * Sleep until GO is pressed; the side switch fully powers it down. */
    while (gpio_get_level(TALK_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

/* SAFE mode (muse_board_t.safe_display_off): the backlight off (active high: its PWM's duty 0). */
static void safe_display_off(void)
{
    muse_board_pin_hold(LCD_BL, 0);
}

/* The SAFE key (muse_board_t.safe_key: the way into and out of SAFE): GPIO0 (the talk key), read raw. */
static bool safe_key(void)
{
    return muse_gpio_safe_key(TALK_GPIO, true);
}

static const muse_board_t s_board = {
    .name = "M5Stack Cardputer ADV",
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round = false,
    .touch = false,
    .talk_button = "GO/space",
    .aux_button = "Esc",
    .keyboard = true,
    /* Keyboard controls sit below the screen. */
    .talk_hint = { LV_ALIGN_BOTTOM_RIGHT, -14, -4 },
    .aux_hint = { LV_ALIGN_BOTTOM_LEFT, 15, -4 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .safe_display_off = safe_display_off,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .power_off = power_off,
    .safe_key = safe_key,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
