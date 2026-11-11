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
 * LCDWIKI 2.8inch ESP32-S3 Display, ES3C28P (capacitive touch) and ES3N28P
 * (no touch): ESP32-S3R8 with 16 MB flash and 8 MB octal PSRAM, a 2.8"
 * 240x320 ILI9341V LCD on SPI, used in landscape (320x240), an FT6336G touch
 * controller on the C model, an ES8311 codec with one mic and an FM8002E
 * speaker amp, and a battery on an ADC pin. BOOT (GPIO0) is the only button
 * the ESP32 sees: RESET resets it.
 *
 * One firmware serves both models: it looks for the FT6336 at boot. Without
 * it there's no settings screen, and with one button there's no menu either:
 * push-to-talk only, set up from the Muse app.
 *
 * Pins and the amp's polarity are from LCDWIKI's page for the board
 * (lcdwiki.com/2.8inch_ESP32-S3_Display) and from its xiaozhi-esp32 board
 * files, config.h and xiaozhi_ai_iot_vietnam_es3n28p_lcd_2.8.cc, which also
 * give the panel's orientation and the battery's ADC table. The 40 MHz SPI
 * limit and the touch panel's orientation are as measured on the board in
 * github.com/jvduuren/esphome-es3c28p-light-panel. CREDITS.md at the
 * repository root lists these sources and their licenses.
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
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* Landscape, the USB-C port on the right: the panel's 320-row axis runs across. */
#define LCD_W 320
#define LCD_H 240
/* The display's pins are SPI2's own, so they skip the GPIO matrix. */
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_12
#define LCD_MOSI GPIO_NUM_11
#define LCD_CS GPIO_NUM_10
#define LCD_DC GPIO_NUM_46
#define LCD_BL GPIO_NUM_45      /* the panel's reset is the ESP32's own */
/* 80 MHz, the next step up, flips bits in the pixels on this board. */
#define LCD_HZ (40 * 1000 * 1000)
#define DRAW_BUF_LINES 60       /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_W * 8 * 2)

#define I2C_SDA GPIO_NUM_16
#define I2C_SCL GPIO_NUM_15
#define TP_RST GPIO_NUM_18
#define TP_ADDR 0x38
#define TP_REG_POINTS 0x02      /* finger count, then X and Y, high byte first */
#define TP_REG_POWER 0xA5       /* 3: hibernate until the reset line pulses */
/*
 * The FT6336 reports in the panel's own portrait plane, 240 across and 320
 * down. In this landscape the screen's X is the panel's Y and the screen's Y
 * runs against the panel's X. These undo that: swap first, then mirror, in
 * the screen's axes.
 */
#define TP_SWAP_XY 1
#define TP_MIRROR_X 0
#define TP_MIRROR_Y 1

#define I2S_MCLK GPIO_NUM_4
#define I2S_BCLK GPIO_NUM_5
#define I2S_WS GPIO_NUM_7
#define I2S_DOUT GPIO_NUM_8
#define I2S_DIN GPIO_NUM_6
#define PA_EN GPIO_NUM_1        /* low: the amp is on */

#define BOOT_GPIO GPIO_NUM_0
#define BATT_ADC ADC_CHANNEL_8  /* GPIO9 */
/* Raw ADC readings (12 dB, 12 bits) for an empty and a full battery, from the
 * board's xiaozhi firmware, whose table is a straight line between them. */
#define BATT_RAW_EMPTY 1970
#define BATT_RAW_FULL 2430
#define BATT_RAW_ABSENT 1000    /* far under an empty cell: nothing on the connector */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp_dev;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;

static muse_board_t s_board;

static void touch_reset(void)
{
    gpio_set_level(TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));   /* the FT6336 answers nothing for the first 200 ms */
}

static esp_err_t init(void)
{
    /* power_off() holds the backlight off through deep sleep. */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(LCD_BL);

    /* The amp is on while its enable is low: keep it off until the codec
     * driver has the pin. */
    const gpio_config_t pa_cfg = { .pin_bit_mask = 1ULL << PA_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(PA_EN, 1);
    ESP_RETURN_ON_ERROR(gpio_config(&pa_cfg), TAG, "amp enable");

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    /* The C model has the touch panel; the N model leaves it out. */
    const gpio_config_t rst_cfg = { .pin_bit_mask = 1ULL << TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(TP_RST, 1);
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "touch reset");
    touch_reset();
    s_board.touch = i2c_master_probe(s_i2c, TP_ADDR, 50) == ESP_OK;
    s_board.touch_talk = s_board.touch;
    ESP_LOGI(TAG, "touch: %s", s_board.touch ? "FT6336" : "none (ES3N28P)");
    if (s_board.touch) {
        const i2c_device_config_t tp_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TP_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp_dev), TAG, "touch");
    }

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    return ESP_OK;
}

/* The FT6336 always answers, so it's asked on every pointer read rather than
 * through its interrupt line. LVGL's task calls this. */
static esp_err_t tp_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points, uint8_t *count,
                         uint8_t max_count, void *ctx)
{
    (void)tp;
    (void)max_count;
    (void)ctx;
    *count = 0;
    const uint8_t reg = TP_REG_POINTS;
    uint8_t d[5];
    if (i2c_master_transmit_receive(s_tp_dev, &reg, 1, d, sizeof(d), 5) != ESP_OK) {
        return ESP_OK;
    }
    int fingers = d[0] & 0x0F;
    if (fingers < 1 || fingers > 2) {
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
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    /* As the board's xiaozhi firmware sets it up for this landscape. */
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, false);
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
            .hor_res = LCD_W,
            .ver_res = LCD_H,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }

    if (s_board.touch) {
        /* The adapter wants a touch handle but, given a read of our own, only
         * looks at its interrupt pin: none, so it asks tp_read() on every
         * pointer read. */
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
        if (!*touch) {
            return NULL;
        }
    }
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

static void send_sleep(void *sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, *(bool *)sleep);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops, and with it the touch reads. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
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
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PA_EN,
        .pa_reverted = true,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    if (*mic) {
        /* ES8311 supports up to 42 dB PGA scale (ES8311_MIC_GAIN_42DB) */
        esp_codec_dev_set_in_gain(*mic, 42.0f);
    }
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    if (mic) {
        /* Ensure maximum sensitivity for clear voice recognition on ES8311 */
        float gain = db < 42 ? 42.0f : (float)db;
        esp_codec_dev_set_in_gain(mic, gain);
    }
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/*
 * The battery's level, from raw readings as the board's firmware maps them;
 * its divider isn't documented, so there's no voltage to report. The charger's
 * status reaches no pin, so the board counts as charging while a USB host is
 * attached and the battery isn't full; a plain USB charger looks like running
 * on battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    out->usb = usb_serial_jtag_is_connected();
    out->charging = false;
    out->battery_pct = -1;
    out->battery_mv = 0;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        sum += raw;
    }
    int raw = sum / 8;
    if (raw < BATT_RAW_ABSENT) {
        return ESP_OK;
    }
    int pct = (raw - BATT_RAW_EMPTY) * 100 / (BATT_RAW_FULL - BATT_RAW_EMPTY);
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->charging = out->usb && out->battery_pct < 100;
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
}

/*
 * Nothing here can cut the power. The screen and touch go off and the chip
 * deep-sleeps until BOOT is pressed, which restarts it. The backlight pin is
 * held low, or it would float while asleep.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    if (s_board.touch) {
        const uint8_t hibernate[] = { TP_REG_POWER, 0x03 };
        i2c_master_transmit(s_tp_dev, hibernate, sizeof(hibernate), 20);
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_hold_en(LCD_BL);
    gpio_deep_sleep_hold_en();
    while (gpio_get_level(BOOT_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT_GPIO, 0), TAG, "boot button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

/* Not const: init() fills in whether the touch panel is there. */
static muse_board_t s_board = {
    .name = "LCDWIKI 2.8inch ESP32-S3 Display",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 2.8f,
    .talk_button = "boot",
    .aux_button = "boot",
    /* BOOT is the only button, so there's no aux_hint: the screen sleeps on its
     * timer and Settings has the power page. BOOT is on the back, by the
     * right edge and below the USB-C port; its icon is at that edge, above
     * the captions. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, 20 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .set_mic_gain = set_mic_gain,
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
