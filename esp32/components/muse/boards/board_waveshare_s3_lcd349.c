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
 * Waveshare ESP32-S3-Touch-LCD-3.49 V2 (and the 3.49B, the same board in a
 * case with a LiPo): ESP32-S3R8 with 16 MB flash and 8 MB octal PSRAM, a
 * 3.49" 172x640 IPS bar on an AXS15231B over QSPI with the controller's own
 * capacitive touch, an ES8311 codec into an NS4150B amp for the speaker, an
 * ES7210 ADC for two mics, a TCA9554 expander, BOOT and PWR keys and a battery
 * on a divider. Native USB Serial/JTAG.
 *
 * Only V2 ("Rev1.1" on the PCB, "V2" on the case, every unit shipped from
 * 2026-06-08): V1 has the backlight on GPIO8 and the panel's reset on GPIO21,
 * which V2 swapped for the expander's INT and the panel's TE.
 *
 * Pins, the expander's lines, the panel's two-command init and its reset, the
 * backlight's inverted PWM and the touch read come from Waveshare's V2
 * schematic and factory firmware, waveshareteam/ESP32-S3-Touch-LCD-3.49-V2
 * (schematic/, ESP-IDF/11_FactoryProgram/main/main.cpp and user_config.h,
 * components/codec_board/board_cfg.txt for the codecs' I2S pins).
 *
 * As on the Guition JC3248W535, in QSPI mode the AXS15231B ignores the row
 * address, so LVGL renders in direct mode into one full-screen buffer and every
 * frame goes out whole, top to bottom.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_W 172
#define LCD_H 640
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_9
#define LCD_SCLK GPIO_NUM_10
#define LCD_D0 GPIO_NUM_11
#define LCD_D1 GPIO_NUM_12
#define LCD_D2 GPIO_NUM_13
#define LCD_D3 GPIO_NUM_14
#define LCD_BL GPIO_NUM_42         /* PWM into the backlight boost's feedback: more duty, less light */
#define CHUNK_ROWS 16              /* rows per QSPI transfer, out of internal RAM */
#define CHUNK_BYTES (LCD_W * CHUNK_ROWS * 2)

#define I2C_SDA GPIO_NUM_47        /* TCA9554, ES8311, ES7210 */
#define I2C_SCL GPIO_NUM_48
#define EXIO_ADDR 0x20
#define TP_SDA GPIO_NUM_17         /* the touch controller has a bus of its own */
#define TP_SCL GPIO_NUM_18
#define TP_ADDR 0x3B

/* TCA9554 lines; the rest (touch, IMU and RTC interrupts) are inputs. */
#define EXIO_BL_EN 1
#define EXIO_LCD_RST 5             /* the panel's reset, its touch included */
#define EXIO_SYS_EN 6              /* high holds the battery switch on */
#define EXIO_PA_EN 7               /* NS4150B */
#define EXIO_OUTPUTS (1u << EXIO_BL_EN | 1u << EXIO_LCD_RST | 1u << EXIO_SYS_EN | 1u << EXIO_PA_EN)

#define I2S_MCLK GPIO_NUM_7
#define I2S_BCLK GPIO_NUM_15
#define I2S_WS GPIO_NUM_46
#define I2S_DOUT GPIO_NUM_45
#define I2S_DIN GPIO_NUM_6

#define TALK_GPIO GPIO_NUM_0       /* BOOT */
#define PWR_GPIO GPIO_NUM_16       /* PWR, through a diode; it also turns the board on from the battery */
#define BATT_ADC ADC_CHANNEL_3     /* GPIO4: VBAT through 200k / 100k */

/* The factory firmware's: the controller's defaults suit this glass. */
static const axs15231b_lcd_init_cmd_t s_lcd_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exio;
static SemaphoreHandle_t s_exio_lock;
static uint8_t s_exio_out;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_tp;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_fb;                  /* the whole screen, in PSRAM */
static uint8_t *s_chunk[2];            /* DMA-capable copies on their way out */
static SemaphoreHandle_t s_chunk_free;
static SemaphoreHandle_t s_lv_lock;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;
static bool s_bl_ready;
static muse_gpio_button_t s_talk, s_pwr;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static esp_err_t exio_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_exio, buf, sizeof(buf), 50);
}

static esp_err_t exio_set(int line, bool on)
{
    xSemaphoreTake(s_exio_lock, portMAX_DELAY);
    s_exio_out = on ? s_exio_out | 1u << line : s_exio_out & ~(1u << line);
    esp_err_t err = exio_write(0x01, s_exio_out);
    xSemaphoreGive(s_exio_lock);
    return err;
}

/* The codec driver switches the amp through this as the speaker opens and closes. */
static int exio_gpio_setup(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t mode)
{
    (void)gpio;
    (void)dir;
    (void)mode;
    return ESP_CODEC_DEV_OK;
}

static int exio_gpio_set(int16_t gpio, bool high)
{
    return exio_set(gpio, high) == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static bool exio_gpio_get(int16_t gpio)
{
    return s_exio_out >> gpio & 1;
}

static const audio_codec_gpio_if_t s_exio_gpio = {
    .setup = exio_gpio_setup,
    .set = exio_gpio_set,
    .get = exio_gpio_get,
};

static esp_err_t init(void)
{
    /* Low before anything else: the boost would light the glass white meanwhile. */
    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight pin");
    gpio_set_level(LCD_BL, 0);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t exio_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXIO_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exio_cfg, &s_exio), TAG, "expander");
    s_exio_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exio_lock, ESP_ERR_NO_MEM, TAG, "expander lock");
    /* On battery the board stays on only while PWR is held until SYS_EN is,
     * so it comes first. Levels before directions: an output takes its level
     * the moment it becomes one. Amp and backlight off, the panel out of reset. */
    s_exio_out = (uint8_t)(~EXIO_OUTPUTS | 1u << EXIO_SYS_EN | 1u << EXIO_LCD_RST);
    ESP_RETURN_ON_ERROR(exio_write(0x01, s_exio_out), TAG, "expander levels");
    ESP_RETURN_ON_ERROR(exio_write(0x03, (uint8_t)~EXIO_OUTPUTS), TAG, "expander directions");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_pwr, PWR_GPIO), TAG, "power button");
    /* A key that started the board or the bootloader is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_pwr.pressed = gpio_get_level(PWR_GPIO) == 0;

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "battery adc");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    return adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    /* esp_lcd's SPI IO ignores the return value: without this the woken
     * flush can wait for the next tick, once a piece. */
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
    return woken == pdTRUE;
}

/*
 * Direct mode: LVGL redraws only what changed, in place in s_fb, and calls
 * this once per area. After the last one the whole frame goes out from row 0,
 * since the panel can't be sent a window lower down. Each piece is copied
 * into internal RAM while the one before is on the wire (the SPI DMA can't
 * keep up reading PSRAM while both cores draw).
 */
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (lv_display_flush_is_last(disp)) {
        int k = 0;
        for (int y = 0; y < LCD_H; y += CHUNK_ROWS) {
            xSemaphoreTake(s_chunk_free, portMAX_DELAY);
            memcpy(s_chunk[k], s_fb + (size_t)y * LCD_W * 2, CHUNK_BYTES);
            if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + CHUNK_ROWS, s_chunk[k]) != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
            }
            k ^= 1;
        }
        /* Both pieces back: the frame is on the panel and s_fb is LVGL's again. */
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        xSemaphoreGive(s_chunk_free);
        xSemaphoreGive(s_chunk_free);
    }
    lv_display_flush_ready(disp);
}

/*
 * The factory firmware's read, as one transaction: this glass answered a
 * separate write and read with nothing. A record is a header, the point count,
 * then X and Y in 12 bits each, X's top two bits the event (1: lifted). 0xD3
 * in every byte means nothing new; the controller reports about every 30 ms,
 * so the last point stands that long. It reports along the glass's long side
 * first, from the bottom.
 */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static const uint8_t cmd[11] = { 0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 };
    static bool down;
    static int16_t last_x, last_y;
    static int64_t last_us;
    uint8_t d[32];
    bool ok = i2c_master_transmit_receive(s_tp, cmd, sizeof(cmd), d, sizeof(d), 20) == ESP_OK;
    bool fresh = false;
    for (int i = 0; ok && i < 14; i++) {
        fresh = fresh || d[i] != 0xD3;
    }
    int64_t now = esp_timer_get_time();
    if (fresh) {
        down = d[1] > 0 && d[1] < 5 && (d[2] >> 6) != 1;
        if (down) {
            int along = (d[2] & 0x0F) << 8 | d[3];
            int across = (d[4] & 0x0F) << 8 | d[5];
            last_x = across < LCD_W ? across : LCD_W - 1;
            last_y = along < LCD_H ? LCD_H - 1 - along : 0;
        }
        last_us = now;
    } else if (now - last_us > 80000) {
        down = false;
    }
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Before the panel's reset, which resets the touch controller too, as the
 * factory firmware has it. */
static esp_err_t touch_bus_start(i2c_master_bus_handle_t *bus)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = TP_SDA,
        .scl_io_num = TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, bus);
}

static esp_err_t touch_start(i2c_master_bus_handle_t bus)
{
    /* The controller stretches SCL while it composes an answer. */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 300000,
        .scl_wait_us = 200000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_tp), TAG, "touch device");
    return i2c_master_probe(bus, TP_ADDR, 50);
}

/* Runs on the LVGL task, so the panel's SPI interrupt lands on its core
 * (see muse_lcd_bands.h for why that matters). */
static esp_err_t lcd_start(void)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,               /* dimmest; BL_EN keeps it dark until the UI sets a level */
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_ch), TAG, "backlight");
    s_bl_ready = true;

    s_fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_chunk[0] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    ESP_RETURN_ON_FALSE(s_fb && s_chunk[0] && s_chunk[1] && s_chunk_free, ESP_ERR_NO_MEM, TAG, "frame buffers");
    memset(s_fb, 0, LCD_W * LCD_H * 2);

    i2c_master_bus_handle_t tp_bus;
    esp_err_t tp_err = touch_bus_start(&tp_bus);

    const spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, CHUNK_BYTES);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "lcd bus");
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, on_chunk_sent, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io), TAG, "lcd io");
    axs15231b_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io, &panel_cfg, &s_panel), TAG, "lcd panel");
    ESP_RETURN_ON_ERROR(exio_set(EXIO_LCD_RST, true), TAG, "lcd reset");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(exio_set(EXIO_LCD_RST, false), TAG, "lcd reset");
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_RETURN_ON_ERROR(exio_set(EXIO_LCD_RST, true), TAG, "lcd reset");
    vTaskDelay(pdMS_TO_TICKS(30));
    /* No esp_lcd_panel_disp_on_off(), here or later: the component sends 0x28
     * for "on", and on this in-cell glass that stops the touch scan. The init
     * list ends in 0x29. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "lcd init");

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(LCD_W, LCD_H);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "lv display");
    /* The panel takes RGB565 big-endian over SPI. */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(s_disp, s_fb, NULL, LCD_W * LCD_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);

    if (tp_err == ESP_OK && touch_start(tp_bus) == ESP_OK) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read);
        lv_indev_set_display(s_indev, s_disp);
    } else {
        ESP_LOGW(TAG, "touch controller not answering at 0x%02x", TP_ADDR);
    }
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_start_err = lcd_start();
    xSemaphoreGive(s_started);
    if (s_start_err != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        xSemaphoreTakeRecursive(s_lv_lock, portMAX_DELAY);
        uint32_t ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lv_lock);
        ms = ms < 5 ? 5 : ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    s_started = xSemaphoreCreateBinary();
    if (!s_lv_lock || !s_started ||
        xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return NULL;
    }
    xSemaphoreTake(s_started, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        return NULL;
    }
    *touch = s_indev;
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lv_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

/* The factory firmware writes 255 - brightness; BL_EN gates the boost. */
static void set_brightness(int pct)
{
    if (!s_bl_ready) {
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (100 - pct) * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    exio_set(EXIO_BL_EN, pct > 0);
}

/* ES8311 out and ES7210 in on one duplex I2S bus with MCLK. The amp's enable
 * is on the expander, which the ES8311 driver drives as its PA pin. */
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
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = &s_exio_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = EXIO_PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_pwr) << 2;
}

/* VBAT x 3. No charger status or VBUS sense reaches the ESP32; USB counts
 * only with a host on the other end. */
static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        sum += raw;
    }
    int mv;
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, sum / 8, &mv), TAG, "adc calibration");
    int v = mv * 3;
    out->usb = usb_serial_jtag_is_connected();
    out->charging = false;
    out->battery_mv = v;
    if (v < 2500) {
        out->battery_pct = -1;     /* no cell: the divider reads near zero */
        return ESP_OK;
    }
    /* The Watcher's LiPo curve, as on the StickS3. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/*
 * Screen and amp off, the battery switch let go: on battery the board goes
 * dark once PWR is released. On USB it stays powered, so it deep-sleeps until
 * PWR is pressed.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    exio_set(EXIO_PA_EN, false);
    exio_set(EXIO_SYS_EN, false);
    while (gpio_get_level(PWR_GPIO) == 0 || gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(PWR_GPIO, 0), TAG, "power button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-3.49",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 3.49f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* The keys' centres on the right edge, measured against the glass: PWR at
     * y 530, BOOT at 600 (RESET, at 450, resets the chip and isn't read). */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -8, 600 - LCD_H / 2 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 530 - LCD_H / 2 },
    .frame_ms = 40,
    .avatar_px = 172,           /* the full width; whole 2x cells would leave 22 px each side */
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,             /* two mics, one on each slot */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
