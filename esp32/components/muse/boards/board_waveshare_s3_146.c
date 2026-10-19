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
 * Waveshare ESP32-S3-Touch-LCD-1.46: round 412 px SPD2010 QSPI panel with the
 * controller's own capacitive touch on I2C, PCM5101 I2S DAC for the speaker
 * (no control interface), an I2S MEMS mic on a bus of its own, TCA9554 I/O
 * expander (LCD and touch resets live on its EXIO1/EXIO2 pins), no PMU.
 * BOOT (GPIO0) is the talk button; holding a finger on the face tile also
 * talks. Same shell and pin family as the Touch-LCD-1.85C, with the SPD2010
 * panel the SenseCAP Watcher uses.
 */
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_ui.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"

static const char *TAG = "board";

/* Pins (board wiki / vendor demo). */
#define LCD_PCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_CS GPIO_NUM_21
#define LCD_BL GPIO_NUM_5
#define LCD_RES 412
#define LCD_HOST SPI2_HOST

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
/* The touch interrupt (GPIO4) goes unused: reads in its ISR path wedged
 * LVGL's task on first touch, so the SPD2010 is polled like the Watcher's. */

#define TP_LIFT_MS 40      /* touch reads empty this long before the finger counts as lifted */
#define TP_POLL_MS 10      /* between touch polls while a finger is down */
#define TP_IDLE_POLL_MS 20 /* and while not */

#define TCA9554_ADDR 0x20
#define TCA9554_OUTPUT_REG 0x01
#define TCA9554_CONFIG_REG 0x03
#define EXIO_LCD_RST_BIT BIT(0)     /* EXIO1, to the SPD2010 panel */
#define EXIO_TOUCH_RST_BIT BIT(1)   /* EXIO2, to its touch side */
/* EXIO3 is the SD card's CS (kept high: deselected); EXIO4/EXIO5 are driven
 * by the QMI8658's interrupt pins, so they stay inputs. */
#define EXIO_OUTPUTS (EXIO_LCD_RST_BIT | EXIO_TOUCH_RST_BIT | BIT(2))

/* Speaker: PCM5101 on its own I2S bus. */
#define SPK_BCLK GPIO_NUM_48
#define SPK_WS GPIO_NUM_38
#define SPK_DOUT GPIO_NUM_47
/* Mic: I2S MEMS microphone on a second bus. */
#define MIC_BCLK GPIO_NUM_15
#define MIC_WS GPIO_NUM_2
#define MIC_DIN GPIO_NUM_39

#define MIC_GAIN_OFFSET_DB 0

#define DRAW_BUF_LINES 104      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tca;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static muse_gpio_button_t s_boot;
static i2s_chan_handle_t s_tx, s_rx;
static int32_t s_mic_dc[2];
static int s_mic_gain_q8 = 256;
static TaskHandle_t s_tp_task;
static portMUX_TYPE s_tp_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_lcd_touch_point_data_t s_tp_point;
static bool s_tp_down;
static bool s_paused;

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
 * The SPD2010 is polled, and now and then a poll finds no report though the
 * finger is still down. Taken as a lift, that turns one tap into two, so the
 * last point stands until reads have come back empty for TP_LIFT_MS.
 *
 * A poll takes about 2 ms of I2C and the driver's busy waits, so it runs on a
 * task of its own rather than holding up LVGL's drawing every frame, and
 * stops while the screen is off.
 */
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

/* The TCA9554's reset lines are driven by the board, not by the panel and
 * touch drivers (they'd use plain GPIOs). */
static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    return i2c_master_transmit(s_tca, (uint8_t[]){ reg, val }, 2, -1);
}

static esp_err_t tca_reset_pulse(uint8_t bit)
{
    uint8_t out;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_tca, (uint8_t[]){ TCA9554_OUTPUT_REG }, 1, &out, 1, -1),
                        TAG, "tca read");
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_OUTPUT_REG, out & ~bit), TAG, "tca low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_OUTPUT_REG, out | bit), TAG, "tca high");
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

    const i2c_device_config_t tca_cfg = {
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tca_cfg, &s_tca), TAG, "tca9554");
    /* Reset and CS lines output and high, the IMU's interrupts left inputs;
     * then pulse the two resets. */
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_OUTPUT_REG, 0xFF), TAG, "tca outputs");
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_CONFIG_REG, (uint8_t)~EXIO_OUTPUTS), TAG, "tca config");
    ESP_RETURN_ON_ERROR(tca_reset_pulse(EXIO_LCD_RST_BIT), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(tca_reset_pulse(EXIO_TOUCH_RST_BIT), TAG, "touch reset");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
    /* BOOT is held at flashing time; don't count a held button as a press. */
    s_boot.pressed = gpio_get_level(GPIO_NUM_0) == 0;
    return ESP_OK;
}

/* muse_lcd_bands_register takes over the panel IO's color-done callback and,
 * with the bands, needs the bus set up on the same core as the UI task. */
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
        ESP_LOGE(TAG, "backlight pwm");
        return NULL;
    }

    const spi_bus_config_t bus =
        SPD2010_PANEL_BUS_QSPI_CONFIG(LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = SPD2010_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    spd2010_vendor_config_t vendor_cfg = { .flags.use_qspi_interface = 1 };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,   /* driven through the TCA9554 in init() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_spd2010(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "spd2010");
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

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io");
        return NULL;
    }
    s_tp_rx = tp_io->rx_param;
    tp_io->rx_param = tp_rx_param;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* driven through the TCA9554 in init() */
        .int_gpio_num = GPIO_NUM_NC,   /* polled (poll_touch) */
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    if (esp_lcd_touch_new_i2c_spd2010(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "spd2010 touch");
        return NULL;
    }
    /* One read to start the touch controller (it may fail; the Watcher's
     * vendor code does the same). */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(s_tp);

    if (xTaskCreatePinnedToCore(poll_touch, "touch", 3072, s_tp, MUSE_UI_PRIORITY, &s_tp_task, !MUSE_UI_CORE) != pdPASS) {
        ESP_LOGE(TAG, "touch task");
        return NULL;
    }
    esp_lv_adapter_touch_config_t tp_lvgl_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    tp_lvgl_cfg.callbacks.custom_touch_read = tp_read;
    *touch = esp_lv_adapter_register_touch(&tp_lvgl_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    s_touch_indev = *touch;
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

static void display_pause(bool pause)
{
    s_paused = pause;
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
        if (s_tp_task) {
            xTaskNotifyGive(s_tp_task);   /* wake the touch poller */
        }
    }
}

/* No codec control anywhere: the PCM5101 takes I2S as is, esp_codec_dev adds
 * the volume in software, and the mic gain is applied here. Both channels run
 * all the time; idle, the speaker sends zeros (auto_clear). */
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
    return i2s_channel_write(s_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK ? ESP_CODEC_DEV_OK
                                                                                : ESP_CODEC_DEV_WRITE_FAIL;
}

/* A MEMS mic's slot has a small DC offset, which the gain would turn into
 * clipping: each slot's zero level is tracked and taken off first. */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t got;
    if (i2s_channel_read(s_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    int16_t *s = (int16_t *)data;
    for (int i = 0; i < size / 2; i++) {
        int32_t *dc = &s_mic_dc[i & 1];
        *dc += (s[i] * 256 - *dc) >> 8;
        int v = (s[i] - (*dc >> 8)) * s_mic_gain_q8 >> 8;
        s[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
}

/* Two I2S buses: the PCM5101 on I2S1 with plain 16-bit stereo, the MEMS mic
 * on I2S0 with the 32-bit slots it needs (each sample is the slot's top
 * 16 bits). */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan, &s_tx, NULL), TAG, "i2s tx chan");
    const i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &tx_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");

    i2s_chan_config_t rx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_chan, NULL, &s_rx), TAG, "i2s rx chan");
    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN,
        },
    };
    rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    rx_cfg.slot_cfg.ws_width = 32;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &rx_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx on");

    static const audio_codec_data_if_t spk_if = { .enable = data_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = data_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(*spk && *mic, ESP_FAIL, TAG, "codec devs");
    ESP_LOGI(TAG, "audio: PCM5101 (spk, I2S1) + MEMS mic (I2S0)");
    return ESP_OK;
}

/* Hold-to-talk on the face tile: a finger held on the avatar for
 * TOUCH_HOLD_MS acts as the talk button. Taps and touches on other tiles
 * (e.g. settings) never trigger talk. Touch state comes from LVGL's input
 * device, which owns the SPD2010 driver handle; reading the driver here
 * would race LVGL's own reads. */
#define TOUCH_HOLD_MS 500

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_boot);

    static bool tp_down = false;          /* finger currently on screen */
    static bool tp_talking = false;       /* hold threshold reached */
    static TickType_t tp_press_tick = 0;  /* when the current press began */
    static bool tp_on_face = false;       /* press started on the face tile */

    bool pressed = false;
    if (s_touch_indev) {
        pressed = (lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED);
    }

    if (pressed && !tp_down) {
        /* New press: only a hold that starts on the face tile can talk. */
        tp_down = true;
        tp_on_face = muse_ui_face_active();
        tp_press_tick = xTaskGetTickCount();
        tp_talking = false;
    } else if (!pressed && tp_down) {
        /* Release: end the talk if one started. */
        if (tp_talking) {
            ev |= MUSE_BTN_TALK_RELEASE;
        }
        tp_down = false;
        tp_talking = false;
    } else if (pressed && tp_down && tp_on_face && !tp_talking) {
        /* Still held on the face: start talking once the hold threshold passes. */
        if ((xTaskGetTickCount() - tp_press_tick) >= pdMS_TO_TICKS(TOUCH_HOLD_MS)) {
            tp_talking = true;
            ev |= MUSE_BTN_TALK_PRESS;
        }
    }
    return ev;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/* The wiki documents no battery-sense pin for this board, so the meter only
 * knows whether a USB host is on the chip's own port. */
static esp_err_t read_power(muse_power_t *out)
{
    out->battery_mv = 0;
    out->battery_pct = -1;
    out->charging = false;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    /* No PMU; deep sleep is the closest thing to off. BOOT wakes it. */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.46",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.46f,
    .talk_button = "BOOT",
    /* BOOT is on the side edge, toward the top of the puck. */
    .talk_hint = { LV_ALIGN_TOP_LEFT, 20, 16 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 1,              /* one mic, on the right slot (audio self-test) */
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
