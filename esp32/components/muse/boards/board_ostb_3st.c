/*
 * Copyright (c) 2026 ledienbien-ai
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
 * OSTB-3ST ("ostb-xiaozhi-3st"): ESP32-S3 with a 1.83" 240x296 NV3023 LCD on
 * SPI, used in landscape (296x240), a CST816 touch controller with no
 * interrupt line, an ES8311 DAC and ES7210 ADC on one duplex I2S port, two
 * volume keys on the top edge, a charge-status pin and a pin that cuts the
 * board's power.
 *
 * Everything here about the hardware (pins, the panel's setup table and
 * orientation, the battery's ADC table) is from the board's xiaozhi-esp32
 * configuration: config.h, ostb-xiaozhi-3st.cc and power_manager.h.
 * CREDITS.md at the repository root lists that source and its license.
 *
 * Once it has its setup table the NV3023 takes the usual MIPI window and
 * memory-write commands, so the panel is a few functions here rather than a
 * dependency on a driver component.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* Landscape: the panel's 296-row axis runs across. */
#define LCD_W 296
#define LCD_H 240
#define LCD_GAP_X 24            /* the glass shows rows 24 to 319 of the controller's 320 */
/* Rows and columns exchanged, rows mirrored: xiaozhi's swap_xy and mirror_y. */
#define LCD_MADCTL (LCD_CMD_MV_BIT | LCD_CMD_MY_BIT)
#define LCD_HOST SPI3_HOST
#define LCD_MOSI GPIO_NUM_10
#define LCD_SCLK GPIO_NUM_9
#define LCD_DC GPIO_NUM_8
#define LCD_CS GPIO_NUM_14
#define LCD_RST GPIO_NUM_18     /* the touch controller's reset too */
#define LCD_BL GPIO_NUM_13
#define LCD_HZ (40 * 1000 * 1000)
#define DRAW_BUF_LINES 60       /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_W * 8 * 2)

#define I2C_SDA GPIO_NUM_12
#define I2C_SCL GPIO_NUM_11
#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02      /* finger count, then X and Y, high byte first */
/* What the controller reports against the screen as Muse draws it. xiaozhi
 * passes its coordinates straight through, so none of these is set. */
#define TP_SWAP_XY 0
#define TP_MIRROR_X 0
#define TP_MIRROR_Y 0

#define I2S_MCLK GPIO_NUM_5
#define I2S_BCLK GPIO_NUM_15
#define I2S_WS GPIO_NUM_16
#define I2S_DOUT GPIO_NUM_6
#define I2S_DIN GPIO_NUM_7
#define PA_EN GPIO_NUM_4

#define TALK_GPIO GPIO_NUM_39   /* volume up, the + key */
#define AUX_GPIO GPIO_NUM_40    /* volume down, the - key */
#define CHG_GPIO GPIO_NUM_47    /* the charger's status: low while charging */
#define PWR_OFF_GPIO GPIO_NUM_3 /* high: the board switches itself off */
#define BATT_ADC ADC_CHANNEL_6  /* GPIO17, on ADC2 */
/* The 4G modem's UART, on the pins the console's UART comes up on. */
#define MODEM_TXD GPIO_NUM_43
#define MODEM_RXD GPIO_NUM_44

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint16_t delay_ms;
    const uint8_t *data;
} lcd_cmd_t;

#define LCD_SETUP(c, ms, ...) { (c), sizeof((uint8_t[]){ __VA_ARGS__ }), (ms), (const uint8_t[]){ __VA_ARGS__ } }

/* The panel maker's setup, as the board's xiaozhi firmware sends it. */
static const lcd_cmd_t s_lcd_setup[] = {
    LCD_SETUP(0xDF, 0, 0x98, 0x53),
    LCD_SETUP(0xDE, 0, 0x00),
    LCD_SETUP(0xB2, 0, 0x25),
    LCD_SETUP(0xB7, 0, 0x00, 0x29, 0x00, 0x51),
    LCD_SETUP(0xBB, 0, 0x4F, 0x1A, 0x55, 0x73, 0x63, 0xF0),
    LCD_SETUP(0xC0, 0, 0x44, 0xA4),
    LCD_SETUP(0xC1, 0, 0x12),
    LCD_SETUP(0xC3, 0, 0x7D, 0x07, 0x14, 0x06, 0xC8, 0x71, 0x6C, 0x77),
    /* 296 lines */
    LCD_SETUP(0xC4, 0, 0x00, 0x00, 0x94, 0x79, 0x25, 0x0A, 0x16, 0x79, 0x25, 0x0A, 0x16, 0x82),
    /* gamma */
    LCD_SETUP(0xC8, 0, 0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00,
              0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00),
    LCD_SETUP(0xD0, 0, 0x04, 0x06, 0x6B, 0x0F, 0x00),
    LCD_SETUP(0xD7, 0, 0x00, 0x30),
    LCD_SETUP(0xE6, 0, 0x10),
    LCD_SETUP(0xDE, 0, 0x01),
    LCD_SETUP(0xB7, 0, 0x03, 0x13, 0xEF, 0x35, 0x35),
    LCD_SETUP(0xC1, 0, 0x14, 0x15, 0xC0),
    LCD_SETUP(0xC2, 0, 0x06, 0x3A),
    LCD_SETUP(0xC4, 0, 0x72, 0x12),
    LCD_SETUP(0xBE, 0, 0x00),
    LCD_SETUP(0xDE, 0, 0x00),
    LCD_SETUP(LCD_CMD_TEON, 0, 0x00),
    LCD_SETUP(LCD_CMD_COLMOD, 0, 0x05),     /* 16 bits a pixel */
    LCD_SETUP(LCD_CMD_CASET, 0, 0x00, 0x00, 0x00, 0xEF),
    LCD_SETUP(LCD_CMD_RASET, 0, 0x00, 0x00, 0x01, 0x27),
    { LCD_CMD_SLPOUT, 0, 120, NULL },
    LCD_SETUP(LCD_CMD_MADCTL, 0, LCD_MADCTL),
    { LCD_CMD_INVOFF, 0, 0, NULL },
};

/* Raw ADC readings (12 dB, 12 bits) against charge, from the board's firmware. */
static const struct {
    uint16_t raw;
    uint8_t pct;
} s_batt_curve[] = {
    { 1890, 0 },  { 1950, 10 }, { 2010, 20 }, { 2070, 30 }, { 2130, 40 }, { 2190, 50 },
    { 2250, 60 }, { 2310, 70 }, { 2370, 80 }, { 2430, 90 }, { 2460, 95 }, { 2489, 100 },
};
#define BATT_RAW_ABSENT 1000    /* far under an empty cell: nothing on the connector */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp_dev;
static esp_lcd_panel_io_handle_t s_io;
static muse_gpio_button_t s_talk, s_aux;
static adc_oneshot_unit_handle_t s_adc;
static int s_batt_raw = -1;     /* the last reading that worked */

static esp_err_t init(void)
{
    const gpio_config_t off_cfg = { .pin_bit_mask = 1ULL << PWR_OFF_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(PWR_OFF_GPIO, 0);
    ESP_RETURN_ON_ERROR(gpio_config(&off_cfg), TAG, "power-off pin");
    const gpio_config_t chg_cfg = {
        .pin_bit_mask = 1ULL << CHG_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&chg_cfg), TAG, "charge pin");
    /* Home Link's console UART starts on the modem's pins, its TX against the
     * modem's. Let go of both: the console is on the chip's USB as well. */
    gpio_reset_pin(MODEM_TXD);
    gpio_reset_pin(MODEM_RXD);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp_dev), TAG, "touch");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_2 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    return ESP_OK;
}

static void lcd_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
}

/* The one thing LVGL's side asks of a panel: a window, then its pixels. */
static esp_err_t lcd_draw(esp_lcd_panel_t *panel, int x1, int y1, int x2, int y2, const void *data)
{
    (void)panel;
    size_t bytes = (size_t)(x2 - x1) * (y2 - y1) * 2;
    x1 += LCD_GAP_X;
    x2 += LCD_GAP_X - 1;
    y2 -= 1;
    const uint8_t cols[] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    const uint8_t rows[] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, cols, sizeof(cols)), TAG, "columns");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, rows, sizeof(rows)), TAG, "rows");
    return esp_lcd_panel_io_tx_color(s_io, LCD_CMD_RAMWR, data, bytes);
}

static esp_lcd_panel_t s_panel = { .draw_bitmap = lcd_draw };

/*
 * The CST816 has no interrupt line here, so it's asked on every pointer read.
 * Idle, it dozes and doesn't answer; that reads as no finger. LVGL's task
 * calls this.
 */
static esp_err_t tp_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points, uint8_t *count,
                         uint8_t max_count, void *ctx)
{
    (void)tp;
    (void)max_count;
    (void)ctx;
    *count = 0;
    const uint8_t reg = TP_REG_POINTS;
    uint8_t d[5];
    if (i2c_master_transmit_receive(s_tp_dev, &reg, 1, d, sizeof(d), 20) != ESP_OK || !(d[0] & 0x0F)) {
        return ESP_OK;
    }
    int x = (d[1] & 0x0F) << 8 | d[2];
    int y = (d[3] & 0x0F) << 8 | d[4];
    if (TP_SWAP_XY) {
        int t = x;
        x = y;
        y = t;
    }
    x = x < LCD_W ? x : LCD_W - 1;
    y = y < LCD_H ? y : LCD_H - 1;
    points[0] = (esp_lcd_touch_point_data_t){
        .x = TP_MIRROR_X ? LCD_W - 1 - x : x,
        .y = TP_MIRROR_Y ? LCD_H - 1 - y : y,
    };
    *count = 1;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
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

    /* One line resets the panel and the touch controller. */
    const gpio_config_t rst_cfg = { .pin_bit_mask = 1ULL << LCD_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(LCD_RST, 1);
    if (gpio_config(&rst_cfg) != ESP_OK) {
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_lcd_setup) / sizeof(s_lcd_setup[0]); i++) {
        const lcd_cmd_t *c = &s_lcd_setup[i];
        if (esp_lcd_panel_io_tx_param(s_io, c->cmd, c->data, c->len) != ESP_OK) {
            return NULL;
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    lcd_cmd(LCD_CMD_DISPON);    /* the backlight is still off: what's in the panel's memory doesn't show */

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = &s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }

    if (i2c_master_probe(s_i2c, TP_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not answering yet");
    }
    /* The adapter wants a touch handle but, given a read of our own, only looks
     * at its interrupt pin: none, so it asks tp_read() on every pointer read. */
    static esp_lcd_touch_t tp = {
        .config = {
            .x_max = LCD_W,
            .y_max = LCD_H,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
        },
    };
    esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, &tp);
    lv_tp_cfg.callbacks.custom_touch_read = tp_read;
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
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

static void send_sleep(void *sleep)
{
    lcd_cmd(*(bool *)sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops, and with it the touch reads. The touch controller
 * is left alone: only the reset line would wake it, and that resets the
 * panel too. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* ES8311 out, ES7210 in, one duplex bus clocked from MCLK. */
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
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    /* xiaozhi runs the ES7210 with four TDM slots: the mic on input 1 and the
     * speaker's signal, for echo cancelling, on input 3. A 2-slot bus carries
     * inputs 1 and 2, and Muse listens to the first. */
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
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
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied.
     * esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_aux }, 2, timeout_ms);
}

static int batt_pct(int raw)
{
    const int n = sizeof(s_batt_curve) / sizeof(s_batt_curve[0]);
    if (raw <= s_batt_curve[0].raw) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (raw < s_batt_curve[i].raw) {
            int span = s_batt_curve[i].raw - s_batt_curve[i - 1].raw;
            int rise = s_batt_curve[i].pct - s_batt_curve[i - 1].pct;
            return s_batt_curve[i - 1].pct + (raw - s_batt_curve[i - 1].raw) * rise / span;
        }
    }
    return 100;
}

/*
 * The battery is on ADC2, which Wi-Fi uses too: a read can time out while
 * the radio has it, so the last good one stands. The firmware's table is in
 * raw readings and the divider isn't documented, so there's no voltage to
 * report. The charger's status pin says charging only until the battery is
 * full; after that, USB shows only while a host is attached.
 */
static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BATT_ADC, &raw) == ESP_OK) {
            sum += raw;
            n++;
        }
    }
    if (n) {
        s_batt_raw = sum / n;
    }
    out->charging = gpio_get_level(CHG_GPIO) == 0;
    out->usb = out->charging || usb_serial_jtag_is_connected();
    out->battery_mv = 0;
    out->battery_pct = s_batt_raw < BATT_RAW_ABSENT ? -1 : batt_pct(s_batt_raw);
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    lcd_cmd(LCD_CMD_DISPOFF);
    lcd_cmd(LCD_CMD_SLPIN);
}

/*
 * The power-off pin switches the board off, as its own firmware does. On USB
 * power it may stay up: then the screen stays off until the + key is
 * pressed, which restarts it.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    gpio_set_level(PWR_OFF_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (gpio_get_level(TALK_GPIO) != 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "OSTB-3ST",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 1.83f,
    .talk_button = "+",
    .aux_button = "-",
    /* The keys are in a row on the top edge, as the maker's pictures show the
     * case: + on the left, - on the right, and between them a key this
     * firmware doesn't read. Their icons go under them, at either end of the
     * status line, which has the middle of the top edge. */
    .talk_hint = { LV_ALIGN_TOP_LEFT, 36, 4 },
    .aux_hint = { LV_ALIGN_TOP_RIGHT, -36, 4 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* the mic is the ES7210's input 1, the left slot */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
