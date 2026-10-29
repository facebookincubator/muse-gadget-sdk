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
 * Waveshare ESP32-S3-Touch-LCD-1.46/1.46B/1.46C: round 412 px SPD2010
 * QSPI LCD and I2C touch, PCM5101 speaker DAC, digital I2S microphone and a
 * TCA9554 expander for the panel and touch resets. Pins and peripheral setup
 * follow Waveshare's ESP32-S3-Touch-LCD-1.46 ESP-IDF example:
 * https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.46
 */
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 412
#define LCD_HOST SPI2_HOST
#define LCD_PCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_CS GPIO_NUM_21
#define LCD_BL GPIO_NUM_5
#define DRAW_BUF_LINES 104
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define TOUCH_INT GPIO_NUM_4
#define TCA9554_ADDR 0x20
#define TCA9554_OUTPUT_REG 0x01
#define TCA9554_CONFIG_REG 0x03
#define EXIO_TOUCH_RST BIT(0)
#define EXIO_LCD_RST BIT(1)

#define TALK_GPIO GPIO_NUM_0
#define POWER_GPIO GPIO_NUM_6
#define POWER_HOLD_GPIO GPIO_NUM_7

#define SPK_BCLK GPIO_NUM_48
#define SPK_WS GPIO_NUM_38
#define SPK_DOUT GPIO_NUM_47
#define MIC_BCLK GPIO_NUM_15
#define MIC_WS GPIO_NUM_2
#define MIC_DIN GPIO_NUM_39

#define BATT_ADC ADC_CHANNEL_7 /* GPIO8, through a 200K/100K divider */
#define TP_POLL_MS 10
#define TP_IDLE_POLL_MS 30
#define TP_LIFT_MS 35

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tca;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_lcd_io;
static muse_gpio_button_t s_talk;
static muse_gpio_button_t s_power;
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static int s_mic_gain_q8 = 256;
static int32_t s_mic_dc[2];
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static TaskHandle_t s_tp_task;
static portMUX_TYPE s_tp_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_lcd_touch_point_data_t s_tp_point;
static bool s_tp_down;
static bool s_paused;

static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    return i2c_master_transmit(s_tca, (uint8_t[]){ reg, val }, 2, -1);
}

static esp_err_t tca_reset_pulse(uint8_t bit, int low_ms, int high_ms)
{
    uint8_t out;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(
                            s_tca, (uint8_t[]){ TCA9554_OUTPUT_REG }, 1, &out, 1, -1),
                        TAG, "tca read");
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_OUTPUT_REG, out & ~bit), TAG, "tca reset low");
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_OUTPUT_REG, out | bit), TAG, "tca reset high");
    vTaskDelay(pdMS_TO_TICKS(high_ms));
    return ESP_OK;
}

static esp_err_t init(void)
{
    const gpio_config_t hold = {
        .pin_bit_mask = 1ULL << POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&hold), TAG, "power hold");
    ESP_RETURN_ON_ERROR(gpio_set_level(POWER_HOLD_GPIO, 1), TAG, "power hold on");

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t tca_cfg = {
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tca_cfg, &s_tca), TAG, "tca9554");
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_CONFIG_REG, 0x00), TAG, "tca config");
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_OUTPUT_REG, 0xFF), TAG, "tca outputs");
    ESP_RETURN_ON_ERROR(tca_reset_pulse(EXIO_LCD_RST, 100, 100), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(tca_reset_pulse(EXIO_TOUCH_RST, 50, 50), TAG, "touch reset");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "boot button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_power, POWER_GPIO), TAG, "power button");
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_power.pressed = gpio_get_level(POWER_GPIO) == 0;

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &chan_cfg), TAG, "adc channel");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable");
    }
#endif
    return ESP_OK;
}

/* SPD2010 update windows must start and end on four-column boundaries. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~3;
    a->x2 |= 3;
}

/* IDF 6 rejects the touch driver's zero-length write before a plain read. */
static esp_err_t (*s_tp_rx)(esp_lcd_panel_io_t *, int, void *, size_t);

static esp_err_t tp_rx_param(esp_lcd_panel_io_t *io, int cmd, void *param, size_t size)
{
    (void)cmd;
    return s_tp_rx(io, -1, param, size);
}

static void poll_touch(void *arg)
{
    esp_lcd_touch_handle_t tp = arg;
    esp_lcd_touch_point_data_t point;
    int64_t last_us = 0;
    for (;;) {
        if (s_paused) {
            taskENTER_CRITICAL(&s_tp_lock);
            s_tp_down = false;
            taskEXIT_CRITICAL(&s_tp_lock);
            last_us = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        uint8_t count = 0;
        if (esp_lcd_touch_read_data(tp) == ESP_OK) {
            esp_lcd_touch_get_data(tp, &point, &count, 1);
        }
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_tp_lock);
        if (count) {
            s_tp_point = point;
            last_us = now;
        } else if (last_us && now - last_us >= TP_LIFT_MS * 1000) {
            last_us = 0;
        }
        s_tp_down = last_us != 0;
        taskEXIT_CRITICAL(&s_tp_lock);
        vTaskDelay(pdMS_TO_TICKS(last_us ? TP_POLL_MS : TP_IDLE_POLL_MS));
    }
}

static esp_err_t tp_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points,
                         uint8_t *count, uint8_t max_count, void *ctx)
{
    (void)tp;
    (void)max_count;
    (void)ctx;
    taskENTER_CRITICAL(&s_tp_lock);
    points[0] = s_tp_point;
    *count = s_tp_down;
    taskEXIT_CRITICAL(&s_tp_lock);
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t spi =
        SPD2010_PANEL_BUS_QSPI_CONFIG(LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &spi, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = SPD2010_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_lcd_io) != ESP_OK) {
        return NULL;
    }
    spd2010_vendor_config_t vendor_cfg = { .flags.use_qspi_interface = 1 };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_spd2010(s_lcd_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t display_cfg = {
        .panel = s_panel,
        .panel_io = s_lcd_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = muse_lcd_bands_register(display_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!display) {
        return NULL;
    }
    lv_display_add_event_cb(display, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    s_tp_rx = tp_io->rx_param;
    tp_io->rx_param = tp_rx_param;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_INT,
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_spd2010(tp_io, &tp_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(tp);
    if (xTaskCreatePinnedToCore(poll_touch, "touch", 3072, tp, MUSE_UI_PRIORITY,
                                &s_tp_task, !MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, tp);
    lv_tp_cfg.callbacks.custom_touch_read = tp_read;
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return display;
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

static void send_sleep(void *arg)
{
    bool sleep = *(bool *)arg;
    esp_lcd_panel_io_tx_param(s_lcd_io, (0x02 << 24) | ((sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void display_pause(bool pause)
{
    s_paused = pause;
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        if (s_tp_task) {
            xTaskNotifyGive(s_tp_task);
        }
        esp_lv_adapter_resume();
    }
}

static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
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
    return i2s_channel_write(s_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK &&
                   wrote == (size_t)size
               ? ESP_CODEC_DEV_OK
               : ESP_CODEC_DEV_WRITE_FAIL;
}

static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t got;
    if (i2s_channel_read(s_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK ||
        got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    int16_t *samples = (int16_t *)data;
    for (int i = 0; i < size / 2; i++) {
        int32_t *dc = &s_mic_dc[i & 1];
        *dc += (samples[i] * 256 - *dc) >> 8;
        int value = (samples[i] - (*dc >> 8)) * s_mic_gain_q8 >> 8;
        samples[i] = value > INT16_MAX ? INT16_MAX : value < INT16_MIN ? INT16_MIN : value;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, db / 20.0f));
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* PCM5101 (SCK grounded): PLL from BCK/LRCK. Use 32-bit Philips slots so
     * BCK is 64×fs — what Waveshare's IDF example and most ESP32 PCM5101
     * boards run. Only OUTL feeds the NS8002 amp (R57 is its gain trimmer). */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &s_tx, NULL), TAG, "speaker i2s");
    i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    spk_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    spk_cfg.slot_cfg.ws_width = 32;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &spk_cfg), TAG, "speaker mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "speaker on");

    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_cfg, NULL, &s_rx), TAG, "microphone i2s");
    i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN,
        },
    };
    mic_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    mic_cfg.slot_cfg.ws_width = 32;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &mic_cfg), TAG, "microphone mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "microphone on");

    static const audio_codec_data_if_t spk_if = { .enable = data_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = data_enable, .read = mic_read };
    *spk = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if });
    *mic = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if });
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    unsigned events = muse_gpio_button_poll(&s_talk);
    unsigned power = muse_gpio_button_poll(&s_power);
    if (power & MUSE_BTN_TALK_PRESS) {
        events |= MUSE_BTN_POWER_PRESS;
    }
    if (power & MUSE_BTN_TALK_RELEASE) {
        events |= MUSE_BTN_POWER_RELEASE;
    }
    return events;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_power }, 2, timeout_ms);
}

static esp_err_t read_power(muse_power_t *out)
{
    int raw;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "battery ADC");
    int mv;
    if (s_cali) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "battery calibration");
    } else {
        mv = raw * 3300 / 4096;
    }
    /* Waveshare's reference uses a 3:1 divider and a measured 0.990476 correction. */
    out->battery_mv = (int)((int64_t)mv * 3000000 / 990476);
    int pct = (out->battery_mv - 3300) * 100 / 900;
    out->battery_pct = out->battery_mv < 2500 ? -1 : pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->charging = false;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    gpio_set_level(POWER_HOLD_GPIO, 0);
    while (gpio_get_level(POWER_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_sleep_enable_ext0_wakeup(POWER_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.46B",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.46f,
    .talk_button = "BOOT",
    .power_button = "PWR",
    .talk_hint = { LV_ALIGN_LEFT_MID, 18, 0 },
    .power_hint = { LV_ALIGN_RIGHT_MID, -18, 0 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
