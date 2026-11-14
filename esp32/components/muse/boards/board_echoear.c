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
 * Espressif EchoEar (sold since as ESP-VoCat): an ESP32-S3-WROOM module with
 * 16 MB of octal PSRAM, a round 360 px ST77916 LCD on QSPI with a PWM
 * backlight, CST816S touch, an ES8311 DAC into an NS4150B amplifier, an
 * ES7210 ADC with two microphones, a BQ27220 gauge on the battery and touch
 * pads (copper foil under the shell). BOOT (GPIO0) is the only key the ESP32
 * reads: RESET resets it and the power key latches the supply in hardware.
 * BOOT is on the back, so a hand on a pad is the talk button too.
 *
 * Two versions of the board share this firmware, told apart at boot as
 * xiaozhi-esp32 does it: v1.2 powers its codecs from a rail that GPIO48
 * switches, so its ES8311 answers on I2C only once that pin is high.
 *
 *            v1.0 (WROOM-2, 32 MB octal flash)   v1.2 (WROOM-1, 16 MB flash)
 *   I2S DIN  GPIO15                              GPIO3
 *   Amp on   GPIO4                               GPIO15
 *   LCD RST  GPIO3, low resets                   GPIO47, high resets
 *   Pads     one, GPIO7                          two, GPIO6 and GPIO7
 *
 * Pins are from Espressif's BSP for the v1.2 board (esp-bsp, bsp/esp_vocat),
 * its user guides (docs.espressif.com/projects/esp-dev-kits, EchoEar v1.0 and
 * v1.2) and xiaozhi-esp32's echoear board, which covers both versions.
 * CREDITS.md at the repository root lists these sources and their licenses.
 *
 * Not used: the BMI270 motion sensor, the microSD slot, the green LED on the
 * microphone board and the serial port on the magnetic connector.
 */
#include <stdlib.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/touch_sens.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
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
#include "echoear_lcd_init.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_PCLK GPIO_NUM_18
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_13
#define LCD_D2 GPIO_NUM_11
#define LCD_D3 GPIO_NUM_12
#define LCD_CS GPIO_NUM_14
#define LCD_BL GPIO_NUM_44          /* also UART0's RX pin, which is let go of */
#define LCD_POWER GPIO_NUM_9        /* the panel's and the SD card's supply; low is on */
#define LCD_RST_V10 GPIO_NUM_3
#define LCD_RST_V12 GPIO_NUM_47
#define LCD_HZ (40 * 1000 * 1000)   /* the driver's default; Espressif's BSP runs it at 80 MHz */
#define DRAW_BUF_LINES 90           /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_2
#define I2C_SCL GPIO_NUM_1
#define TP_INT GPIO_NUM_10          /* pulses low with each touch report */
#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02          /* finger count, then X and Y, high byte first */
#define TP_REG_AUTO_SLEEP 0xFE      /* 1: stay awake between touches */

#define CODEC_POWER GPIO_NUM_48     /* v1.2: the codecs' supply; high is on */
#define I2S_MCLK GPIO_NUM_42
#define I2S_BCLK GPIO_NUM_40
#define I2S_WS GPIO_NUM_39
#define I2S_DOUT GPIO_NUM_41
#define I2S_DIN_V10 GPIO_NUM_15
#define I2S_DIN_V12 GPIO_NUM_3
#define PA_EN_V10 GPIO_NUM_4
#define PA_EN_V12 GPIO_NUM_15

#define BOOT_GPIO GPIO_NUM_0

/* Touch pads under the shell. On the ESP32-S3, touch channel n is GPIO n. */
#define PAD_CHAN 7
#define PAD_CHAN_V12 6              /* v1.0 has no pad here: the pin is UART1's TX */
#define PAD_MAX 2
#define PAD_THRESH 150              /* in 0.01 %: a hand raises the reading by over 1.5 %, as Espressif's BSP takes it */
#define PAD_MIN_REST 1000           /* a reading under this isn't a pad's */
#define PAD_DEBOUNCE 3              /* polls, 10 ms apart */
#define PAD_STUCK_MS 20000          /* past the longest recording: something is lying on the pad */
#define PAD_LOW_MS 1000             /* this long under its rest level: the level is wrong */
#define LED_GPIO GPIO_NUM_43        /* green, lit while low; also UART0's TX pin */

/* BQ27220 fuel gauge: 16-bit registers, low byte first. */
#define GAUGE_ADDR 0x55
#define GAUGE_REG_VOLTAGE 0x08      /* mV */
#define GAUGE_REG_CURRENT 0x0C      /* mA, signed: positive into the battery */
#define GAUGE_REG_SOC 0x2C          /* % */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp_dev;
static i2c_master_dev_handle_t s_gauge;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_boot;
typedef struct {
    touch_channel_handle_t chan;
    int gpio;
    bool pressed;
    uint8_t count;                  /* polls its reading has said otherwise */
    TickType_t rested;              /* when it last read near its rest level */
    int peak;                       /* the furthest above it in this touch, in 0.01 % */
} pad_t;
static touch_sensor_handle_t s_touch;
static pad_t s_pads[PAD_MAX];
static int s_pad_count;
static TaskHandle_t s_pad_waiter;   /* the task in wait_buttons() */
static enum { TALK_UP, TALK_PAD, TALK_KEY } s_talk;
static bool s_v12;                  /* the v1.2 board rather than v1.0 */
/* Which board this is, kept through restarts and deep sleep (not through
 * power-off): it can only be told while the codecs' rail is off. */
#define VERSION_V10 0x45453130u
#define VERSION_V12 0x45453132u
static RTC_NOINIT_ATTR uint32_t s_version;
static volatile bool s_tp_irq;      /* the touch controller has a report */
static bool s_tp_down;

static esp_err_t tp_write(uint8_t reg, uint8_t v)
{
    const uint8_t buf[] = { reg, v };
    return i2c_master_transmit(s_tp_dev, buf, sizeof(buf), 20);
}

static esp_err_t output_pin(gpio_num_t pin, int level)
{
    const gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(pin, level);
    return gpio_config(&cfg);
}

static bool es8311_answers(void)
{
    return i2c_master_probe(s_i2c, ES8311_CODEC_DEFAULT_ADDR >> 1, 100) == ESP_OK;
}

/*
 * Which board: with the codecs' rail off (the pin is low by now), only v1.0's
 * ES8311 answers, since v1.0 has no such rail. After a restart a v1.2's rail
 * may still be falling, so it gets a second to stop answering before this
 * takes the board for a v1.0.
 */
static uint32_t detect_version(void)
{
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!es8311_answers()) {
            gpio_set_level(CODEC_POWER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!es8311_answers()) {
                ESP_LOGE(TAG, "no ES8311 with the codec rail off or on: taking this for v1.2");
            }
            return VERSION_V12;
        }
    }
    return VERSION_V10;
}

/* A pad took a hand, by the sensor's own count: out of wait_buttons(). */
static bool IRAM_ATTR on_pad_active(touch_sensor_handle_t sens, const touch_active_event_data_t *event, void *ctx)
{
    (void)sens;
    (void)event;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    if (s_pad_waiter) {
        vTaskNotifyGiveFromISR(s_pad_waiter, &woken);
    }
    return woken == pdTRUE;
}

static esp_err_t pads_start(void)
{
    /* The controller, the channels and the filter are set up as Espressif's
     * BSP has them (bsp/esp_vocat, bsp_button.c), and the order is ESP-IDF's
     * touch_sens_sleep example. */
    touch_sensor_sample_config_t sample[] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
    };
    const touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample);
    ESP_RETURN_ON_ERROR(touch_sensor_new_controller(&sens_cfg, &s_touch), TAG, "touch sensor");
    touch_channel_config_t chan_cfg = {
        .active_thresh = { 2000 },   /* until the pad's rest level is known */
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    const int chans[PAD_MAX] = { PAD_CHAN, PAD_CHAN_V12 };
    int n = s_v12 ? 2 : 1;
    for (int i = 0; i < n; i++) {
        s_pads[i].gpio = chans[i];
        ESP_RETURN_ON_ERROR(touch_sensor_new_channel(s_touch, chans[i], &chan_cfg, &s_pads[i].chan), TAG,
                            "touch channel %d", chans[i]);
    }
    const touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_RETURN_ON_ERROR(touch_sensor_config_filter(s_touch, &filter_cfg), TAG, "touch filter");

    /* A few scans to learn each pad's rest level. The sensor then counts a
     * pad as touched at the same 1.5 % above it that poll_pad() does, which
     * is when it wakes the chip from light sleep. */
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "touch enable");
    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_trigger_oneshot_scanning(s_touch, 2000), TAG, "touch scan");
    }
    ESP_RETURN_ON_ERROR(touch_sensor_disable(s_touch), TAG, "touch disable");
    for (int i = 0; i < n; i++) {
        uint32_t rest = 0;
        ESP_RETURN_ON_ERROR(touch_channel_read_data(s_pads[i].chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &rest), TAG,
                            "touch read");
        ESP_RETURN_ON_FALSE(rest >= PAD_MIN_REST, ESP_ERR_INVALID_RESPONSE, TAG, "touch pad on GPIO%d reads %lu",
                            chans[i], (unsigned long)rest);
        chan_cfg.active_thresh[0] = (uint32_t)((uint64_t)rest * PAD_THRESH / 10000);
        ESP_RETURN_ON_ERROR(touch_sensor_reconfig_channel(s_pads[i].chan, &chan_cfg), TAG, "touch threshold");
        ESP_LOGI(TAG, "touch pad on GPIO%d rests at %lu, touched from +%lu", chans[i], (unsigned long)rest,
                 (unsigned long)chan_cfg.active_thresh[0]);
    }
    const touch_event_callbacks_t callbacks = { .on_active = on_pad_active };
    ESP_RETURN_ON_ERROR(touch_sensor_register_callbacks(s_touch, &callbacks, NULL), TAG, "touch callback");
    /* Screen off and the chip in light sleep, a hand on a pad wakes it. */
    const touch_sleep_config_t sleep_cfg = TOUCH_SENSOR_DEFAULT_LSLP_CONFIG();
    ESP_RETURN_ON_ERROR(touch_sensor_config_sleep_wakeup(s_touch, &sleep_cfg), TAG, "touch wake");
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "touch enable");
    ESP_RETURN_ON_ERROR(touch_sensor_start_continuous_scanning(s_touch), TAG, "touch scanning");
    for (int i = 0; i < n; i++) {
        s_pads[i].rested = xTaskGetTickCount();
    }
    s_pad_count = n;
    return ESP_OK;
}

static void pads_off(void)
{
    touch_sensor_stop_continuous_scanning(s_touch);
    touch_sensor_disable(s_touch);
    touch_sensor_config_sleep_wakeup(s_touch, NULL);
}

/* Before deep sleep: the pads must not be what ends it. */
static void pads_stop(void)
{
    if (s_pad_count) {
        s_pad_count = 0;
        pads_off();
    }
}

/* Without the pads the board still works: BOOT talks. */
static void pads_init(void)
{
    if (pads_start() == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "no touch pads: BOOT is the talk button");
    if (s_touch) {
        /* Nothing is left that could wake the chip. A step pads_start()
         * didn't reach only logs that it has nothing to undo. */
        pads_off();
    }
}

/* How far the pad reads above its rest level, in 0.01 %; under it, below zero. */
static bool pad_read(const pad_t *p, int *up)
{
    uint32_t now = 0, rest = 0;
    if (touch_channel_read_data(p->chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, &now) != ESP_OK ||
        touch_channel_read_data(p->chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &rest) != ESP_OK || !rest) {
        return false;
    }
    int64_t d = ((int64_t)now - (int64_t)rest) * 10000 / (int64_t)rest;
    *up = d > INT16_MAX ? INT16_MAX : d < INT16_MIN ? INT16_MIN : (int)d;
    return true;
}

/* Lets go at half of what it takes to count as touched. */
static bool pad_touched(const pad_t *p, int up)
{
    return up > (p->pressed ? PAD_THRESH / 2 : PAD_THRESH);
}

/*
 * The sensor follows each pad's rest level by itself, also through light
 * sleep, but only while the reading stays near it. A reading that stays away
 * is put right here by taking it for the rest level. Above the level, that
 * is after PAD_STUCK_MS, so that something left lying on a pad doesn't hold
 * the talk button down for good. Under it, after PAD_LOW_MS: a hand only
 * raises the reading, so the level was taken with one on the pad (at
 * power-on, say).
 */
static void poll_pad(pad_t *p)
{
    int up;
    if (!pad_read(p, &up)) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (abs(up) <= PAD_THRESH / 4) {
        p->rested = now;
    } else if (now - p->rested >= pdMS_TO_TICKS(up < 0 ? PAD_LOW_MS : PAD_STUCK_MS)) {
        if (p->pressed) {
            ESP_LOGW(TAG, "touch pad on GPIO%d held too long: let go of", p->gpio);
        }
        const touch_chan_benchmark_config_t cfg = { .do_reset = true };
        touch_channel_config_benchmark(p->chan, &cfg);
        p->rested = now;
        return;   /* the next reading is against the new level */
    }
    if (pad_touched(p, up) == p->pressed) {
        p->count = 0;
        if (p->pressed && up > p->peak) {
            p->peak = up;
        }
        return;
    }
    if (++p->count < PAD_DEBOUNCE) {
        return;
    }
    p->count = 0;
    p->pressed = !p->pressed;
    if (p->pressed) {
        p->peak = up;
    } else {
        /* The figure to set PAD_THRESH by. */
        ESP_LOGI(TAG, "touch pad on GPIO%d: touched, up to +%d.%02d %%", p->gpio, p->peak / 100, p->peak % 100);
    }
}

static esp_err_t init(void)
{
    /* power_off() holds these through deep sleep. */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(LCD_BL);
    gpio_hold_dis(CODEC_POWER);
    rtc_gpio_hold_dis(LCD_POWER);
    rtc_gpio_deinit(LCD_POWER);

    /* Home Link's console UART starts on the backlight's pin and the LED's.
     * Let go of both: the console is on the chip's USB as well. */
    gpio_reset_pin(LCD_BL);
    gpio_reset_pin(LED_GPIO);
    ESP_RETURN_ON_ERROR(output_pin(LCD_BL, 0), TAG, "backlight");
    ESP_RETURN_ON_ERROR(output_pin(LED_GPIO, 1), TAG, "led");
    ESP_RETURN_ON_ERROR(output_pin(LCD_POWER, 0), TAG, "lcd power");

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
    const i2c_device_config_t gauge_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GAUGE_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &gauge_cfg, &s_gauge), TAG, "gauge");

    ESP_RETURN_ON_ERROR(output_pin(CODEC_POWER, 0), TAG, "codec power");
    if (s_version != VERSION_V10 && s_version != VERSION_V12) {
        s_version = detect_version();
    }
    s_v12 = s_version == VERSION_V12;
    gpio_set_level(CODEC_POWER, s_v12);
    vTaskDelay(pdMS_TO_TICKS(100));   /* the codecs' rail comes up */
    ESP_LOGI(TAG, "EchoEar %s", s_v12 ? "v1.2" : "v1.0");

    /* The amp stays off until the codec driver has its pin. */
    ESP_RETURN_ON_ERROR(output_pin(s_v12 ? PA_EN_V12 : PA_EN_V10, 0), TAG, "amp enable");
    pads_init();
    return muse_gpio_button_init(&s_boot, BOOT_GPIO);
}

static void IRAM_ATTR on_touch_line(void *arg)
{
    (void)arg;
    s_tp_irq = true;
}

/*
 * The CST816S is read only once it has pulsed its interrupt line, and from
 * then until it reports no finger: idle, it may be asleep and not answer on
 * I2C. LVGL's task calls this each time it reads the pointer.
 */
static esp_err_t tp_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points, uint8_t *count,
                         uint8_t max_count, void *ctx)
{
    (void)tp;
    (void)max_count;
    (void)ctx;
    *count = 0;
    if (!s_tp_irq && !s_tp_down) {
        return ESP_OK;
    }
    s_tp_irq = false;
    const uint8_t reg = TP_REG_POINTS;
    uint8_t d[5];
    if (i2c_master_transmit_receive(s_tp_dev, &reg, 1, d, sizeof(d), 20) != ESP_OK || !(d[0] & 0x0F)) {
        s_tp_down = false;
        return ESP_OK;
    }
    int x = (d[1] & 0x0F) << 8 | d[2];
    int y = (d[3] & 0x0F) << 8 | d[4];
    points[0] = (esp_lcd_touch_point_data_t){
        .x = x < LCD_RES ? x : LCD_RES - 1,
        .y = y < LCD_RES ? y : LCD_RES - 1,
    };
    *count = 1;
    s_tp_down = true;
    return ESP_OK;
}

/*
 * The controller has no reset line here, so nothing could wake it from its
 * deep sleep: it is only ever told whether to doze between touches. Awake, it
 * answers every read; dozing, it wakes at a touch and pulses its interrupt
 * line, which is all tp_read() waits for. A write it sleeps through leaves it
 * dozing, which works the same.
 */
static void touch_stay_awake(bool awake)
{
    if (tp_write(TP_REG_AUTO_SLEEP, awake) != ESP_OK && awake) {
        ESP_LOGW(TAG, "touch controller not answering");
    }
    s_tp_irq = false;
    s_tp_down = false;
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

    const spi_bus_config_t bus =
        ST77916_PANEL_BUS_QSPI_CONFIG(LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_HZ;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    /* The register setup is Espressif's for this panel (echoear_lcd_init.h);
     * xiaozhi-esp32 sends the same. */
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = disp_init_data,
        .init_cmds_size = sizeof(disp_init_data) / sizeof(disp_init_data[0]),
        .flags.use_qspi_interface = 1,
    };
    /* v1.2 resets the panel through a transistor, which turns the level over. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = s_v12 ? LCD_RST_V12 : LCD_RST_V10,
        .flags.reset_active_high = s_v12,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
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

    touch_stay_awake(true);
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    /* muse_gpio_button_init() installed the GPIO interrupt service. */
    if (gpio_config(&int_cfg) != ESP_OK || gpio_isr_handler_add(TP_INT, on_touch_line, NULL) != ESP_OK) {
        return NULL;
    }
    /* The adapter wants a touch handle but, given a read of our own, only looks
     * at its interrupt pin: none, so it asks tp_read() on every pointer read. */
    static esp_lcd_touch_t tp = {
        .config = {
            .x_max = LCD_RES,
            .y_max = LCD_RES,
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
    /* SLPIN/SLPOUT as a QSPI command write (opcode 02h), as the driver sends its own. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops, and the touch controller may doze. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        touch_stay_awake(false);
    } else {
        touch_stay_awake(true);
        esp_lv_adapter_resume();
    }
}

/* ES8311 out, ES7210 in, one duplex bus. */
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
            .din = s_v12 ? I2S_DIN_V12 : I2S_DIN_V10,
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

    /* As Espressif's BSP and xiaozhi-esp32 set the pair up; the amp enable is high for on. */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = s_v12 ? PA_EN_V12 : PA_EN_V10,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    /* Inputs 1 and 2, which a 2-slot bus carries. Input 1 is a microphone
     * (it's the one xiaozhi listens to); what's on input 2 isn't relied on. */
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

/*
 * BOOT and the pads are one talk button, and BOOT comes first: pressed while
 * a hand is on a pad, it ends that press and starts its own, since a key
 * press confirms a pairing and a touch doesn't (MUSE_BTN_TALK_TOUCH).
 */
static unsigned poll_buttons(void)
{
    muse_gpio_button_poll(&s_boot);
    int talk = s_boot.pressed ? TALK_KEY : TALK_UP;
    for (int i = 0; i < s_pad_count; i++) {
        poll_pad(&s_pads[i]);
        if (talk == TALK_UP && s_pads[i].pressed) {
            talk = TALK_PAD;
        }
    }
    int was = s_talk;
    s_talk = talk;
    if (talk == was || (was == TALK_KEY && talk == TALK_PAD)) {
        return 0;   /* BOOT let go with a hand still on a pad: still held */
    }
    if (talk == TALK_UP) {
        return MUSE_BTN_TALK_RELEASE;
    }
    if (talk == TALK_PAD) {
        return MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_TOUCH;
    }
    return was == TALK_PAD ? MUSE_BTN_TALK_RELEASE | MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_PRESS;
}

static void wait_buttons(int timeout_ms)
{
    s_pad_waiter = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < s_pad_count; i++) {
        int up;
        if (pad_read(&s_pads[i], &up) && pad_touched(&s_pads[i], up) != s_pads[i].pressed) {
            /* Mid-change: debounce at the awake poll rate. */
            vTaskDelay(pdMS_TO_TICKS(10));
            return;
        }
    }
    /* on_pad_active() ends this wait as a change on BOOT's line does. */
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

static bool gauge_read(uint8_t reg, uint16_t *out)
{
    uint8_t d[2];
    if (i2c_master_transmit_receive(s_gauge, &reg, 1, d, sizeof(d), 20) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)(d[1] << 8 | d[0]);
    return true;
}

/*
 * The gauge's own figures. Nothing tells the ESP32 that a charger is plugged
 * in, so the board counts as on USB while a USB host is attached or current
 * is flowing into the battery; on a plain charger with a full battery it
 * looks like running on battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    out->usb = usb_serial_jtag_is_connected();
    out->charging = false;
    out->battery_pct = -1;
    out->battery_mv = 0;
    uint16_t mv, ma, soc;
    if (!gauge_read(GAUGE_REG_VOLTAGE, &mv) || !gauge_read(GAUGE_REG_CURRENT, &ma) ||
        !gauge_read(GAUGE_REG_SOC, &soc)) {
        /* The gauge runs off the battery: without one it doesn't answer. */
        static bool said;
        if (!said) {
            ESP_LOGW(TAG, "battery gauge not answering: no battery level");
            said = true;
        }
        return ESP_OK;
    }
    if (mv < 2500 || mv > 5000) {
        return ESP_OK;   /* no battery on the connector */
    }
    out->battery_mv = mv;
    out->battery_pct = soc > 100 ? 100 : soc;
    out->charging = (int16_t)ma > 10;
    out->usb = out->usb || out->charging;
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

/*
 * The power key cuts the supply by itself; nothing here can. This turns off
 * what the ESP32 switches (the backlight, the panel's supply, and on v1.2 the
 * codecs') and deep-sleeps until BOOT is pressed, which restarts it: the pads
 * don't. The three pins are held where they are, or they would float while
 * asleep.
 */
static esp_err_t power_off(void)
{
    pads_stop();
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_hold_en(LCD_BL);
    gpio_set_level(s_v12 ? PA_EN_V12 : PA_EN_V10, 0);
    gpio_set_level(CODEC_POWER, 0);
    gpio_hold_en(CODEC_POWER);
    gpio_deep_sleep_hold_en();
    rtc_gpio_init(LCD_POWER);
    rtc_gpio_set_direction(LCD_POWER, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(LCD_POWER, 1);
    rtc_gpio_hold_en(LCD_POWER);
    while (gpio_get_level(BOOT_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT_GPIO, 0), TAG, "boot button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Espressif EchoEar",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .aux_button = "boot",
    /* BOOT is the only key, so there's no aux_hint: the screen sleeps on its
     * timer and Settings has the power page. The mic icon mirrors the speaker
     * button, under the state word on the right, as on the Waveshare 1.85C,
     * which has the same screen. */
    .talk_hint = { LV_ALIGN_CENTER, 99, -84 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,
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
