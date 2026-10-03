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
 * M5Stack StopWatch (C152): ESP32-S3R8 (16 MB flash, 8 MB octal PSRAM), round
 * 1.75" 466 px CO5300 AMOLED on QSPI with CST820 touch, ES8311 codec with one
 * mic and an AW8737A speaker amp, two buttons on the rim (yellow upper left,
 * blue upper right) and a power button wired to the power chip.
 *
 * Everything shares one I2C bus (GPIO 47/48). M5's power chip (M5PM1, 0x6E)
 * measures the battery and USB; its GPIO2 is the charger's CHG_STAT. M5's IO
 * expander (M5IOE1, 0x4F) switches the rest: IO3 audio power, IO4 touch reset,
 * IO5 panel reset, IO8 the 3V3_L3B rail, IO9 the vibration motor, IO10 the amp.
 *
 * Pins and registers are from M5Unified and M5GFX (board_M5StopWatch:
 * M5Unified.inl, Power_Class.inl, M5PM1_Class.inl, M5IOE1_Class.inl; M5GFX.cpp
 * Panel_StopWatch) and M5's factory firmware, M5StopWatch-UserDemo at 6b4aa125
 * (main/hal/). The panel's 466 columns start at 6, as on the Waveshare 1.75C;
 * the factory firmware hands touch coordinates to LVGL unscaled.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 466
#define LCD_GAP_X 6                /* the 466 columns in the controller's 480 */
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_39
#define LCD_SCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_41
#define LCD_D1 GPIO_NUM_42
#define LCD_D2 GPIO_NUM_46
#define LCD_D3 GPIO_NUM_45
#define DRAW_BUF_LINES 118         /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define I2S_MCLK GPIO_NUM_18
#define I2S_BCLK GPIO_NUM_17
#define I2S_WS GPIO_NUM_15
#define I2S_DOUT GPIO_NUM_21
#define I2S_DIN GPIO_NUM_16

#define TALK_GPIO GPIO_NUM_2       /* KEYA, yellow, upper left */
#define AUX_GPIO GPIO_NUM_1        /* KEYB, blue, upper right */

#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each */
#define TP_REG_SLEEP 0xE5          /* 0x03: deep sleep until reset */
#define TP_REG_NO_AUTO_SLEEP 0xFE  /* else it stops answering I2C when idle */

#define PMIC_ADDR 0x6E
#define PMIC_PWR_CFG 0x06
#define PMIC_PWR_5V_OUT 0x08       /* 5 V boost to the Grove port */
#define PMIC_I2C_CFG 0x09          /* 0: no I2C idle sleep */
#define PMIC_WDT 0x0A              /* 0: watchdog off */
#define PMIC_GPIO_MODE 0x10        /* 1 = output */
#define PMIC_GPIO_OUT 0x11
#define PMIC_GPIO_IN 0x12
#define PMIC_GPIO_OD 0x13          /* 1 = open drain */
#define PMIC_GPIO_FUNC0 0x16       /* two bits per GPIO 0-3; 0 = GPIO */
#define PMIC_CHG_STAT BIT(2)       /* low while charging */
#define PMIC_CHG_PROG BIT(3)       /* driven low, as M5's firmware does */
#define PMIC_VBAT_MV 0x22          /* 16-bit little-endian millivolts */
#define PMIC_VIN_MV 0x24           /* USB */

#define IOE_ADDR 0x4F
#define IOE_MODE 0x03              /* 16 bits, IO1 in bit 0; 1 = output */
#define IOE_OUT 0x05
#define IOE_IN 0x07
#define IOE_OD 0x13                /* 1 = open drain */
#define IOE_PWM1_DUTY 0x1B         /* the motor's PWM; bit 15 enables */
#define IOE_I2C_CFG 0x23           /* 0: no I2C idle sleep */
#define IOE_PIN(n) ((uint16_t)BIT((n) - 1))
#define IOE_AUDIO IOE_PIN(3)
#define IOE_TP_RST IOE_PIN(4)
#define IOE_LCD_RST IOE_PIN(5)
#define IOE_L3B IOE_PIN(8)
#define IOE_MOTOR IOE_PIN(9)
#define IOE_SPK IOE_PIN(10)
#define IOE_OUTPUTS (IOE_AUDIO | IOE_TP_RST | IOE_LCD_RST | IOE_L3B | IOE_MOTOR | IOE_SPK)
#define SPK_PA_PIN 10              /* es8311's pa_pin, routed to the expander by s_ioe_gpio */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pmic, s_ioe, s_tp;
static SemaphoreHandle_t s_ioe_lock;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk, s_aux;

/* Both chips are microcontrollers and now and then miss a transfer; retry. */
static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50);
    }
    return err;
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *val, size_t n)
{
    uint8_t buf[3] = { reg };
    assert(n < sizeof(buf));
    memcpy(buf + 1, val, n);
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit(dev, buf, n + 1, 50);
    }
    return err;
}

static esp_err_t pmic_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, reg, &v, 1), TAG, "pmic read %02x", reg);
    uint8_t want = (v & ~mask) | (bits & mask);
    return want == v ? ESP_OK : reg_write(s_pmic, reg, &want, 1);
}

static int pmic_mv(uint8_t reg)
{
    uint8_t b[2];
    return reg_read(s_pmic, reg, b, sizeof(b)) == ESP_OK ? b[0] | b[1] << 8 : 0;
}

/* The expander's registers come in pairs, IO1-8 then IO9-14. The amp (audio
 * task), touch reset (input task) and power-off all change them, so one at a
 * time. */
static esp_err_t ioe_update(uint8_t reg, uint16_t mask, uint16_t bits)
{
    xSemaphoreTake(s_ioe_lock, portMAX_DELAY);
    uint8_t b[2];
    esp_err_t err = reg_read(s_ioe, reg, b, sizeof(b));
    if (err == ESP_OK) {
        uint16_t v = b[0] | b[1] << 8;
        uint16_t want = (v & ~mask) | (bits & mask);
        if (want != v) {
            b[0] = want;
            b[1] = want >> 8;
            err = reg_write(s_ioe, reg, b, sizeof(b));
        }
    }
    xSemaphoreGive(s_ioe_lock);
    return err;
}

static esp_err_t ioe_set(uint16_t pins, bool high)
{
    return ioe_update(IOE_OUT, pins, high ? pins : 0);
}

/* The codec driver switches the amp through this as the speaker opens and closes. */
static int ioe_gpio_setup(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t mode)
{
    (void)gpio;
    (void)dir;
    (void)mode;
    return ESP_CODEC_DEV_OK;
}

static int ioe_gpio_set(int16_t gpio, bool high)
{
    (void)gpio;
    return ioe_set(IOE_SPK, high) == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static bool ioe_gpio_get(int16_t gpio)
{
    (void)gpio;
    uint8_t b[2] = { 0 };
    reg_read(s_ioe, IOE_OUT, b, sizeof(b));
    return (b[0] | b[1] << 8) & IOE_SPK;
}

static const audio_codec_gpio_if_t s_ioe_gpio = {
    .setup = ioe_gpio_setup,
    .set = ioe_gpio_set,
    .get = ioe_gpio_get,
};

static esp_err_t add_device(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_i2c, &cfg, dev);
}

/* Reset pulse on the touch controller, then keep it answering while idle. */
static void tp_reset(void)
{
    ioe_set(IOE_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioe_set(IOE_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (reg_write(s_tp, TP_REG_NO_AUTO_SLEEP, (uint8_t[]){ 0x01 }, 1) != ESP_OK) {
        ESP_LOGW(TAG, "touch not answering");
    }
}

static esp_err_t init(void)
{
    s_ioe_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ioe_lock, ESP_ERR_NO_MEM, TAG, "ioe lock");
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(add_device(PMIC_ADDR, 100000, &s_pmic), TAG, "pmic");
    ESP_RETURN_ON_ERROR(add_device(IOE_ADDR, 100000, &s_ioe), TAG, "ioe");
    ESP_RETURN_ON_ERROR(add_device(TP_ADDR, 400000, &s_tp), TAG, "touch");

    /* The power chip keeps its settings while the ESP32 resets, and M5's
     * firmware may have left its I2C sleep or watchdog on; M5GFX clears both
     * on every boot. CHG_STAT is an input, CHG_PROG low as M5's firmware has
     * it, and the 5 V boost off: nothing Muse uses hangs off the Grove port. */
    ESP_RETURN_ON_ERROR(reg_write(s_pmic, PMIC_I2C_CFG, (uint8_t[]){ 0 }, 1), TAG, "pmic i2c sleep off");
    ESP_RETURN_ON_ERROR(reg_write(s_pmic, PMIC_WDT, (uint8_t[]){ 0 }, 1), TAG, "pmic watchdog off");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_FUNC0, 0xF0, 0), TAG, "pmic gpio2-3 function");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OD, PMIC_CHG_PROG, 0), TAG, "pmic gpio3 push-pull");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_CHG_PROG, 0), TAG, "pmic gpio3 low");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_MODE, PMIC_CHG_STAT | PMIC_CHG_PROG, PMIC_CHG_PROG), TAG, "pmic gpio mode");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_PWR_CFG, PMIC_PWR_5V_OUT, 0), TAG, "5 V out off");

    /* Motor and amp off, audio and the L3B rail on, and both resets released
     * (driven push-pull). Then reset the panel and touch together, as M5GFX
     * does. The expander's own MUX select (IO1) is left as it is. */
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_I2C_CFG, (uint8_t[]){ 0 }, 1), TAG, "ioe i2c sleep off");
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_PWM1_DUTY, (uint8_t[]){ 0, 0 }, 2), TAG, "motor pwm off");
    ESP_RETURN_ON_ERROR(ioe_update(IOE_OD, IOE_OUTPUTS, 0), TAG, "ioe push-pull");
    ESP_RETURN_ON_ERROR(ioe_update(IOE_OUT, IOE_OUTPUTS, IOE_AUDIO | IOE_TP_RST | IOE_LCD_RST | IOE_L3B), TAG,
                        "ioe levels");
    ESP_RETURN_ON_ERROR(ioe_update(IOE_MODE, IOE_OUTPUTS, IOE_OUTPUTS), TAG, "ioe outputs");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_set(IOE_TP_RST | IOE_LCD_RST, false), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_set(IOE_TP_RST | IOE_LCD_RST, true), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(120));   /* the panel's wake from reset */

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");
    /* A button that woke the board from power-off is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_aux.pressed = gpio_get_level(AUX_GPIO) == 0;
    return ESP_OK;
}

/* M5GFX's Panel_StopWatch init, less the tearing line: COLMOD and MADCTL come
 * from the driver first. */
static const co5300_lcd_init_cmd_t s_lcd_init[] = {
    { 0x11, NULL, 0, 150 },                     /* sleep out */
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },        /* brightness control on */
    { 0x51, (uint8_t[]){ 0xA0 }, 1, 0 },        /* brightness, until the UI sets its own */
    { 0x29, NULL, 0, 0 },                       /* display on */
};

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* LVGL polls this from its own task. The CST820 reports one point. */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (reg_read(s_tp, TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = (b[1] & 0x0F) << 8 | b[2];
    int y = (b[3] & 0x0F) << 8 | b[4];
    data->point.x = x < LCD_RES ? x : LCD_RES - 1;
    data->point.y = y < LCD_RES ? y : LCD_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* As on the Waveshare 1.75C, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        CO5300_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* the expander's IO5, pulsed in init() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
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
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    tp_reset();
    *touch = lv_indev_create();
    if (!*touch) {
        return NULL;
    }
    lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(*touch, disp);
    lv_indev_set_read_cb(*touch, tp_read);
    if (esp_lv_adapter_start() != ESP_OK) {
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
    /* CO5300 "write display brightness" (0x51). */
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

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the Waveshare 1.75C. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops and the CST820 sleeps until its reset line wakes it. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        reg_write(s_tp, TP_REG_SLEEP, (uint8_t[]){ 0x03 }, 1);
    } else {
        tp_reset();
        esp_lv_adapter_resume();
    }
}

/* One ES8311 does both directions over a duplex I2S bus, clocked from MCLK. */
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
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = &s_ioe_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = SPK_PA_PIN,
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
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_aux }, 2, timeout_ms);
}

/* The power chip measures the battery and USB; the charger's CHG_STAT reaches its GPIO2. */
static esp_err_t read_power(muse_power_t *out)
{
    uint8_t in;
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, PMIC_GPIO_IN, &in, 1), TAG, "pmic read");
    int v = pmic_mv(PMIC_VBAT_MV);
    out->usb = pmic_mv(PMIC_VIN_MV) > 4000;
    out->charging = out->usb && !(in & PMIC_CHG_STAT);
    out->battery_mv = v;
    if (v < 2500) {
        out->battery_pct = -1;
        return ESP_OK;
    }
    /* The Watcher's LiPo curve, as on the StickS3. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

/*
 * Screen, touch, audio and the L3B rail off, then deep sleep until either
 * button is pressed. Double-clicking the power button has the power chip cut
 * the rest, and a click turns it back on.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    /* Best effort: with the screen already off on battery (display_pause) the
     * touch controller is asleep and doesn't answer. */
    reg_write(s_tp, TP_REG_SLEEP, (uint8_t[]){ 0x03 }, 1);
    /* Sleeping with these still powered would drain the battery. */
    ESP_RETURN_ON_ERROR(ioe_set(IOE_SPK | IOE_AUDIO | IOE_L3B, false), TAG, "rails off");
    while (gpio_get_level(TALK_GPIO) == 0 || gpio_get_level(AUX_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    /* M5's firmware pulls the buttons up internally, so keep those pull-ups
     * powered through deep sleep. */
    const gpio_num_t keys[] = { TALK_GPIO, AUX_GPIO };
    for (int i = 0; i < 2; i++) {
        rtc_gpio_pullup_en(keys[i]);
        rtc_gpio_pulldown_dis(keys[i]);
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(TALK_GPIO) | BIT64(AUX_GPIO), ESP_EXT1_WAKEUP_ANY_LOW),
                        TAG, "button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "M5Stack StopWatch",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.75f,
    .talk_button = "yellow",
    .aux_button = "blue",
    /* The buttons sit on the rim either side of 12 o'clock, about 27 degrees
     * off it, where M5's stopwatch app draws them. */
    .talk_hint = { LV_ALIGN_CENTER, -91, -178 },
    .aux_hint = { LV_ALIGN_CENTER, 91, -178 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
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
