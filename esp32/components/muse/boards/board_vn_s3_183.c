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
 * VN ESP32-S3 1.83-inch NV3023: a Vietnamese ESP32-S3R8 board (16 MB flash, 8 MB
 * octal PSRAM) with a 1.83" 240x284 IPS LCD on an NV3023 over SPI (the Xingzhi
 * Cube 1.83" panel), an ES8311 codec for the speaker and an ES7210 ADC for the
 * mics, BOOT, Vol+ and Vol- buttons, and a battery with a charge-status pin.
 * Native USB Serial/JTAG. It ships with xiaozhi firmware, where it's
 * xiaozhi-ai-iot-vietnam-1st.
 *
 * Pins, the panel's init sequence, its 36-column offset and the battery levels
 * come from that xiaozhi-esp32 port in TienHuyIoT/xiaozhi-esp32_vietnam
 * (MIT): main/boards/xiaozhi-ai-iot-vietnam-1st/ config.h,
 * xiaozhi-ai-iot-vietnam-1st.cc and power_manager.h. The same table drives the
 * Xingzhi Cube 1.83" in xingzhi-cube-1.83tft-wifi.
 *
 * xiaozhi reads the ES7210's first channel as the voice and its second as the
 * speaker reference for echo cancelling, so Muse takes slot 0.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_nv3023.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_H_RES 284          /* landscape: the panel's rows and columns swapped */
#define LCD_V_RES 240
#define LCD_X_GAP 36           /* the glass starts 36 columns into the NV3023's 320 */
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_9
#define LCD_MOSI GPIO_NUM_10
#define LCD_CS GPIO_NUM_14
#define LCD_DC GPIO_NUM_8
#define LCD_RST GPIO_NUM_18
#define LCD_BL GPIO_NUM_13     /* active high */
/* Two buffers of 16 rows, 18 KB of internal RAM. 32 rows took 36 KB, which
 * left no 8 KB block once Muse was paired, so the Muse VM lookup task couldn't
 * start and every turn failed with CAN'T REACH MUSE. */
#define DRAW_BUF_LINES 16

#define I2C_SDA GPIO_NUM_12    /* ES8311 and ES7210 */
#define I2C_SCL GPIO_NUM_11

#define I2S_MCLK GPIO_NUM_5
#define I2S_BCLK GPIO_NUM_15
#define I2S_WS GPIO_NUM_16
#define I2S_DOUT GPIO_NUM_6
#define I2S_DIN GPIO_NUM_7
#define PA_EN GPIO_NUM_4       /* amp on while HIGH */

#define TALK_GPIO GPIO_NUM_0   /* BOOT */
#define MENU_GPIO GPIO_NUM_39  /* Vol+: opens the menu, then moves down */
#define UP_GPIO GPIO_NUM_40    /* Vol-: moves up, or back from a page */
#define CHARGING_GPIO GPIO_NUM_47   /* LOW while charging */
#define BATT_ADC ADC_CHANNEL_6      /* ADC2: GPIO17 */

static const nv3023_lcd_init_cmd_t s_lcd_init[] = {
    {0xfd, (uint8_t[]){0x06, 0x08}, 2, 0},
    {0x61, (uint8_t[]){0x07, 0x04}, 2, 0},
    {0x62, (uint8_t[]){0x00, 0x44, 0x45}, 3, 0},
    {0x63, (uint8_t[]){0x41, 0x07, 0x12, 0x12}, 4, 0},
    {0x64, (uint8_t[]){0x37}, 1, 0},
    {0x65, (uint8_t[]){0x09, 0x10, 0x21}, 3, 0},
    {0x66, (uint8_t[]){0x09, 0x10, 0x21}, 3, 0},
    {0x67, (uint8_t[]){0x20, 0x40}, 2, 0},
    {0x68, (uint8_t[]){0x90, 0x4c, 0x7C, 0x66}, 4, 0},
    {0xb1, (uint8_t[]){0x0F, 0x02, 0x01}, 3, 0},
    {0xB4, (uint8_t[]){0x01}, 1, 0},
    {0xB5, (uint8_t[]){0x02, 0x02, 0x0a, 0x14}, 4, 0},
    {0xB6, (uint8_t[]){0x04, 0x01, 0x9f, 0x00, 0x02}, 5, 0},
    {0xdf, (uint8_t[]){0x11}, 1, 0},
    {0xE2, (uint8_t[]){0x13, 0x00, 0x00, 0x30, 0x33, 0x3f}, 6, 0},
    {0xE5, (uint8_t[]){0x3f, 0x33, 0x30, 0x00, 0x00, 0x13}, 6, 0},
    {0xE1, (uint8_t[]){0x00, 0x57}, 2, 0},
    {0xE4, (uint8_t[]){0x58, 0x00}, 2, 0},
    {0xE0, (uint8_t[]){0x01, 0x03, 0x0d, 0x0e, 0x0e, 0x0c, 0x15, 0x19}, 8, 0},
    {0xE3, (uint8_t[]){0x1a, 0x16, 0x0C, 0x0f, 0x0e, 0x0d, 0x02, 0x01}, 8, 0},
    {0xE6, (uint8_t[]){0x00, 0xff}, 2, 0},
    {0xE7, (uint8_t[]){0x01, 0x04, 0x03, 0x03, 0x00, 0x12}, 6, 0},
    {0xE8, (uint8_t[]){0x00, 0x70, 0x00}, 3, 0},
    {0xEc, (uint8_t[]){0x52}, 1, 0},
    {0xF1, (uint8_t[]){0x01, 0x01, 0x02}, 3, 0},
    {0xF6, (uint8_t[]){0x09, 0x10, 0x00, 0x00}, 4, 0},
    {0xfd, (uint8_t[]){0xfa, 0xfc}, 2, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0xa8}, 1, 0},
    {0x21, NULL, 0, 0},
    {0x11, NULL, 0, 200},
    {0x29, NULL, 0, 10},
};

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk, s_menu, s_up;
static adc_oneshot_unit_handle_t s_adc;

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
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_menu, MENU_GPIO), TAG, "menu button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_up, UP_GPIO), TAG, "up button");

    const gpio_config_t charging = {
        .pin_bit_mask = 1ULL << CHARGING_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&charging), TAG, "charging pin");
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_2 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    return adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg);
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
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = NV3023_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    const nv3023_vendor_config_t vendor = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    if (esp_lcd_new_panel_nv3023(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
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
        .panel_io = s_io,
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

/* SLPIN/SLPOUT, as the xiaozhi port sends them; GRAM is kept. */
static void panel_sleep(bool sleep)
{
    esp_lcd_panel_io_tx_param(s_io, sleep ? 0x10 : 0x11, NULL, 0);
    if (!sleep) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

/* ES8311 out and ES7210 in on one duplex I2S bus, MCLK from GPIO5. */
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
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_menu) << 2;
    if (muse_gpio_button_poll(&s_up) & MUSE_BTN_TALK_PRESS) {
        ev |= MUSE_BTN_UP;
    }
    return ev;
}

/* Raw 12-bit counts at 12 dB to percent, the xiaozhi port's table. The divider
 * isn't documented, so no millivolts. */
static esp_err_t read_power(muse_power_t *out)
{
    static const struct { int raw, pct; } levels[] = {
        {1985, 0}, {2048, 20}, {2172, 40}, {2296, 60}, {2420, 80}, {2544, 100},
    };
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        /* Wi-Fi shares ADC2 and wins: skip this reading quietly, the next one comes soon. */
        if (adc_oneshot_read(s_adc, BATT_ADC, &v) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        sum += v;
    }
    int raw = sum / 8, pct = raw < levels[0].raw ? 0 : 100;
    for (int i = 0; i + 1 < (int)(sizeof(levels) / sizeof(levels[0])); i++) {
        if (raw >= levels[i].raw && raw < levels[i + 1].raw) {
            pct = levels[i].pct + (raw - levels[i].raw) * (levels[i + 1].pct - levels[i].pct)
                                  / (levels[i + 1].raw - levels[i].raw);
            break;
        }
    }
    out->battery_pct = pct;
    out->battery_mv = 0;
    out->charging = gpio_get_level(CHARGING_GPIO) == 0;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "wake button");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "VN ESP32-S3 1.83-inch NV3023",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = false,
    .diagonal_in = 1.83f,
    .talk_button = "boot",
    .aux_button = "Vol+",
    /* The menu puts each button's hint on the bottom bar by these: Select on
     * the left, Down on the right. */
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 8, -8 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -8, -8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* MIC1; slot 1 is the speaker reference */
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
