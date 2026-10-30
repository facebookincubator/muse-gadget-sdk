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
 * Waveshare ESP32-S3-Touch-AMOLED-1.43 (the plain one, not the 1.43C): round
 * 466 px SH8601/CO5300 QSPI AMOLED with FT3168 touch, QMI8658 IMU and PCF85063
 * RTC on the same I2C bus, no speaker or microphone, no PMU. The panel's reset
 * is a plain GPIO; there is no I/O expander. BOOT (GPIO0) is the talk button,
 * but without a mic it only wakes the screen and confirms pairing (replies
 * are text), and a tap confirms pairing too. Pins from arduino-esp32's
 * variants/waveshare_esp32_s3_touch_amoled_143.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
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
#include "esp_lcd_touch_ft5x06.h"

static const char *TAG = "board";

/* Pins (arduino-esp32 variant / board wiki). */
#define LCD_CS GPIO_NUM_9
#define LCD_SCLK GPIO_NUM_10
#define LCD_D0 GPIO_NUM_11
#define LCD_D1 GPIO_NUM_12
#define LCD_D2 GPIO_NUM_13
#define LCD_D3 GPIO_NUM_14
#define LCD_RST GPIO_NUM_21
#define LCD_RES 466
#define LCD_GAP_X 6                /* the 466 columns in the controller's 480 */
#define LCD_HOST SPI2_HOST

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
/* The FT3168's INT and RST aren't wired to the chip. */

/* The arduino-esp32 variant puts the battery divider on GPIO4 (ADC1 CH3);
 * the wiki says GPIO5. The variant matched the hardware here. */
#define BAT_ADC ADC_CHANNEL_3

#define DRAW_BUF_LINES 118         /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static uint8_t s_lcd_level = 0xA0;

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

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
    /* BOOT is held at flashing time; don't count a held button as a press. */
    s_boot.pressed = gpio_get_level(GPIO_NUM_0) == 0;

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BAT_ADC, &ch_cfg), TAG, "adc channel");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BAT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable; battery mV is uncalibrated");
    }
#endif
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
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io");
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* not wired to the chip */
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "ft3168 touch");
        return NULL;
    }
    /* The FT3168 sits out one read every ~3.7 s (a NACK; the FT5x06 monitor
     * register doesn't change it). The touch state just carries to the next
     * poll 30 ms later, but the driver logs every miss as an error, so those
     * two tags go quiet. The display's own IO is SPI, not behind these. */
    esp_log_level_set("FT5x06", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
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
 * pairing, which saves digging for the side button while it dangles on a
 * lanyard.
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

/* The battery reaches the ADC through the board's divider; 200K/100K (3x)
 * like the other boards in this family, not yet confirmed against a cell. */
static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BAT_ADC, &v), TAG, "adc read");
        sum += v;
    }
    int raw = sum / 8;
    int mv;
    if (s_cali) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
    } else {
        mv = raw * 3300 / 4096;
    }
    out->battery_mv = mv * 3;
    /* Rough Li-ion window: 3.3 V empty, 4.2 V full. Far below empty, no
     * cell is fitted: report no battery rather than 0%. */
    int pct = (out->battery_mv - 3300) * 100 / (4200 - 3300);
    out->battery_pct = out->battery_mv < 2500 ? -1 : pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->charging = false;   /* no charge-state line reaches the chip */
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    panel_sleep(true);
    /* No PMU; deep sleep is the closest thing to off. BOOT wakes it. */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.43",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.43f,
    .talk_button = "boot",
    /* BOOT is on the side edge, toward the top of the puck. */
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
