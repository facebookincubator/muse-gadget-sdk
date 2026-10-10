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
 * ES7243) mic, and a wheel in the top-right corner: a rotary encoder on GPIOs
 * with its push on a PCA9535 expander. The expander also switches the power
 * rails and reads the charger. The wheel's push powers the board on, and the
 * firmware then holds the system rail. The Himax camera rail stays off until
 * a capture request or live preview; the SD card and Grove rails stay off.
 * Pins follow Seeed's sensecap-watcher BSP
 * (SenseCAP-Watcher-Firmware) and xiaozhi-esp32's sensecap-watcher board.
 */
#include <stdlib.h>

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
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "hal/i2c_periph.h"
#include "soc/spi_periph.h"
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
#define LCD_POWER_UP_MS 200   /* the LCD rail on before the panel's setup (lcd_power) */

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
#define AMP_ON_MS 20           /* the speaker amp starting up before the first sound */

#define KNOB_A GPIO_NUM_41
#define KNOB_B GPIO_NUM_42
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
#define EXP_CAM_SYNC BIT(6)        /* high while the Himax has a reply waiting */
#define EXP_PWR_LCD BIT(9)
#define EXP_PWR_SYSTEM BIT(10)     /* holds the board on from battery */
#define EXP_PWR_AI BIT(11)         /* Himax vision coprocessor */
#define EXP_PWR_CODEC_PA BIT(12)
#define EXP_PWR_BAT_ADC BIT(15)
#define EXP_RAILS (EXP_PWR_LCD | EXP_PWR_CODEC_PA | EXP_PWR_BAT_ADC)

/* The Himax camera speaks SSCMA on its own SPI bus (Seeed's BSP pins). */
#define CAM_HOST SPI2_HOST
#define CAM_SCLK GPIO_NUM_4
#define CAM_MOSI GPIO_NUM_5
#define CAM_MISO GPIO_NUM_6
#define CAM_CS GPIO_NUM_21

#define DEBOUNCE_SAMPLES 3
#define TURN_COUNTS 2      /* encoder quarter-steps that make a turn */
#define TURN_REST 20       /* 200 ms without movement ends a turn */
#define TURN_MAX 40        /* 400 ms, well short of Muse's hold-to-power-off */
#define TP_LIFT_MS 40      /* touch reads empty this long before the finger counts as lifted */
#define TP_POLL_MS 10      /* between touch polls while a finger is down */
#define TP_IDLE_POLL_MS 20 /* and while not */
#define WAIT_DEBOUNCE_MS 10 /* wait_buttons() mid-debounce: the awake poll */
#define WAIT_BUSY_MS 50    /* ... and with EXP_INT already low: as often as it polled before */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp;
static uint16_t s_exp_out;
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

/* The camera switches its rail from its own task, so changes to s_exp_out take turns. */
static SemaphoreHandle_t s_exp_lock;

static esp_err_t exp_set(uint16_t mask, bool on)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    s_exp_out = on ? s_exp_out | mask : s_exp_out & ~mask;
    esp_err_t err = exp_write(EXP_REG_OUTPUT, s_exp_out);
    xSemaphoreGive(s_exp_lock);
    return err;
}

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

/* Deep sleep until an expander input changes. The wheel's push is one. */
static void sleep_until_wheel(void)
{
    uint16_t in;
    exp_read(&in);  /* clears the interrupt */
    rtc_gpio_pullup_en(EXP_INT);
    rtc_gpio_pulldown_dis(EXP_INT);
    esp_sleep_enable_ext0_wakeup(EXP_INT, 0);
    /* Light sleep stays configured for CPU power-down (muse_input_power_init)
     * and leaves the timer armed: only the wheel may wake this. */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
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
    const i2c_device_config_t exp_cfg = { .device_address = EXP_ADDR, .scl_speed_hz = 400000 };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exp_cfg, &s_exp), TAG, "expander");

    s_exp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_lock, ESP_ERR_NO_MEM, TAG, "expander lock");
    /* Outputs start low, then the system rail, then the rails Muse uses (Seeed's order). */
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT, 0), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG, EXP_INPUTS), TAG, "expander config");
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
            last_us = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
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
        taskEXIT_CRITICAL(&s_tp_lock);
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
    *count = s_tp_down;
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
 * The LCD rail is off while paused. Idle, the SPI holds the clock (mode 3) and
 * CS high, which would feed the unpowered panel, so the lines go to plain
 * GPIOs driven low first, as init() holds them before the rail is up.
 */
static void lcd_bus(void *spi)
{
    const spi_signal_conn_t *sig = &spi_periph_signal[LCD_HOST];
    const struct {
        gpio_num_t pin;
        int in, out;
    } lines[] = {
        { LCD_PCLK, -1, sig->spiclk_out },
        { LCD_D0, sig->spid_in, sig->spid_out },
        { LCD_D1, sig->spiq_in, sig->spiq_out },
        { LCD_D2, sig->spiwp_in, sig->spiwp_out },
        { LCD_D3, sig->spihd_in, sig->spihd_out },
        { LCD_CS, -1, sig->spics_out[0] },
    };
    for (int i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if (*(bool *)spi) {
            gpio_set_direction(lines[i].pin, lines[i].in >= 0 ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT);
            if (lines[i].in >= 0) {
                esp_rom_gpio_connect_in_signal(lines[i].pin, lines[i].in, false);
            }
            esp_rom_gpio_connect_out_signal(lines[i].pin, lines[i].out, false, false);
        } else {
            gpio_set_level(lines[i].pin, 0);
            gpio_set_direction(lines[i].pin, GPIO_MODE_OUTPUT);
        }
    }
}

/*
 * Powered again, the panel has lost its setup and what it showed. Its reset
 * line follows the chip's, so a software reset. Its memory comes up random,
 * and the backlight goes on before LVGL's redraw has reached the panel, so it
 * is cleared to black first. Runs on the send task between bands, so its
 * chunk buffers are idle; the clearing takes a buffer of its own for a moment.
 */
static void panel_setup(void *arg)
{
    bool spi = true;
    lcd_bus(&spi);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);   /* ends with SLPOUT */
    uint8_t *black = heap_caps_calloc(1, LCD_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (black) {
        for (int y = 0; y < LCD_RES; y += 8) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_RES, y + 8 < LCD_RES ? y + 8 : LCD_RES, black);
        }
    }
    esp_lcd_panel_disp_on_off(s_panel, true);   /* waits for the queued draws first */
    free(black);
}

/*
 * The touch controller shares the rail, and its bus's pull-ups would feed it
 * too, so its lines are held low as well, as init() holds them. The touch task
 * is parked while paused. The I2C peripheral sees an idle (high) bus meanwhile.
 */
static void tp_bus(bool on)
{
    const i2c_signal_conn_t *sig = &i2c_periph_signal[I2C_NUM_1];
    const struct {
        gpio_num_t pin;
        int in, out;
    } lines[] = {
        { TP_SDA, sig->sda_in_sig, sig->sda_out_sig },
        { TP_SCL, sig->scl_in_sig, sig->scl_out_sig },
    };
    for (int i = 0; i < 2; i++) {
        if (on) {
            gpio_set_level(lines[i].pin, 1);
            gpio_set_direction(lines[i].pin, GPIO_MODE_INPUT_OUTPUT_OD);
            gpio_pullup_en(lines[i].pin);
            esp_rom_gpio_connect_out_signal(lines[i].pin, lines[i].out, false, false);
            esp_rom_gpio_connect_in_signal(lines[i].pin, lines[i].in, false);
        } else {
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, lines[i].in, false);
            gpio_pullup_dis(lines[i].pin);
            gpio_set_level(lines[i].pin, 0);
            gpio_set_direction(lines[i].pin, GPIO_MODE_OUTPUT);
        }
    }
}

static void lcd_power(bool on)
{
    if (!on) {
        bool spi = false;
        muse_lcd_bands_run(lcd_bus, &spi);
        tp_bus(false);
        exp_set(EXP_PWR_LCD, false);
        ESP_LOGI(TAG, "LCD rail off");
        return;
    }
    exp_set(EXP_PWR_LCD, true);
    /* The SPD2010 runs firmware of its own, which takes a while to start: at
     * boot ~150 ms pass between the rail and its setup, and after only 50 ms
     * some of the setup was lost and the panel showed garbage. */
    vTaskDelay(pdMS_TO_TICKS(LCD_POWER_UP_MS));
    tp_bus(true);
    muse_lcd_bands_run(panel_setup, NULL);
    ESP_LOGI(TAG, "LCD rail on, panel set up");
}

/*
 * Screen off: LVGL stops and the chip light-sleeps in wait_buttons(). PCNT has
 * to stop too: it holds the APB clock up, and light sleep would stop it
 * anyway. Instead each wheel line wakes the chip, and interrupts, at the level
 * it isn't at now. The panel sleeps, then loses its rail: left scanning with
 * its backlight off, its touch interrupt reached the expander about six times
 * a second, and each one woke the chip. Touch doesn't wake it while paused, so
 * nothing is lost; the touch driver restarts the controller on its first read
 * after. LVGL redraws the whole screen on resume, since the panel forgot it.
 */
static void display_pause(bool pause)
{
    static const gpio_num_t lines[] = { KNOB_A, KNOB_B };
    s_paused = pause;
    if (pause) {
        s_waiter = xTaskGetCurrentTaskHandle();   /* the input task, before a line can fire */
        esp_lv_adapter_pause(-1);
        panel_sleep(true);
        lcd_power(false);
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
        lcd_power(true);
        xTaskNotifyGive(s_tp_task);
        esp_lv_adapter_resume();
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_obj_invalidate(lv_screen_active());
            esp_lv_adapter_unlock();
        }
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

/*
 * EXP_PWR_CODEC_PA is the speaker amp's enable. The amp runs straight from the
 * battery, so left enabled while resting it idles all night. The codecs have
 * a rail of their own, always on, and keep their setup.
 */
static void audio_power(bool on)
{
    exp_set(EXP_PWR_CODEC_PA, on);
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(AMP_ON_MS));
    }
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
    return raw ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
}

/*
 * A turn of the wheel, either way, reads as one short press: it goes down on
 * the first step and up once the wheel rests. A long turn is cut short so it
 * can't become Muse's hold-to-power-off; the rest of it is ignored.
 */
static unsigned poll_wheel_turn(void)
{
    static int moved, rest, held;
    static bool down, spent;
    int n = 0;
    if (s_paused) {
        n = s_wheel_moved ? TURN_COUNTS : 0;
    } else if (pcnt_unit_get_count(s_knob, &n) == ESP_OK && n) {
        pcnt_unit_clear_count(s_knob);
    }
    rest = n ? 0 : rest + 1;
    if (down) {
        if (rest < TURN_REST && ++held < TURN_MAX) {
            return 0;
        }
        down = false;
        spent = rest < TURN_REST;
        moved = 0;
        return MUSE_BTN_TALK_RELEASE;
    }
    if (rest >= TURN_REST) {
        moved = 0;
        spent = false;
        return 0;
    }
    moved += n;
    if (spent || abs(moved) < TURN_COUNTS) {
        return 0;
    }
    down = true;
    held = 0;
    return MUSE_BTN_TALK_PRESS;
}

static unsigned poll_buttons(void)
{
    return poll_wheel_push() | poll_wheel_turn() << 2;
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
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
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
     * USB keeps the board on, so sleep until the wheel is pressed. */
    ESP_RETURN_ON_ERROR(exp_set(EXP_RAILS | EXP_PWR_AI | EXP_PWR_SYSTEM, false), TAG, "rails off");
    uint16_t in;
    while (exp_read(&in) == ESP_OK && !(in & EXP_WHEEL)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    sleep_until_wheel();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Seeed SenseCAP Watcher",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.45f,
    .talk_button = "wheel",
    .aux_button = "scroll",
    /* The wheel is in the top-right corner: press it to talk, turn it to sleep.
     * Turning it isn't a button of its own, so no aux_hint: a power icon beside
     * the wheel would point at a button that isn't there. */
    .talk_hint = { LV_ALIGN_CENTER, 100, -143 },    /* 55 degrees above 3 o'clock */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .audio_power = audio_power,
    .mic_slot = 1,              /* one mic, on the right slot */
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
