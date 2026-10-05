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
 * Seeed SenseCAP Watcher: ESP32-S3 with 8 MB PSRAM, round 412 px SPD2010 LCD
 * on QSPI with built-in touch, ES8311 speaker and ES7243E (older units:
 * ES7243) mic, a WS2813 RGB light, and a wheel in the top-right corner: a
 * rotary encoder on GPIOs with its push on a PCA9535 expander. The expander
 * also switches the power rails and reads the charger. The wheel's push powers
 * the board on, and the firmware then holds the system rail. The Himax AI
 * camera speaks SSCMA over SPI2, which it shares with the SD card. Its rail
 * stays off until a capture request, the live preview (components/camera) or
 * Muse's agent (muse_camera.h) needs it; the SD card and Grove rails stay off
 * until used. Touches, the wheel and the light also go to Muse's agent
 * (muse_hw.h). Pins follow Seeed's sensecap-watcher BSP
 * (SenseCAP-Watcher-Firmware) and xiaozhi-esp32's sensecap-watcher board.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_hw.h"

#if CONFIG_MUSE_WATCHER_CAMERA && CONFIG_MUSE_HW_COMMANDS
#error "One driver for the Himax per build: MUSE_HW_COMMANDS' camera.* or MUSE_WATCHER_CAMERA's"
#endif
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#include "camera.h"
#include "camera_sscma.h"
#endif

static const char *TAG = "board";

#define LCD_RES 412
#define LCD_HOST SPI3_HOST
#define LCD_PCLK GPIO_NUM_7
#define LCD_D0 GPIO_NUM_9
#define LCD_D1 GPIO_NUM_1
#define LCD_D2 GPIO_NUM_14
#define LCD_D3 GPIO_NUM_13
#define LCD_CS GPIO_NUM_45
#define LCD_BL GPIO_NUM_8
#define DRAW_BUF_LINES 104      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define TP_SDA GPIO_NUM_39     /* touch has its own I2C bus */
#define TP_SCL GPIO_NUM_38

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define I2S_MCLK GPIO_NUM_10
#define I2S_BCLK GPIO_NUM_11
#define I2S_WS GPIO_NUM_12
#define I2S_DIN GPIO_NUM_15
#define I2S_DOUT GPIO_NUM_16
#define ES7243_ADDR 0x13       /* 7-bit; newer units have an ES7243E at 0x14 */
#define ES7243E_ADDR 0x14

#define KNOB_A GPIO_NUM_41
#define KNOB_B GPIO_NUM_42
/* The encoder counts down turning clockwise (A leads B); -1 flips it if not. */
#define KNOB_CW_SIGN (-1)
#define RGB_LED GPIO_NUM_40
#define BATT_ADC ADC_CHANNEL_2     /* GPIO3, through a 62k/20k divider */

/* PCA9535 at 0x21: port 0 in the low byte, port 1 in the high byte. */
#define EXP_ADDR 0x21
#define EXP_INT GPIO_NUM_2         /* low when an input changes */
#define EXP_REG_INPUT 0x00
#define EXP_REG_OUTPUT 0x02
#define EXP_REG_CONFIG 0x06        /* 1 = input */
#define EXP_INPUTS 0x20FF          /* port 0 and BAT_DET (13), which read_power() ignores */
#define EXP_STDBY BIT(1)           /* low once charged */
#define EXP_VBUS BIT(2)            /* low on USB power */
#define EXP_WHEEL BIT(3)           /* low while pressed */
#define EXP_SD_DET BIT(4)          /* low while a card is in */
#define EXP_CAM_SYNC BIT(6)        /* high while the Himax has a reply waiting */
#define EXP_HIMAX_RST BIT(7)       /* pulled up; driven low, it resets the Himax */
#define EXP_PWR_SD BIT(8)          /* also powers the SPI2 pull-ups the Himax needs */
#define EXP_PWR_LCD BIT(9)
#define EXP_PWR_SYSTEM BIT(10)     /* holds the board on from battery */
#define EXP_PWR_AI BIT(11)         /* Himax vision coprocessor */
#define EXP_PWR_CODEC_PA BIT(12)
#define EXP_PWR_GROVE BIT(14)
#define EXP_PWR_BAT_ADC BIT(15)
#define EXP_RAILS (EXP_PWR_LCD | EXP_PWR_CODEC_PA | EXP_PWR_BAT_ADC)

/* The Himax camera speaks SSCMA on SPI2 (Seeed's BSP pins), a bus it shares
 * with the SD card. As in Seeed's sscma_client, every command is one 256-byte
 * packet, and the answer is clocked out after a pause. */
#define CAM_HOST SPI2_HOST
#define CAM_SCLK GPIO_NUM_4
#define CAM_MOSI GPIO_NUM_5
#define CAM_MISO GPIO_NUM_6
#define CAM_CS GPIO_NUM_21
#define SD_CS GPIO_NUM_46          /* pulled down: undriven, the card answers too */
#define CAM_HZ (12 * 1000 * 1000)
#define HEADER_TX GPIO_NUM_19      /* the 2x4 header's spare pins, a UART in Seeed's firmware */
#define HEADER_RX GPIO_NUM_20
#define CAM_PACKET 256
#define CAM_PAYLOAD 250
#define CAM_WAIT_MS 2              /* before each packet and each answer, as Seeed's BSP */
#define CAM_FEATURE 0x10
#define CAM_READ 0x01
#define CAM_WRITE 0x02
#define CAM_AVAILABLE 0x03

#define DEBOUNCE_SAMPLES 3
#define TURN_COUNTS 2      /* encoder quarter-steps to a detent */
#define TURN_REST 20       /* 200 ms without movement ends a turn */
#define TP_LIFT_MS 40      /* touch reads empty this long before the finger counts as lifted */
#define TP_POLL_MS 10      /* between touch polls while a finger is down */
#define TP_IDLE_POLL_MS 20 /* and while not */
#define WAIT_DEBOUNCE_MS 10 /* wait_buttons() mid-debounce: the awake poll */
#define WAIT_BUSY_MS 50    /* ... and with EXP_INT already low: as often as it polled before */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp;
static uint16_t s_exp_out;
static uint16_t s_exp_cfg = EXP_INPUTS;
/* The output and config shadows: the camera and the SD card switch rails from
 * their own tasks, so changes to them take turns. */
static SemaphoreHandle_t s_exp_lock;
static SemaphoreHandle_t s_spi2_lock;  /* spi2_rails(): its count and the rails it switches, together */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static pcnt_unit_handle_t s_knob;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_wheel_down;
static uint8_t s_wheel_stable;     /* samples the push has read changed */
static bool s_paused;
static volatile bool s_wheel_moved;
static TaskHandle_t s_waiter;      /* the input task, woken out of wait_buttons() */

static esp_err_t exp_write(uint8_t reg, uint16_t v)
{
    const uint8_t buf[] = { reg, v & 0xff, v >> 8 };
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 50);
}

static esp_err_t exp_read(uint16_t *v)
{
    const uint8_t reg = EXP_REG_INPUT;
    uint8_t buf[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_exp, &reg, 1, buf, sizeof(buf), 50), TAG, "expander read");
    *v = buf[0] | buf[1] << 8;
    return ESP_OK;
}

static esp_err_t exp_set(uint16_t mask, bool on)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    s_exp_out = on ? s_exp_out | mask : s_exp_out & ~mask;
    esp_err_t err = exp_write(EXP_REG_OUTPUT, s_exp_out);
    xSemaphoreGive(s_exp_lock);
    return err;
}

/* Pins in `mask` become inputs (true) or outputs (false). */
static esp_err_t exp_input(uint16_t mask, bool input)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    s_exp_cfg = input ? s_exp_cfg | mask : s_exp_cfg & ~mask;
    esp_err_t err = exp_write(EXP_REG_CONFIG, s_exp_cfg);
    xSemaphoreGive(s_exp_lock);
    return err;
}

/* Deep sleep until an expander input changes. The wheel's push is one. */
static void sleep_until_wheel(void)
{
    uint16_t in;
    exp_read(&in);  /* clears the interrupt */
    rtc_gpio_pullup_en(EXP_INT);
    rtc_gpio_pulldown_dis(EXP_INT);
    esp_sleep_enable_ext0_wakeup(EXP_INT, 0);
    esp_deep_sleep_start();
}

/* A line reached the level it was armed at: a wheel line (display_pause) or
 * the expander's INT (wait_buttons). Level-triggered, so off until armed
 * again: a wheel line, once per pause. */
static void on_wake_line(void *arg)
{
    gpio_num_t gpio = (gpio_num_t)(intptr_t)arg;
    gpio_intr_disable(gpio);
    if (gpio != EXP_INT) {
        s_wheel_moved = true;
    }
    BaseType_t woken = pdFALSE;
    if (s_waiter) {
        vTaskNotifyGiveFromISR(s_waiter, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static esp_err_t knob_init(void)
{
    const pcnt_unit_config_t unit_cfg = { .low_limit = -100, .high_limit = 100 };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &s_knob), TAG, "pcnt");
    const pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 1000 };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_knob, &filter), TAG, "pcnt filter");
    /* Full quadrature: count both edges of both lines. */
    pcnt_channel_handle_t a, b;
    const pcnt_chan_config_t a_cfg = { .edge_gpio_num = KNOB_A, .level_gpio_num = KNOB_B };
    const pcnt_chan_config_t b_cfg = { .edge_gpio_num = KNOB_B, .level_gpio_num = KNOB_A };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_knob, &a_cfg, &a), TAG, "pcnt a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_knob, &b_cfg, &b), TAG, "pcnt b");
    pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    gpio_pullup_en(KNOB_A);
    gpio_pullup_en(KNOB_B);
    /* For display_pause(); the lines don't interrupt until then. */
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "gpio isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(KNOB_A, on_wake_line, (void *)(intptr_t)KNOB_A), TAG, "knob a isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(KNOB_B, on_wake_line, (void *)(intptr_t)KNOB_B), TAG, "knob b isr");
    gpio_intr_disable(KNOB_A);
    gpio_intr_disable(KNOB_B);
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_knob), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_knob), TAG, "pcnt clear");
    return pcnt_unit_start(s_knob);
}

static esp_err_t set_led(uint8_t r, uint8_t g, uint8_t b);
#if CONFIG_MUSE_WATCHER_CAMERA
static esp_err_t camera_power(bool on)
{
    return exp_set(EXP_PWR_AI, on);
}

static bool camera_has_data(void)
{
    uint16_t in;
    return exp_read(&in) == ESP_OK && (in & EXP_CAM_SYNC);
}
#endif

static esp_err_t init(void)
{
    /* Hold the LCD and touch lines low until the LCD rail is up, as Seeed's BSP does. */
    const gpio_config_t lcd_pins = {
        .pin_bit_mask = BIT64(TP_SDA) | BIT64(TP_SCL) | BIT64(LCD_PCLK) | BIT64(LCD_D0) | BIT64(LCD_D1) |
                        BIT64(LCD_D2) | BIT64(LCD_D3) | BIT64(LCD_CS) | BIT64(LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&lcd_pins), TAG, "lcd pins");
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (lcd_pins.pin_bit_mask & BIT64(pin)) {
            gpio_set_level(pin, 0);
        }
    }

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    s_exp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_lock, ESP_ERR_NO_MEM, TAG, "expander lock");
    s_spi2_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_spi2_lock, ESP_ERR_NO_MEM, TAG, "spi2 rails lock");
    const i2c_device_config_t exp_cfg = { .device_address = EXP_ADDR, .scl_speed_hz = 400000 };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exp_cfg, &s_exp), TAG, "expander");

    /* Outputs start low, then the system rail, then the rails Muse uses (Seeed's order). */
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT, 0), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG, s_exp_cfg), TAG, "expander config");
    uint16_t in;
    ESP_RETURN_ON_ERROR(exp_read(&in), TAG, "expander inputs");
    if ((esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0)) && (in & EXP_WHEEL)) {
        /* Powered off on USB, and something other than the wheel woke us (the charger). */
        sleep_until_wheel();
    }
    ESP_RETURN_ON_ERROR(exp_set(EXP_PWR_SYSTEM, true), TAG, "system rail");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exp_set(EXP_RAILS, true), TAG, "rails");
    vTaskDelay(pdMS_TO_TICKS(50));
#if CONFIG_MUSE_WATCHER_CAMERA
    ESP_RETURN_ON_ERROR(watcher_camera_prepare(), TAG, "camera lock");
    /* Reset stays untouched, as before: powering the camera up resets it. */
    const camera_sscma_config_t cam = {
        .name = "Himax WiseEye2 (SenseCAP Watcher)",
        .host = CAM_HOST,
        .sclk = CAM_SCLK,
        .mosi = CAM_MOSI,
        .miso = CAM_MISO,
        .cs = CAM_CS,
        .power = camera_power,
        .has_data = camera_has_data,
        .resolution = 3,          /* 640x480 stills */
        .stream_resolution = 1,   /* 416x416 preview frames: they fit the round screen */
    };
    camera_register(camera_sscma(&cam));
#endif
    ESP_LOGI(TAG, "expander inputs 0x%04x", in);
    /* The wheel is held at boot to power on; don't count that as a press. */
    s_wheel_down = !(in & EXP_WHEEL);

    ESP_RETURN_ON_ERROR(knob_init(), TAG, "knob");
    /* The RGB light shows whatever its data line picked up at power-on until
     * something drives it, so start it off. */
    if (set_led(0, 0, 0) != ESP_OK) {
        ESP_LOGW(TAG, "rgb light didn't answer");
    }
    /* The expander holds EXP_INT low from an input changing until the inputs
     * are read. wait_buttons() sleeps on it, as Seeed's BSP does. */
    const gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(EXP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "expander int");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(EXP_INT, on_wake_line, (void *)(intptr_t)EXP_INT), TAG,
                        "expander int isr");
    gpio_intr_disable(EXP_INT);

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_2_5,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration: battery level disabled");
    }
    return ESP_OK;
}

/* The SPD2010 needs update windows that start and end on 4-column boundaries. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~3;
    a->x2 |= 3;
}

/* The touch driver writes the register address, then reads with an empty
 * command. IDF 6 rejects that as a zero-length write; -1 makes it a plain read. */
static esp_err_t (*s_tp_rx)(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size);

static esp_err_t tp_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    return s_tp_rx(io, -1, param, param_size);
}

/*
 * The SPD2010 is polled (it has no interrupt line here), and now and then a
 * poll finds no report though the finger is still down. Taken as a lift, that
 * turns one tap into two, so the last point stands until reads have come back
 * empty for TP_LIFT_MS.
 *
 * A poll takes about 2 ms of I2C and the driver's busy waits, so it runs on a
 * task of its own rather than holding up LVGL's drawing every frame, and
 * stops while the screen is off.
 */
static TaskHandle_t s_tp_task;
static portMUX_TYPE s_tp_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_lcd_touch_point_data_t s_tp_point;
static bool s_tp_down;

static void poll_touch(void *arg)
{
    esp_lcd_touch_handle_t tp = arg;
    esp_lcd_touch_point_data_t p;
    int64_t last_us = 0;
    for (;;) {
        if (s_paused) {
            taskENTER_CRITICAL(&s_tp_lock);
            s_tp_down = false;
            taskEXIT_CRITICAL(&s_tp_lock);
            if (last_us) {
                muse_hw_touch(false, 0, 0);
            }
            last_us = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        bool was_down = last_us != 0;
        uint8_t n = 0;
        if (esp_lcd_touch_read_data(tp) == ESP_OK) {
            esp_lcd_touch_get_data(tp, &p, &n, 1);
        }
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_tp_lock);
        if (n) {
            s_tp_point = p;
            last_us = now;
        } else if (last_us && now - last_us >= TP_LIFT_MS * 1000) {
            last_us = 0;
        }
        s_tp_down = last_us != 0;
        esp_lcd_touch_point_data_t at = s_tp_point;
        taskEXIT_CRITICAL(&s_tp_lock);
        if (last_us || was_down) {
            muse_hw_touch(last_us != 0, at.x, at.y);
        }
        vTaskDelay(pdMS_TO_TICKS(last_us ? TP_POLL_MS : TP_IDLE_POLL_MS));
    }
}

static esp_err_t tp_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points, uint8_t *count,
                         uint8_t max_count, void *ctx)
{
    (void)tp;
    (void)max_count;
    (void)ctx;
    taskENTER_CRITICAL(&s_tp_lock);
    points[0] = s_tp_point;
    *count = s_tp_down && !muse_hw_captured();
    taskEXIT_CRITICAL(&s_tp_lock);
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

    const spi_bus_config_t bus =
        SPD2010_PANEL_BUS_QSPI_CONFIG(LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = SPD2010_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    spd2010_vendor_config_t vendor_cfg = { .flags.use_qspi_interface = 1 };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_spd2010(s_io, &panel_cfg, &s_panel) != ESP_OK) {
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
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    i2c_master_bus_handle_t tp_bus;
    const i2c_master_bus_config_t tp_bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = TP_SDA,
        .scl_io_num = TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&tp_bus_cfg, &tp_bus) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(tp_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    s_tp_rx = tp_io->rx_param;
    tp_io->rx_param = tp_rx_param;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_spd2010(tp_io, &tp_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    /* Seeed reads once to start the touch controller. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(tp);

    if (xTaskCreatePinnedToCore(poll_touch, "touch", 3072, tp, MUSE_UI_PRIORITY, &s_tp_task, !MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    lv_tp_cfg.callbacks.custom_touch_read = tp_read;
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

/* SLPIN/SLPOUT: the SPD2010 stops driving the panel and scanning for touch. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/*
 * Screen off: LVGL stops and the chip light-sleeps in wait_buttons(). PCNT has
 * to stop too: it holds the APB clock up, and light sleep would stop it
 * anyway. Instead each wheel line wakes the chip, and interrupts, at the level
 * it isn't at now. The panel sleeps: left scanning with its backlight off, its
 * touch interrupt reached the expander about six times a second, and each one
 * woke the chip. Touch doesn't wake it while paused, so nothing is lost; the
 * touch driver restarts the controller on its first read after.
 */
static void display_pause(bool pause)
{
    static const gpio_num_t lines[] = { KNOB_A, KNOB_B };
    s_paused = pause;
    if (pause) {
        s_waiter = xTaskGetCurrentTaskHandle();   /* the input task, before a line can fire */
        esp_lv_adapter_pause(-1);
        panel_sleep(true);
        pcnt_unit_stop(s_knob);
        pcnt_unit_disable(s_knob);
        s_wheel_moved = false;
        for (int i = 0; i < 2; i++) {
            gpio_wakeup_enable(lines[i], gpio_get_level(lines[i]) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
            gpio_intr_enable(lines[i]);
        }
        esp_sleep_enable_gpio_wakeup();
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        for (int i = 0; i < 2; i++) {
            gpio_intr_disable(lines[i]);
            gpio_wakeup_disable(lines[i]);
            gpio_set_intr_type(lines[i], GPIO_INTR_DISABLE);
        }
        pcnt_unit_enable(s_knob);
        pcnt_unit_clear_count(s_knob);
        pcnt_unit_start(s_knob);
        panel_sleep(false);
        xTaskNotifyGive(s_tp_task);
        esp_lv_adapter_resume();
    }
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

/* ES8311 plays and a separate ES7243(E) ADC records, on one duplex I2S bus with MCLK. */
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
    /* Seeed probes for the older ES7243 first. */
    bool es7243 = i2c_master_probe(s_i2c, ES7243_ADDR, 50) == ESP_OK;
    audio_codec_i2c_cfg_t adc_i2c = {
        .port = I2C_NUM_0,
        .addr = (es7243 ? ES7243_ADDR : ES7243E_ADDR) << 1,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,     /* the amplifier is on an expander rail */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    const audio_codec_if_t *adc;
    if (es7243) {
        es7243_codec_cfg_t cfg = { .ctrl_if = adc_ctrl };
        adc = es7243_codec_new(&cfg);
    } else {
        es7243e_codec_cfg_t cfg = { .ctrl_if = adc_ctrl };
        adc = es7243e_codec_new(&cfg);
    }
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "%s not responding", es7243 ? "ES7243" : "ES7243E");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* The wheel's push, debounced like muse_gpio_button_poll(). */
static unsigned poll_wheel_push(void)
{
    uint16_t in;
    if (exp_read(&in) != ESP_OK) {
        return 0;
    }
    bool raw = !(in & EXP_WHEEL);
    if (raw == s_wheel_down) {
        s_wheel_stable = 0;
        return 0;
    }
    /* Asleep, a press is what woke the input task: it counts on its first sample. */
    if (++s_wheel_stable < DEBOUNCE_SAMPLES && !(raw && muse_state_asleep())) {
        return 0;
    }
    s_wheel_stable = 0;
    s_wheel_down = raw;
    muse_hw_wheel_push(raw);
    return raw ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
}

/*
 * The wheel's turns, for the dial: detents since the last poll, + clockwise,
 * TURN_COUNTS quarter-steps each. Muse's agent gets each turn whole, with its
 * direction, once the wheel rests. Paused, PCNT is stopped and a move of
 * either line reads as one detent, which only wakes Muse.
 */
static int poll_dial(void)
{
    static int counts, turned, rest;
    if (s_paused) {
        if (!s_wheel_moved) {
            return 0;
        }
        s_wheel_moved = false;
        return 1;
    }
    int n = 0;
    if (pcnt_unit_get_count(s_knob, &n) == ESP_OK && n) {
        pcnt_unit_clear_count(s_knob);
    }
    rest = n ? 0 : rest + 1;
    counts += n;
    turned += n;
    if (rest == TURN_REST && turned) {
        int steps = turned / TURN_COUNTS;
        muse_hw_wheel_turn(KNOB_CW_SIGN * (steps ? steps : turned > 0 ? 1 : -1));
        turned = 0;
    }
    int detents = counts / TURN_COUNTS;
    counts -= detents * TURN_COUNTS;
    if (rest >= TURN_REST) {
        counts = 0;   /* half a detent, left when the wheel rests, is a wobble */
    }
    return KNOB_CW_SIGN * detents;
}

static unsigned poll_buttons(void)
{
    return poll_wheel_push();
}

/*
 * Display paused: sleeps until the wheel turns (display_pause armed its
 * lines) or an expander input changes: the push, USB power, the charger, the
 * touch panel's interrupt. poll_buttons() reads the inputs before each wait,
 * which lets EXP_INT go high again. If it's low already, an input changed
 * since then: a noisy one can't make this poll faster than it did without
 * the wait.
 */
static void wait_buttons(int timeout_ms)
{
    if (s_wheel_stable) {
        vTaskDelay(pdMS_TO_TICKS(WAIT_DEBOUNCE_MS));
        return;
    }
    if (!gpio_get_level(EXP_INT)) {
        vTaskDelay(pdMS_TO_TICKS(WAIT_BUSY_MS));
        return;
    }
    gpio_wakeup_enable(EXP_INT, GPIO_INTR_LOW_LEVEL);
    gpio_intr_enable(EXP_INT);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    gpio_intr_disable(EXP_INT);
    gpio_wakeup_disable(EXP_INT);
}

/*
 * Battery voltage and Seeed's charge curve; the expander has the rest. The
 * battery is built in. The expander's BAT_DET line reads high with it charging
 * and charged, and neither Seeed's firmware nor xiaozhi's reads it. Taking it
 * as "no battery" kept Muse from ever counting as on battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    uint16_t in;
    ESP_RETURN_ON_ERROR(exp_read(&in), TAG, "expander read");
    out->usb = !(in & EXP_VBUS);
    out->charging = out->usb && (in & EXP_STDBY);
    out->battery_pct = -1;
    out->battery_mv = 0;
    ESP_RETURN_ON_FALSE(s_cali, ESP_ERR_INVALID_STATE, TAG, "no ADC calibration");
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
        sum += mv;
    }
    int v = sum / 8 * 82 / 20;
    out->battery_mv = v;
    /* Measured on USB too, for the percentage. The voltage tells whether the
     * cell is there: below 3 V on USB it's gone or cut off. */
    bool battery = v >= 3000 || !out->usb;
    out->charging = out->charging && battery;
    if (!battery) {
        return ESP_OK;
    }
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/* The WS2813 on GPIO40, as Seeed's BSP drives it. Started on first use. */
static esp_err_t set_led(uint8_t r, uint8_t g, uint8_t b)
{
    static led_strip_handle_t led;
    if (!led) {
        const led_strip_config_t cfg = {
            .strip_gpio_num = RGB_LED,
            .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        const led_strip_rmt_config_t rmt = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
        };
        ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&cfg, &rmt, &led), TAG, "rgb light");
    }
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(led, 0, r, g, b), TAG, "rgb light");
    return led_strip_refresh(led);
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    /* On battery, dropping the system rail cuts power once the wheel is up.
     * USB keeps the board on, so sleep until the wheel is pressed, with the
     * camera, SD card and Grove rails off too. */
    ESP_RETURN_ON_ERROR(exp_set(EXP_RAILS | EXP_PWR_AI | EXP_PWR_SD | EXP_PWR_GROVE | EXP_PWR_SYSTEM, false), TAG,
                        "rails off");
    uint16_t in;
    while (exp_read(&in) == ESP_OK && !(in & EXP_WHEEL)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    sleep_until_wheel();
    return ESP_FAIL;
}

/* ---- The Himax camera: SSCMA over SPI2 ---- */

static spi_device_handle_t s_cam;
static uint8_t *s_cam_tx;          /* one packet, in DMA-capable internal RAM while powered */
static uint8_t *s_cam_rx;          /* one answer, likewise */
static int s_spi2_users;           /* the camera and the SD card: while either, both rails stay up */
static bool s_spi2_bus;

/* The bus, and the SD card's CS kept high (it's pulled down). */
static esp_err_t spi2_bus_start(void)
{
    if (s_spi2_bus) {
        return ESP_OK;
    }
    /* Keep the SD card off the bus: its CS is pulled down. */
    const gpio_config_t cs = { .pin_bit_mask = BIT64(SD_CS), .mode = GPIO_MODE_OUTPUT, .pull_up_en = true };
    ESP_RETURN_ON_ERROR(gpio_config(&cs), TAG, "sd cs");
    gpio_set_level(SD_CS, 1);
    const spi_bus_config_t bus = {
        .sclk_io_num = CAM_SCLK,
        .mosi_io_num = CAM_MOSI,
        .miso_io_num = CAM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MUSE_SSCMA_READ_MAX + 1,
    };
    esp_err_t err = spi_bus_initialize(CAM_HOST, &bus, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "spi2 bus");
    s_spi2_bus = true;
    return ESP_OK;
}

/*
 * The SD rail carries the bus pull-ups and the Himax, powered, keeps its pins
 * from dragging the bus: both are up while the camera or the card is in use,
 * as in Seeed's firmware. Each of those holds them with on, then lets go.
 */
static esp_err_t spi2_rails(bool on)
{
    /* Held through the switch and the settle: an off racing an on can't
     * leave the rails off while counted on, or let on return before they're up. */
    xSemaphoreTake(s_spi2_lock, portMAX_DELAY);
    /* Never below none: an unmatched off would leave the rails dead for good. */
    bool change = on ? s_spi2_users++ == 0 : s_spi2_users > 0 && --s_spi2_users == 0;
    esp_err_t err = ESP_OK;
    if (change) {
        err = exp_set(EXP_PWR_SD | EXP_PWR_AI, on);
        if (on) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    xSemaphoreGive(s_spi2_lock);
    return err;
}

/* The reader's polls (muse_camera.c's task) and requests' writes (the caller's)
 * share one SPI device, and the driver's bus lock is the device's, not a
 * task's: two tasks interleaving acquire and release trip its assert. So each
 * whole transfer holds this; waiting for it is bounded, so a power-down never
 * has to kill the reader mid-transfer. (The bus itself can only be waited for
 * without a limit: the card's transfers are short.) */
#define CAM_LOCK_MS 300
static SemaphoreHandle_t s_cam_lock;

static bool cam_take(void)
{
    if (!s_cam_lock || xSemaphoreTake(s_cam_lock, pdMS_TO_TICKS(CAM_LOCK_MS)) != pdTRUE) {
        return false;
    }
    if (s_cam_tx && spi_device_acquire_bus(s_cam, portMAX_DELAY) == ESP_OK) {
        return true;
    }
    xSemaphoreGive(s_cam_lock);   /* powered down meanwhile */
    return false;
}

static void cam_give(void)
{
    spi_device_release_bus(s_cam);
    xSemaphoreGive(s_cam_lock);
}

static esp_err_t cam_bus_start(void)
{
    if (!s_cam_lock) {
        s_cam_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_cam_lock, ESP_ERR_NO_MEM, TAG, "camera lock");
    }
    if (s_cam) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(spi2_bus_start(), TAG, "spi2 bus");
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = CAM_HZ,
        .mode = 0,
        .spics_io_num = CAM_CS,
        .queue_size = 1,
    };
    return spi_bus_add_device(CAM_HOST, &dev, &s_cam);
}

static bool s_cam_rails;   /* the camera holds the SPI2 rails: off undoes only what on did */

static esp_err_t cam_power(bool on)
{
    if (!on) {
        /* Not under a transfer: the reader is gone, but a request may be late. */
        bool locked = s_cam_lock && xSemaphoreTake(s_cam_lock, pdMS_TO_TICKS(2 * CAM_LOCK_MS)) == pdTRUE;
        exp_input(EXP_HIMAX_RST, false);   /* held in reset while the card may still use the bus */
        if (s_cam_rails) {
            spi2_rails(false);
            s_cam_rails = false;
        }
        heap_caps_free(s_cam_tx);
        heap_caps_free(s_cam_rx);
        s_cam_tx = s_cam_rx = NULL;
        if (locked) {
            xSemaphoreGive(s_cam_lock);
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(cam_bus_start(), TAG, "camera bus");
    if (!s_cam_tx || !s_cam_rx) {
        heap_caps_free(s_cam_tx);
        heap_caps_free(s_cam_rx);
        s_cam_tx = heap_caps_calloc(1, CAM_PACKET, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_cam_rx = heap_caps_malloc(MUSE_SSCMA_READ_MAX + 1, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_cam_tx || !s_cam_rx) {
            heap_caps_free(s_cam_tx);
            heap_caps_free(s_cam_rx);
            s_cam_tx = s_cam_rx = NULL;
            ESP_LOGE(TAG, "camera buffers: no internal DMA memory");
            return ESP_ERR_NO_MEM;
        }
    }
    /* The rails, then a reset: its latch is 0, so as an output it holds the
     * Himax in reset. */
    if (!s_cam_rails) {
        s_cam_rails = true;   /* counted even if the expander write fails: off uncounts it */
        ESP_RETURN_ON_ERROR(spi2_rails(true), TAG, "camera rails");
    }
    ESP_RETURN_ON_ERROR(exp_input(EXP_HIMAX_RST, false), TAG, "himax reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exp_input(EXP_HIMAX_RST, true), TAG, "himax run");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

/* One command packet: feature, command, length, payload, two 0xFF, zeros to 256. */
static esp_err_t cam_packet(uint8_t cmd, uint16_t len, const void *payload, size_t n)
{
    memset(s_cam_tx, 0, CAM_PACKET);
    s_cam_tx[0] = CAM_FEATURE;
    s_cam_tx[1] = cmd;
    s_cam_tx[2] = len >> 8;
    s_cam_tx[3] = len & 0xff;
    if (payload) {
        memcpy(s_cam_tx + 4, payload, n);
    }
    s_cam_tx[4 + n] = 0xff;
    s_cam_tx[5 + n] = 0xff;
    vTaskDelay(pdMS_TO_TICKS(CAM_WAIT_MS));
    spi_transaction_t t = { .length = CAM_PACKET * 8, .tx_buffer = s_cam_tx };
    return spi_device_transmit(s_cam, &t);
}

static esp_err_t cam_answer(size_t n)
{
    vTaskDelay(pdMS_TO_TICKS(CAM_WAIT_MS));
    spi_transaction_t t = { .length = n * 8, .rxlength = n * 8, .rx_buffer = s_cam_rx };
    return spi_device_transmit(s_cam, &t);
}

static esp_err_t cam_write(const void *data, size_t len)
{
    if (!cam_take()) {
        return s_cam_tx ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    for (size_t off = 0; err == ESP_OK && off < len; off += CAM_PAYLOAD) {
        size_t n = len - off < CAM_PAYLOAD ? len - off : CAM_PAYLOAD;
        err = cam_packet(CAM_WRITE, n, (const uint8_t *)data + off, n);
    }
    cam_give();
    return err;
}

static esp_err_t cam_available(size_t *len)
{
    *len = 0;
    ESP_RETURN_ON_FALSE(s_cam_tx, ESP_ERR_INVALID_STATE, TAG, "camera off");
    uint16_t in;
    ESP_RETURN_ON_ERROR(exp_read(&in), TAG, "expander read");
    if (!(in & EXP_CAM_SYNC)) {
        return ESP_OK;
    }
    if (!cam_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = cam_packet(CAM_AVAILABLE, 0, NULL, 0);
    if (err == ESP_OK) {
        err = cam_answer(2);
    }
    if (err == ESP_OK) {
        size_t n = s_cam_rx[0] << 8 | s_cam_rx[1];
        *len = n == 0xffff ? 0 : n;
    }
    cam_give();
    return err;
}

static esp_err_t cam_read(void *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= MUSE_SSCMA_READ_MAX, ESP_ERR_INVALID_ARG, TAG, "camera read");
    if (!cam_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = cam_packet(CAM_READ, len, NULL, 0);
    if (err == ESP_OK) {
        err = cam_answer(len);
    }
    if (err == ESP_OK) {
        memcpy(buf, s_cam_rx, len);
    }
    cam_give();
    return err;
}

/* ---- Ports: Grove, the header's UART, the SD card ---- */

static esp_err_t sd_power(bool on)
{
    if (on) {
        ESP_RETURN_ON_ERROR(spi2_bus_start(), TAG, "spi2 bus");
        /* The Himax isn't in use: keep it in reset, quiet on the bus. */
        if (!s_cam_tx) {
            exp_input(EXP_HIMAX_RST, false);
        }
    }
    return spi2_rails(on);
}

static bool sd_present(void)
{
    uint16_t in;
    return exp_read(&in) == ESP_OK && !(in & EXP_SD_DET);
}

static esp_err_t grove_power(bool on)
{
    return exp_set(EXP_PWR_GROVE, on);
}

/* The expander, both codecs and the RTC Seeed's BSP expects. */
static const uint8_t ONBOARD_I2C[] = { EXP_ADDR, 0x18, ES7243_ADDR, ES7243E_ADDR, 0x51, 0 };

static const muse_ports_t *ports(void)
{
    static muse_ports_t p;
    p = (muse_ports_t){
        .i2c_bus = s_i2c,
        .i2c_onboard = ONBOARD_I2C,
        .grove_power = grove_power,
        .uart_tx = HEADER_TX,
        .uart_rx = HEADER_RX,
        .sd_spi_host = CAM_HOST,
        .sd_cs = SD_CS,
        .sd_power = sd_power,
        .sd_present = sd_present,
    };
    return &p;
}

static const muse_sscma_ops_t s_sscma = {
    .power = cam_power,
    .write = cam_write,
    .available = cam_available,
    .read = cam_read,
};

static const muse_board_t s_board = {
    .name = "Seeed SenseCAP Watcher",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.45f,
    .talk_button = "wheel",
    .aux_button = "scroll",
    /* The wheel is in the top-right corner: press it to talk, turn it for the
     * volume. Turning it isn't a button of its own (poll_dial, not the aux
     * button), so aux_hint places the volume icon beside the wheel, not a
     * power icon. */
    .talk_hint = { LV_ALIGN_CENTER, 100, -143 },    /* 55 and 35 degrees above 3 o'clock */
    .aux_hint = { LV_ALIGN_CENTER, 143, -100 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 1,              /* one mic, on the right slot */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
    .set_led = set_led,
    .poll_dial = poll_dial,
    .sscma = &s_sscma,
    .ports = ports,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
