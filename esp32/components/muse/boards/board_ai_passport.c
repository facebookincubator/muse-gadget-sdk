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
 * FoloToy AI Passport: ESP32-C3 (8 MB flash, no PSRAM), a 240x320 ST7789P3
 * panel on SPI2 with no touch controller, one ES8311 for speaker and mic on a
 * duplex I2S0 bus, three keys (UP/DOWN/OK) on one ADC resistor ladder, a
 * CW2017 fuel gauge, and a hardware power key that doesn't reach the MCU.
 *
 * Pins, bus settings, the panel's vendor init table and the ADC key windows
 * all come from FoloToy's BSP, the board's single source of truth
 * (github.com/FoloToy/ai-passport: components/bsp/include/bsp_pins.h for the
 * pins and windows, components/bsp/src/bsp_display.c for the ST7789P3
 * sequence, src/bsp_audio.c for the codec's I2S and ES8311 settings,
 * src/bsp_button.c for how the ladder is read, src/bsp_battery.c for the
 * CW2017 registers). Its hardware guide documents the rest: the panel is
 * portrait, needs the invert command, has no MISO (the panel can't be read),
 * and is reset in software (LCD RST is -1).
 *
 * Buttons with no touch: OK is Talk, and Select while the menu is open;
 * UP/DOWN step the menu (muse_input.c reads them on every board, keyboard
 * field or not). The aux button is the DOWN key: on a board without touch
 * muse_input.c maps it to "menu down", which is what its icon shows.
 *
 * No PSRAM, so Muse has no voice session of its own (MUSE_HATCH defaults
 * off): a voice note goes over Link's control session to the paired Muse and
 * the reply comes back as text. Images can't be shown either (the UI holds a
 * whole image at once). See devices/sdkconfig.muse-ai-passport.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
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

static const char *TAG = "board";

#define LCD_W 240
#define LCD_H 320
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_8
#define LCD_MOSI GPIO_NUM_9
#define LCD_CS GPIO_NUM_1
#define LCD_DC GPIO_NUM_20
#define LCD_RST GPIO_NUM_NC        /* hardwired to 3.3 V: reset goes over SWRESET */
#define LCD_BL GPIO_NUM_21
#define LCD_PCLK_HZ (80 * 1000 * 1000)
#define BL_FREQ_HZ 5000            /* the BSP's LEDC backlight: 5 kHz, 10-bit */
/* 240 x 8 x 2 = 3.8 KB: one small draw buffer, single because internal RAM is
 * the binding constraint here (no PSRAM, and Wi-Fi, BLE, the codec's DMA
 * buffers and Muse's widgets all share it). LVGL flushes in more, smaller
 * pieces; the avatar's frames are small ones anyway. */
#define DRAW_BUF_LINES 8

#define I2C_SDA GPIO_NUM_10
#define I2C_SCL GPIO_NUM_7
#define CW2017_ADDR 0x63
#define CW2017_REG_VERSION 0x00
#define CW2017_REG_VCELL 0x02      /* 14-bit, 312.5 uV a count */
#define CW2017_REG_SOC 0x04        /* high byte: whole percent */

#define I2S_MCLK GPIO_NUM_6
#define I2S_BCLK GPIO_NUM_5
#define I2S_WS GPIO_NUM_3
#define I2S_DOUT GPIO_NUM_2
#define I2S_DIN GPIO_NUM_4
#define PA_PIN (-1)                /* the amp is always on: no MCU pin for it */

#define BTN_ADC_UNIT ADC_UNIT_1
#define BTN_ADC_CHANNEL ADC_CHANNEL_0   /* GPIO0, the three keys' shared node */
#define BTN_GPIO GPIO_NUM_0             /* the same pin, for the deep-sleep wake */
#define BTN_SAMPLES 4                   /* averaged per reading */
#define BTN_DEBOUNCE 3                  /* readings in a row before a key counts */
#define BTN_RELEASED_MV 1900            /* above the OK window: nothing pressed */

/* UP / DOWN / OK, each a window on the ladder; the boundaries are the midpoints
 * between neighbouring steps (see BSP_BTN_MV_TABLE in bsp_pins.h). */
enum { KEY_UP = 0, KEY_DOWN = 1, KEY_OK = 2, KEY_COUNT = 3, KEY_NONE = -1 };
static const struct {
    int lo, hi;
} KEY_MV[KEY_COUNT] = { { 0, 150 }, { 150, 447 }, { 447, 1900 } };

/*
 * The ST7789P3 vendor sequence (porch, power, gamma) from FoloToy's BSP
 * (components/bsp/src/bsp_display.c), which took it from the panel vendor's
 * reference example. These are not generic ST7789 defaults: a different panel
 * needs its own vendor's table. SLPOUT/COLMOD (esp_lcd_panel_init), INVON
 * (invert_color), DISPON (disp_on_off) and MADCTL (mirror) are done by
 * esp_lcd and are not repeated here.
 */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint16_t delay_ms;
} st_init_cmd_t;

static const st_init_cmd_t ST7789P3_CMDS[] = {
    { 0xB2, { 0x05, 0x05, 0x00, 0x33, 0x33 }, 5, 0 },   /* PORCTRL porch */
    { 0xB7, { 0x35 }, 1, 0 },                           /* GCTRL gate */
    { 0xBB, { 0x21 }, 1, 0 },                           /* VCOMS */
    { 0xC0, { 0x2C }, 1, 0 },                           /* LCMCTRL */
    { 0xC2, { 0x01 }, 1, 0 },                           /* VDVVRHEN */
    { 0xC3, { 0x0B }, 1, 0 },                           /* VRHS */
    { 0xC4, { 0x20 }, 1, 0 },                           /* VDVSET */
    { 0xC6, { 0x0F }, 1, 0 },                           /* FRCTRL2 60 Hz dot inversion */
    { 0xD0, { 0xA7, 0xA1 }, 2, 0 },                     /* PWCTRL1 */
    { 0xD0, { 0xA4, 0xA1 }, 2, 0 },                     /* PWCTRL1: the vendor example resends it */
    { 0xD6, { 0xA1 }, 1, 0 },
    { 0xE0, { 0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
              0x49, 0x09, 0x16, 0x15, 0x26, 0x2B }, 14, 0 },   /* PVGAMCTRL */
    { 0xE1, { 0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
              0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A }, 14, 10 },  /* NVGAMCTRL */
};

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_gauge;
static esp_lcd_panel_handle_t s_panel;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_key = KEY_NONE;       /* last accepted key */
static int s_key_raw = KEY_NONE;   /* last reading */
static int s_key_stable;           /* readings in a row of s_key_raw */

/* One reading in millivolts, or -1 if it can't be read or doesn't fit a key.
 * Averaged: the ladder's steps are 150 mV apart at the bottom. */
static int key_read_mv(void)
{
    int sum = 0;
    for (int i = 0; i < BTN_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BTN_ADC_CHANNEL, &raw) != ESP_OK) {
            return -1;
        }
        sum += raw;
    }
    int mv;
    if (adc_cali_raw_to_voltage(s_cali, sum / BTN_SAMPLES, &mv) != ESP_OK) {
        return -1;
    }
    return mv;
}

static int key_read(void)
{
    int mv = key_read_mv();
    if (mv < 0 || mv >= BTN_RELEASED_MV) {
        return KEY_NONE;
    }
    for (int k = 0; k < KEY_COUNT; k++) {
        if (mv >= KEY_MV[k].lo && mv < KEY_MV[k].hi) {
            return k;
        }
    }
    /* Between windows (a barely-inserted contact): hold the last key rather
     * than reporting a phantom one. */
    return s_key;
}

static esp_err_t init(void)
{
    /* Every cause is a bit: BIT(ESP_SLEEP_WAKEUP_UNDEFINED) is a plain reset,
     * BIT(ESP_SLEEP_WAKEUP_GPIO) a key that woke the board from the deep sleep
     * power_off() leaves it in. */
    uint32_t wake = esp_sleep_get_wakeup_causes();
    ESP_LOGI(TAG, "wakeup causes 0x%02X (%s)", (unsigned)wake,
             wake & BIT(ESP_SLEEP_WAKEUP_GPIO) ? "key press" : "cold boot");

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    /* The gauge shares the bus with the codec at 100 kHz: 400 kHz was flaky on
     * the BSP's six-device bus with only the internal pull-ups. */
    const i2c_device_config_t gauge_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CW2017_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c, &gauge_cfg, &s_gauge) != ESP_OK) {
        ESP_LOGW(TAG, "no CW2017: battery unavailable");
        s_gauge = NULL;
    } else {
        uint8_t ver;
        const uint8_t reg = CW2017_REG_VERSION;
        if (i2c_master_transmit_receive(s_gauge, &reg, 1, &ver, 1, 100) != ESP_OK) {
            ESP_LOGW(TAG, "CW2017 at 0x%02X does not answer: battery unavailable", CW2017_ADDR);
            i2c_master_bus_rm_device(s_gauge);
            s_gauge = NULL;
        } else {
            ESP_LOGI(TAG, "CW2017 version 0x%02X", ver);
        }
    }

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = BTN_ADC_UNIT };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    /* 12 dB reaches the released state's 3.3 V; the windows are the BSP's. */
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BTN_ADC_CHANNEL, &ch_cfg), TAG, "key adc");
    const adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = BTN_ADC_UNIT,
        .chan = BTN_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali), TAG, "adc calibration");
    /* A key held while the board boots stays down: don't read it as a press. */
    s_key = s_key_raw = key_read();
    ESP_LOGI(TAG, "keys up: ADC1_CH%d on GPIO0, %d mV", BTN_ADC_CHANNEL, key_read_mv());
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BL_FREQ_HZ,
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
        .miso_io_num = GPIO_NUM_NC,   /* the panel has no MISO: it can't be read */
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
        .spi_mode = 0,                /* SCK idles low, sampled on the rising edge */
        .pclk_hz = LCD_PCLK_HZ,
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
    /* Portrait 240x320, as the panel and the UI are laid out. */
    esp_lcd_panel_reset(s_panel);     /* SWRESET: RST isn't on a GPIO */
    esp_lcd_panel_init(s_panel);
    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const st_init_cmd_t *c = &ST7789P3_CMDS[i];
        if (esp_lcd_panel_io_tx_param(io, c->cmd, c->data, c->len) != ESP_OK) {
            ESP_LOGE(TAG, "panel command 0x%02X failed", c->cmd);
            return NULL;
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    esp_lcd_panel_invert_color(s_panel, true);   /* this panel ships inverted */
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
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
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    ESP_LOGI(TAG, "display ready %dx%d, internal RAM free %u largest %u", LCD_W, LCD_H,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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

/*
 * One ES8311 does both directions over a duplex I2S bus, clocked from MCLK.
 * The codec settings are the BSP's (src/bsp_audio.c): MCLK in, codec as
 * slave.
 */
#define AUDIO_TRIES 12          /* up to ~4 s: about how long a Wi-Fi scan holds RAM */
#define AUDIO_RETRY_MS 350

/* Duplex I2S, both channels up, or an error with nothing left behind. */
static esp_err_t i2s_open(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* Smaller DMA buffers than the IDF defaults: by the time the codec opens,
     * the display's draw buffer and LVGL's widgets have most of internal RAM,
     * and 6 x 240 frames a direction no longer fits. 3 x 160 is 30 ms a
     * direction, above the 20 ms voice chunk size. */
    chan_cfg.dma_desc_num = 3;
    chan_cfg.dma_frame_num = 160;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, tx, rx), TAG, "i2s channel");
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
    esp_err_t err = i2s_channel_init_std_mode(*tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(*rx, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(*tx);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(*rx);
    }
    if (err != ESP_OK) {
        /* Its DMA buffers are allocated per channel and fail when internal
         * RAM is momentarily short; hand both back and let the caller retry. */
        i2s_del_channel(*tx);
        i2s_del_channel(*rx);
        *tx = *rx = NULL;
    }
    return err;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    /*
     * The Wi-Fi scan at boot holds internal RAM (dynamic RX buffers, 1.6 KB
     * each) for a second or two, and the I2S DMA buffers have to come from
     * that same RAM. Rather than leave the voice pipeline dead for the rest
     * of the session -- muse_voice_start doesn't retry -- try again until the
     * scan is done.
     */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; !tx && attempt <= AUDIO_TRIES; attempt++) {
        err = i2s_open(&tx, &rx);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "i2s not up yet (%s); internal RAM free %u largest %u, retry %d/%d",
                 esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), attempt, AUDIO_TRIES);
        vTaskDelay(pdMS_TO_TICKS(AUDIO_RETRY_MS));
    }
    ESP_RETURN_ON_ERROR(err, TAG, "i2s: no DMA RAM");

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
        .pa_pin = PA_PIN,
        .master_mode = false,
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

/* One ladder: a new key is accepted after three equal readings (30 ms at the
 * input task's 10 ms), and only on the change, so a held key doesn't repeat.
 * Talk needs both edges; the menu keys only presses. */
static unsigned poll_buttons(void)
{
    int raw = key_read();
    if (raw == s_key_raw) {
        if (s_key_stable < BTN_DEBOUNCE) {
            s_key_stable++;
        }
    } else {
        s_key_raw = raw;
        s_key_stable = 1;
    }
    if (s_key_stable < BTN_DEBOUNCE || raw == s_key) {
        return 0;
    }
    int prev = s_key;
    /* The ladder's voltages, for a board with reworked resistors: the windows
     * above come from FoloToy's BSP and the whole ladder is one ADC channel. */
    ESP_LOGD(TAG, "key %d -> %d (%d mV)", prev, raw, key_read_mv());
    s_key = raw;
    unsigned ev = 0;
    if (prev == KEY_OK && s_key != KEY_OK) {
        ev |= MUSE_BTN_TALK_RELEASE;
    }
    if (s_key == KEY_OK && prev != KEY_OK) {
        ev |= MUSE_BTN_TALK_PRESS;
    }
    /* The board contract is edge-triggered: a held ADC key must not enqueue
     * another menu movement on every 10 ms input poll. */
    if (s_key == KEY_UP) {
        ev |= MUSE_BTN_UP;
    } else if (s_key == KEY_DOWN) {
        ev |= MUSE_BTN_DOWN;
    }
    return ev;
}

/* Battery: the CW2017's own state of charge, and the cell voltage it measures.
 * This BSP exposes no VBUS/charger-detect signal, so do not claim USB power
 * based on the unrelated USB Serial/JTAG host-connection state. */
static esp_err_t read_power(muse_power_t *out)
{
    out->usb = false;       /* unknown on this BSP; never infer it from console */
    out->charging = false;  /* no charger-detect signal is exposed */
    out->battery_pct = -1;
    out->battery_mv = 0;
    if (!s_gauge) {
        return ESP_OK;
    }
    uint8_t soc[2] = { 0 };
    uint8_t reg = CW2017_REG_SOC;
    if (i2c_master_transmit_receive(s_gauge, &reg, 1, soc, 2, 100) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t mv[2] = { 0 };
    reg = CW2017_REG_VCELL;
    if (i2c_master_transmit_receive(s_gauge, &reg, 1, mv, 2, 100) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    out->battery_mv = (int)((((mv[0] << 8) | mv[1]) & 0x3FFF) * 3125 / 10000);
    /* 0xFF while the gauge hasn't finished its first conversion. */
    if (soc[0] <= 100) {
        out->battery_pct = soc[0];
    }
    return ESP_OK;
}

/*
 * The board's power key is hardware: it holds the rail up and cuts it after a
 * couple of seconds, so the firmware can't switch the board off. What it can
 * do is save what it needs and sleep; any of the three keys (they all pull
 * the ladder node low) wakes it and the board boots again.
 *
 * ESP32-C3 has no RTC IO subsystem, so neither EXT0 nor EXT1 exists: the
 * deep-sleep wakeup is the GPIO wakeup source (SOC_GPIO_SUPPORT_HP_PERIPH_
 * PD_SLEEP_WAKEUP). Its internal pull-ups stay off
 * (CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=n): the ladder's 10 k
 * external pull-up is what sets the key voltages.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        panel_sleep(true);   /* SLPIN: a panel left displaying draws ~1 mA */
    }
    if (s_gauge) {
        i2c_master_bus_rm_device(s_gauge);   /* keep the gauge quiet over I2C */
        s_gauge = NULL;
    }
    /* Do not strand the board awake if a key contact stalls between ladder
     * windows or the ADC calibration fails. Require stable release samples,
     * but bound the wait so a faulty ladder still reaches deep sleep. */
    int released = 0;
    for (int tries = 0; s_key != KEY_NONE && tries < 100; tries++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int mv = key_read_mv();
        if (mv >= BTN_RELEASED_MV) {
            if (++released >= BTN_DEBOUNCE) {
                s_key = KEY_NONE;
            }
        } else {
            released = 0;
            if (mv >= 0) {
                s_key = key_read();
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT64(BTN_GPIO), ESP_GPIO_WAKEUP_GPIO_LOW),
                        TAG, "key wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "FoloToy AI Passport",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    /* FoloToy's specs give the resolution (240x320) but not the diagonal; the
     * UI doesn't use this field. */
    .diagonal_in = 2.0f,
    .talk_button = "OK",
    .aux_button = "DOWN",
    /* UP / DOWN / OK along the bottom edge, left to right (the ladder's order
     * in bsp_pins.h, which the BSP's demo page prints in the same order);
     * talk_hint is the mic and sits by OK, aux_hint the menu icon by DOWN. */
    .talk_hint = { LV_ALIGN_BOTTOM_RIGHT, -18, -4 },
    .aux_hint = { LV_ALIGN_BOTTOM_MID, 0, -4 },
    .frame_ms = 50,             /* a single core, and the panel shares the bus */
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    /* No display_pause/wait_buttons: light sleep's CPU state costs ~8.5 KB of
     * internal RAM, which this board (no PSRAM) can't spare. */
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
