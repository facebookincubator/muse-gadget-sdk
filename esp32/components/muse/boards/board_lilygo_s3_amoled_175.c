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
 * LilyGO T-Display-S3 AMOLED 1.75 (H741-01): round 466 px CO5300 QSPI AMOLED
 * (panel H0175Y003AM) with CST9217 touch, SY6970 charger and PCF8563 RTC on
 * the same I2C bus, no speaker or microphone. The panel is driven like the
 * Waveshare AMOLED-1.43: banded flush, even-aligned windows, 6 of the
 * controller's columns as the gap, brightness over 0x51 between bands. BOOT
 * (GPIO0) is the talk button, but without a mic it only wakes the screen and
 * confirms pairing (replies are text), and a tap confirms pairing too. The
 * SY6970 reports the real battery voltage and charge state over I2C; its
 * watchdog is disabled so the charger keeps host settings. Pins from LilyGO's
 * components/private_library/pin_config.h.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_link.h"
#include "muse_ui.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst9217.h"

static const char *TAG = "board";

/* Pins (LilyGO pin_config.h, H0175Y003AM). */
#define LCD_CS GPIO_NUM_10
#define LCD_SCLK GPIO_NUM_12
#define LCD_D0 GPIO_NUM_11
#define LCD_D1 GPIO_NUM_13
#define LCD_D2 GPIO_NUM_14
#define LCD_D3 GPIO_NUM_15
#define LCD_RST GPIO_NUM_17
#define LCD_EN GPIO_NUM_16            /* panel power; high to run */
#define LCD_RES 466
#define LCD_GAP_X 6                   /* the 466 columns in the controller's RAM */
#define LCD_HOST SPI2_HOST

#define I2C_SDA GPIO_NUM_7
#define I2C_SCL GPIO_NUM_6
/* The CST9217's INT (GPIO9) is shared with the PCF8563's; the touch is polled,
 * so neither is used. No reset line reaches the touch. */

/* SY6970 charger (I2C 0x6A): battery voltage and charge state. */
#define SY6970_ADDR 0x6A
#define SY6970_REG_ADC 0x02           /* CONV_START / CONV_RATE */
#define SY6970_REG_WDT 0x07           /* watchdog bits 5:4 */
#define SY6970_REG_BATFET 0x09        /* BATFET_DIS bit 5 */
#define SY6970_REG_STATUS 0x0B        /* VBUS_STAT 7:5, CHRG_STAT 4:3, PG_STAT 2 */
#define SY6970_REG_BATV 0x0E          /* 2304 mV + 20 mV * bits 6:0 */

#define DRAW_BUF_LINES 118            /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_sy6970;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static muse_gpio_button_t s_boot;
static uint8_t s_lcd_level = 0xA0;

static esp_err_t sy6970_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_sy6970, &reg, 1, val, 1, 100);
}

static esp_err_t sy6970_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_sy6970, buf, 2, 100);
}

static esp_err_t init(void)
{
    /* Panel power first, so the controller is up before display_start resets it. */
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << LCD_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&en_cfg), TAG, "lcd en");
    gpio_set_level(LCD_EN, 1);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    const i2c_device_config_t chg_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SY6970_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &chg_cfg, &s_sy6970), TAG, "sy6970");
    /* Host mode: without this the charger's watchdog resets its settings
     * every 40 s. Then continuous ADC, so REG_BATV always holds a fresh
     * conversion when read_power polls; the register's other bits (input
     * source detection, ICO) keep their values, as in LilyGO's driver. And
     * the battery back on: power_off() leaves the BATFET open, which on USB
     * only ends in deep sleep, and with it open the cell doesn't charge. */
    uint8_t wdt, adc, batfet;
    if (sy6970_read(SY6970_REG_WDT, &wdt) == ESP_OK) {
        esp_err_t err = sy6970_write(SY6970_REG_WDT, wdt & ~0x30);
        if (err == ESP_OK) {
            err = sy6970_read(SY6970_REG_ADC, &adc);
        }
        if (err == ESP_OK) {
            err = sy6970_write(SY6970_REG_ADC, adc | 0xC0);   /* CONV_START | CONV_RATE */
        }
        if (err == ESP_OK) {
            err = sy6970_read(SY6970_REG_BATFET, &batfet);
        }
        if (err == ESP_OK && (batfet & 0x20)) {
            err = sy6970_write(SY6970_REG_BATFET, batfet & ~0x20);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SY6970 setup failed (%s); battery state may be stale", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "SY6970 not answering; battery state unavailable");
    }

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
    /* BOOT is held at flashing time; don't count a held button as a press. */
    s_boot.pressed = gpio_get_level(GPIO_NUM_0) == 0;
    return ESP_OK;
}

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* A CO5300 command over QSPI, sent between flush bands so it never lands in
 * the middle of a pixel write. */
static void lcd_cmd(uint8_t cmd, const void *param, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (cmd << 8), param, n);
}

static void send_brightness(void *level)
{
    s_lcd_level = *(uint8_t *)level;
    lcd_cmd(0x51, &s_lcd_level, 1);
}

static void send_sleep(void *sleep)
{
    lcd_cmd(*(bool *)sleep ? 0x10 : 0x11, NULL, 0);
}

static void send_off(void *arg)
{
    esp_lcd_panel_disp_on_off(s_panel, false);
}

/* muse_lcd_bands_register takes over the panel IO's color-done callback and,
 * with the bands, needs the bus set up on the same core as the UI task. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        CO5300_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    co5300_vendor_config_t vendor_cfg = { .flags.use_qspi_interface = 1 };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "co5300");
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);
    lcd_cmd(0x51, &s_lcd_level, 1);   /* visible until the UI sets its own */

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
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io");
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* no reset line to the chip */
        .int_gpio_num = GPIO_NUM_NC,   /* INT shares GPIO9 with the RTC; polled */
        .flags = {
            .swap_xy = 0,
            /* The CST9217 reports both axes inverted on this panel (LilyGO's
             * example subtracts both coordinates from the resolution). */
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    if (esp_lcd_touch_new_i2c_cst9217(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "cst9217 touch");
        return NULL;
    }
    const esp_lv_adapter_touch_config_t tp_lvgl_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
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

/* AMOLED: no backlight pin, brightness is the panel's 0x51 command. */
static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

/* SLPIN/SLPOUT: GRAM is kept. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/*
 * BOOT wakes the screen and confirms pairing; without a microphone there is
 * nothing to talk into, so no hold-to-talk on the face. A tap also confirms
 * pairing, like the AMOLED-1.43.
 */
static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_boot);

    static bool down;
    static bool confirming;
    bool pressed = s_touch_indev && lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED;
    if (pressed && !down && muse_link_state() == MUSE_LINK_CONFIRM) {
        confirming = true;
        ev |= MUSE_BTN_TALK_PRESS;
    } else if (!pressed && down && confirming) {
        confirming = false;
        ev |= MUSE_BTN_TALK_RELEASE;
    }
    down = pressed;
    return ev;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/* The SY6970's own ADC reads the cell terminal voltage; no divider involved.
 * Percentage stays off anyway: while charging the terminal sits at the charge
 * voltage, which would read as a full cell and run the battery policy on the
 * charger. Voltage and the real charge state are reported. */
static esp_err_t read_power(muse_power_t *out)
{
    uint8_t batv, status;
    ESP_RETURN_ON_ERROR(sy6970_read(SY6970_REG_BATV, &batv), TAG, "batv");
    ESP_RETURN_ON_ERROR(sy6970_read(SY6970_REG_STATUS, &status), TAG, "status");
    out->battery_mv = 2304 + 20 * (batv & 0x7F);
    out->battery_pct = -1;
    unsigned chrg = (status >> 3) & 0x3;   /* 01 pre-charge, 10 fast charge */
    out->charging = chrg == 1 || chrg == 2;
    out->usb = (status & 0x04) || usb_serial_jtag_is_connected();   /* PG_STAT */
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(send_off, NULL);
    panel_sleep(true);
    gpio_set_level(LCD_EN, 0);
    /* On battery, opening the BATFET is a real power off (plugging USB starts
     * it again). Only when the charger says there's no input power (PG_STAT
     * clear): with input, or if it can't be read, the BATFET stays closed, so
     * the cell keeps charging, and the board deep-sleeps with BOOT as the
     * wake button. */
    uint8_t status, batfet;
    bool on_battery = sy6970_read(SY6970_REG_STATUS, &status) == ESP_OK && !(status & 0x04);
    if (on_battery && sy6970_read(SY6970_REG_BATFET, &batfet) == ESP_OK) {
        sy6970_write(SY6970_REG_BATFET, batfet | 0x20);
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0), TAG, "boot wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "LilyGO T-Display-S3 AMOLED 1.75",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.75f,
    .talk_button = "boot",
    /* BOOT is on the side edge of the puck. */
    .talk_hint = { LV_ALIGN_TOP_LEFT, 20, 16 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    /* No speaker or microphone on this board: audio_init stays NULL and the
     * replies are text on screen. */
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
