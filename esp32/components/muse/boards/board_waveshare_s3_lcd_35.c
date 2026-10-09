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
 * Waveshare ESP32-S3-Touch-LCD-3.5: ESP32-S3R8 (16 MB flash, 8 MB octal PSRAM),
 * 3.5" 320x480 ST7796 SPI LCD run landscape, FT6336 capacitive touch, ES8311
 * codec with speaker and mic, AXP2101 PMIC, TCA9554 I/O expander holding the
 * panel's CS (P0, kept low) and reset (P1), BOOT button on GPIO0, camera
 * connector (unused). The LCD/codec PMIC rails are off until the AXP2101 is
 * configured, so init() must run before the panel.
 * Pins and power init follow xiaozhi-esp32's board config
 * (main/boards/waveshare/esp32-s3-touch-lcd-3.5) and Waveshare's schematic.
 * There is no vendor BSP in the ESP Component Registry, so like the FNK0104B
 * board file this drives esp_lcd, the codec and the GPIOs directly.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* The panel is wired portrait; MADCTL's MV bit (esp_lcd_panel_swap_xy)
 * presents it landscape, as xiaozhi-esp32 does. */
#define LCD_H_RES 480
#define LCD_V_RES 320
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_5
#define LCD_MOSI GPIO_NUM_1
#define LCD_MISO GPIO_NUM_2
#define LCD_DC   GPIO_NUM_3
#define LCD_BL   GPIO_NUM_6     /* active high */
#define DRAW_BUF_LINES 32

#define I2C_SDA GPIO_NUM_8      /* shared by AXP2101, TCA9554, FT6336, ES8311 */
#define I2C_SCL GPIO_NUM_7
#define PMIC_ADDR 0x34          /* AXP2101 */
#define EXP_ADDR  0x20          /* TCA9554: P0 LCD CS (low = selected), P1 LCD RST */
#define EXP_REG_OUT 0x01
#define EXP_REG_CFG 0x03
#define EXP_LCD_CS  (1 << 0)
#define EXP_LCD_RST (1 << 1)
#define TOUCH_ADDR 0x38
#define TOUCH_REG_TD_STATUS 0x02
#define TOUCH_REG_P1 0x03       /* XH, XL, YH, YL; high nibble of XH/YH */

#define I2S_MCLK GPIO_NUM_12
#define I2S_BCLK GPIO_NUM_13
#define I2S_DIN  GPIO_NUM_14    /* mic */
#define I2S_DOUT GPIO_NUM_16    /* speaker */
#define I2S_WS   GPIO_NUM_15

#define TALK_GPIO GPIO_NUM_0    /* BOOT button */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pmic;
static i2c_master_dev_handle_t s_exp;
static i2c_master_dev_handle_t s_touch_dev;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

/* xiaozhi-esp32's Pmic constructor: DC1 at 3.3 V, ALDO1/BLDO1/BLDO2 on for the
 * codec and panel, charger tuned for a small Li-Po, 4 s power-key hold-off. */
static esp_err_t pmic_init(void)
{
    static const uint8_t seq[][2] = {
        {0x22, 0b110},
        {0x27, 0x10},
        {0x80, 0x01},                       /* disable all DC/DCs but DC1 */
        {0x90, 0x00},
        {0x91, 0x00},                       /* all LDOs off */
        {0x82, (3300 - 1500) / 100},        /* DC1 -> 3.3 V */
        {0x92, (3300 - 500) / 100},         /* ALDO1 -> 3.3 V */
        {0x96, (1500 - 500) / 100},
        {0x97, (2800 - 500) / 100},
        {0x90, 0x31},                       /* ALDO1, BLDO1, BLDO2 on */
        {0x64, 0x02},                       /* charger CV 4.1 V */
        {0x61, 0x02},                       /* precharge 50 mA */
        {0x62, 0x08},                       /* charge 200 mA */
        {0x63, 0x01},                       /* terminate at 25 mA */
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(reg_write(s_pmic, seq[i][0], seq[i][1]), TAG, "pmic reg 0x%02x", seq[i][0]);
    }
    return ESP_OK;
}

static esp_err_t expander_init(void)
{
    uint8_t cfg = 0xff;
    /* CS low (panel selected) and RST low, then release RST after 100 ms. */
    ESP_RETURN_ON_ERROR(reg_write(s_exp, EXP_REG_OUT, 0x00), TAG, "expander out");
    ESP_RETURN_ON_ERROR(reg_read(s_exp, EXP_REG_CFG, &cfg), TAG, "expander cfg");
    ESP_RETURN_ON_ERROR(reg_write(s_exp, EXP_REG_CFG, cfg & ~(EXP_LCD_CS | EXP_LCD_RST)),
                        TAG, "expander dir");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(reg_write(s_exp, EXP_REG_OUT, EXP_LCD_RST), TAG, "expander rst");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
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

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000,
    };
    i2c_device_config_t c = dev_cfg;
    c.device_address = PMIC_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &c, &s_pmic), TAG, "pmic i2c");
    c.device_address = EXP_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &c, &s_exp), TAG, "expander i2c");
    c.device_address = TOUCH_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &c, &s_touch_dev), TAG, "touch i2c");

    ESP_RETURN_ON_ERROR(pmic_init(), TAG, "pmic");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "expander");

    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static esp_err_t touch_read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, n, 50);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t count = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    if (touch_read_regs(TOUCH_REG_TD_STATUS, &count, 1) != ESP_OK || (count & 0x0F) == 0) {
        return;
    }
    uint8_t p[4];
    if (touch_read_regs(TOUCH_REG_P1, p, sizeof(p)) != ESP_OK) {
        return;
    }
    int rx = ((p[0] & 0x0F) << 8) | p[1];
    int ry = ((p[2] & 0x0F) << 8) | p[3];
    /* Raw touch reports in the panel's portrait frame; map it into the
     * landscape view. Same transform xiaozhi-esp32 applies to this panel:
     * swap the axes, then mirror both. */
    int x = LCD_H_RES - 1 - ry;
    int y = LCD_V_RES - 1 - rx;
    if (x < 0) x = 0;
    else if (x > LCD_H_RES - 1) x = LCD_H_RES - 1;
    if (y < 0) y = 0;
    else if (y > LCD_V_RES - 1) y = LCD_V_RES - 1;
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
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
        .miso_io_num = LCD_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,         /* the expander holds CS low */
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
        .reset_gpio_num = GPIO_NUM_NC,      /* reset came from the expander */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7796(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);   /* landscape, as xiaozhi-esp32 does */
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
            /* Two 480x32-line RGB565 buffers would take ~60 KB of internal
             * RAM the I2S DMA descriptors also need; park them in PSRAM. */
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }

    /* The FT6336 sits on the shared I2C bus with no reset or interrupt line;
     * poll it like the FNK0104B does. */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, touch_read_cb);
    *touch = indev;

    if (esp_lv_adapter_start() != ESP_OK) {
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

/* One ES8311 does both directions over a duplex I2S bus, MCLK from its pin. */
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
        .pa_pin = GPIO_NUM_NC,              /* no amp GPIO; the speaker is always enabled */
        .use_mclk = true,
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
    return muse_gpio_button_poll(&s_talk);
}

/* The AXP2101 has a battery gauge: reg 0xA4 is the percentage, reg 0x01
 * bits [6:5] report charging (1) or discharging (2). */
static esp_err_t read_power(muse_power_t *out)
{
    uint8_t pct = 0, status = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, 0xA4, &pct), TAG, "pmic pct");
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, 0x01, &status), TAG, "pmic status");
    out->battery_pct = pct > 100 ? 100 : pct;
    out->battery_mv = 0;
    out->charging = ((status >> 5) & 0x3) == 1;
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
    .name = "Waveshare ESP32-S3-Touch-LCD-3.5",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 3.5f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_TOP_LEFT, 8, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
