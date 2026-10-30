/*
 * Copyright (c) 2026 limengdu.
 * SPDX-License-Identifier: Apache-2.0
 *
 * reTerminal Sticky: ESP32-S3, 32 MB flash, 8 MB octal PSRAM, SSD1677,
 * GT911, PDM mic, BQ27220 and SD on the panel's SPI2 bus.
 * Hardware reference: Seeed-reTerminal-E10xx-Firmware, factory_fw
 * main/boards/reterminal_e1005/config.h and reterminal_e1005.cpp (MIT,
 * Copyright (c) 2026 JoJang2u). FreeInk drives the panel (its "sticky"
 * profile), and Muse's e-ink layer draws on it (eink/).
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_link.h"
#include "eink/muse_eink.h"
#if CONFIG_MUSE_STICKY_SD
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#endif

#define SCREEN_W 480
#define SCREEN_H 800

static const char *TAG = "sticky";
static muse_gpio_button_t s_ai, s_up, s_down;
static i2c_master_bus_handle_t s_sensor_bus, s_touch_bus;
static i2c_master_dev_handle_t s_gauge;
static esp_lcd_touch_handle_t s_touch;
static i2s_chan_handle_t s_mic;
#if CONFIG_MUSE_STICKY_SD
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_sd_lock;
#endif
static bool s_was_pressed, s_mic_on;
static int64_t s_gauge_log_us;

static esp_err_t gauge_connect(void)
{
    if (s_gauge) return ESP_OK;
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_sensor_bus, 0x55, 100), TAG, "BQ27220 probe");
    const i2c_device_config_t gauge = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x55,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(s_sensor_bus, &gauge, &s_gauge);
}

static esp_err_t touch_start(void);
#if CONFIG_MUSE_STICKY_SD
static esp_err_t sd_bus_init(void)
{
    /* The panel is write-only; the SD card also needs its MISO line. FreeInk
     * reuses this already-initialized bus without changing its pins. */
    const spi_bus_config_t bus = {
        .mosi_io_num = GPIO_NUM_14,
        .miso_io_num = GPIO_NUM_12,
        .sclk_io_num = GPIO_NUM_13,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    return spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
}

static void sd_task(void *arg);
#endif

static esp_err_t init(void)
{
    const gpio_config_t outputs = {
        .pin_bit_mask = BIT64(8) | BIT64(10) | BIT64(38) | BIT64(39) | BIT64(42),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "output GPIOs");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_8, 1), TAG, "SD chip select");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_10, 1), TAG, "SD power");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_38, 0), TAG, "mic power");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_39, 0), TAG, "charger enable");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_42, 1), TAG, "touch power");
    const gpio_config_t inputs = {
        .pin_bit_mask = BIT64(9) | BIT64(40),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&inputs), TAG, "power inputs");
#if CONFIG_MUSE_STICKY_SD
    const gpio_config_t sd_detect = {
        .pin_bit_mask = BIT64(11),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sd_detect), TAG, "SD detect");
#endif
    gpio_sleep_sel_dis(GPIO_NUM_45);
    gpio_sleep_sel_dis(GPIO_NUM_46);
    gpio_sleep_sel_dis(GPIO_NUM_47);
    gpio_sleep_sel_dis(GPIO_NUM_10);
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_ai, GPIO_NUM_4), TAG, "AI button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_up, GPIO_NUM_5), TAG, "up button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_down, GPIO_NUM_6), TAG, "down button");

    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_1,
        .scl_io_num = GPIO_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_1, &s_sensor_bus);
    if (err != ESP_OK) err = i2c_new_master_bus(&cfg, &s_sensor_bus);
    ESP_RETURN_ON_ERROR(err, TAG, "sensor I2C");
    if (gauge_connect() != ESP_OK) ESP_LOGW(TAG, "BQ27220 not responding; retrying later");
    if (touch_start() != ESP_OK) ESP_LOGW(TAG, "GT911 unavailable; buttons remain active");
#if CONFIG_MUSE_STICKY_SD
    ESP_RETURN_ON_ERROR(sd_bus_init(), TAG, "shared SD/panel SPI bus");
    s_sd_lock = xSemaphoreCreateMutex();
    if (!s_sd_lock || xTaskCreate(sd_task, "sticky_sd", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "SD hotplug monitor unavailable");
    }
#endif
    return ESP_OK;
}

static esp_err_t touch_start(void)
{
    const gpio_config_t control = {
        .pin_bit_mask = BIT64(21) | BIT64(41),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&control), TAG, "touch control");
    gpio_set_level(GPIO_NUM_21, 0);
    gpio_set_level(GPIO_NUM_41, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(GPIO_NUM_41, 1);
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT), TAG, "touch INT");
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_3,
        .scl_io_num = GPIO_NUM_2,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_touch_bus), TAG, "touch I2C");
    uint8_t addr = 0x5d;
    if (i2c_master_probe(s_touch_bus, addr, 100) != ESP_OK) addr = 0x14;
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_touch_bus, addr, 100), TAG, "GT911 probe");
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_touch_bus, &io_cfg, &io), TAG, "GT911 I2C");
    esp_lcd_touch_io_gt911_config_t gt = {.dev_addr = addr};
    const esp_lcd_touch_config_t tp = {
        .x_max = SCREEN_W,
        .y_max = SCREEN_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .driver_data = &gt,
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp, &s_touch);
}

/* The digitizer counts rows from the bottom. */
static uint16_t sticky_touch_y(uint16_t raw)
{
    return raw < SCREEN_H ? SCREEN_H - 1 - raw : 0;
}

static bool touch_read(int16_t *x, int16_t *y)
{
    esp_lcd_touch_point_data_t point;
    uint8_t count = 0;
    if (!s_touch || esp_lcd_touch_read_data(s_touch) != ESP_OK ||
        esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK || !count) {
        s_was_pressed = false;
        return false;
    }
    *x = point.x < SCREEN_W ? point.x : SCREEN_W - 1;
    *y = sticky_touch_y(point.y);
    if (!s_was_pressed && muse_link_state() == MUSE_LINK_CONFIRM) muse_link_talk_press();
    s_was_pressed = true;
    return true;
}

#if CONFIG_MUSE_STICKY_SD
static void sd_task(void *arg)
{
    (void)arg;
    int64_t retry_at = 0;
    for (;;) {
        xSemaphoreTake(s_sd_lock, portMAX_DELAY);
        bool inserted = !gpio_get_level(GPIO_NUM_11);
        if (!inserted && s_card) {
            esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
            s_card = NULL;
            ESP_LOGI(TAG, "SD card removed");
        }
        if (inserted && !s_card && esp_timer_get_time() >= retry_at) {
            sdmmc_host_t host = SDSPI_HOST_DEFAULT();
            host.slot = SPI2_HOST;
            host.max_freq_khz = 20000;
            sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
            slot.host_id = SPI2_HOST;
            slot.gpio_cs = GPIO_NUM_8;
            const esp_vfs_fat_sdmmc_mount_config_t config = {
                .format_if_mount_failed = false,
                .max_files = 4,
            };
            esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &config, &s_card);
            if (err == ESP_OK) ESP_LOGI(TAG, "SD card mounted at /sdcard");
            else {
                s_card = NULL;
                ESP_LOGW(TAG, "SD mount: %s", esp_err_to_name(err));
                retry_at = esp_timer_get_time() + 5000000;
            }
        }
        xSemaphoreGive(s_sd_lock);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = MUSE_AUDIO_CHUNK;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_mic), TAG, "PDM channel");
    const i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = GPIO_NUM_19, .din = GPIO_NUM_20},
    };
    return i2s_channel_init_pdm_rx_mode(s_mic, &pdm);
}

static esp_err_t mic_power(bool on)
{
    if (on == s_mic_on) return ESP_OK;
    if (on) {
        gpio_set_level(GPIO_NUM_38, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_err_t err = i2s_channel_enable(s_mic);
        if (err != ESP_OK) {
            gpio_set_level(GPIO_NUM_38, 0);
            return err;
        }
    } else {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_mic), TAG, "stop PDM");
        gpio_set_level(GPIO_NUM_38, 0);
    }
    s_mic_on = on;
    return ESP_OK;
}

static esp_err_t mic_read(int16_t *mono, size_t frames)
{
    size_t bytes = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_read(s_mic, mono, frames * sizeof(*mono), &bytes, 1000),
                        TAG, "PDM read");
    return bytes == frames * sizeof(*mono) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_ai) |
           (muse_gpio_button_poll(&s_up) & MUSE_BTN_TALK_PRESS ? MUSE_BTN_UP : 0) |
           (muse_gpio_button_poll(&s_down) & MUSE_BTN_TALK_PRESS ? MUSE_BTN_DOWN : 0);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_button_t *const lines[] = {&s_ai, &s_up, &s_down};
    muse_gpio_buttons_wait(lines, 3, timeout_ms);
}

static esp_err_t gauge_read(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(s_gauge, &reg, 1, data, sizeof(data), 100);
    if (err != ESP_OK) {
        int64_t now = esp_timer_get_time();
        if (!s_gauge_log_us || now - s_gauge_log_us > 60000000) {
            ESP_LOGW(TAG, "BQ27220 reg 0x%02x: %s (SDA=%d SCL=%d)", reg,
                     esp_err_to_name(err), gpio_get_level(GPIO_NUM_1), gpio_get_level(GPIO_NUM_0));
            s_gauge_log_us = now;
        }
        return err;
    }
    *value = (uint16_t)data[0] | (uint16_t)data[1] << 8;
    return ESP_OK;
}

static esp_err_t read_power(muse_power_t *out)
{
    out->usb = gpio_get_level(GPIO_NUM_9);
    out->battery_pct = -1;
    out->battery_mv = 0;
    out->charging = false;
    if (!s_gauge && gauge_connect() != ESP_OK) return ESP_OK;
    uint16_t mv, soc, current;
    if (gauge_read(0x08, &mv) != ESP_OK || gauge_read(0x2c, &soc) != ESP_OK ||
        gauge_read(0x0c, &current) != ESP_OK) return ESP_OK;
    if (mv < 2500 || mv > 5000 || soc > 100) return ESP_OK;
    if (s_gauge_log_us) {
        ESP_LOGI(TAG, "BQ27220 reads recovered");
        s_gauge_log_us = 0;
    }
    out->battery_mv = mv;
    out->battery_pct = soc;
    out->charging = out->usb && (int16_t)current > 0;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    if (gpio_get_level(GPIO_NUM_9)) return ESP_ERR_NOT_SUPPORTED;
#if CONFIG_MUSE_STICKY_SD
    if (s_sd_lock) {
        xSemaphoreTake(s_sd_lock, portMAX_DELAY);
        if (s_card) {
            esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
            s_card = NULL;
        }
        xSemaphoreGive(s_sd_lock);
    }
#endif
    gpio_hold_dis(GPIO_NUM_45);
    gpio_set_level(GPIO_NUM_45, 0);
    gpio_set_level(GPIO_NUM_46, 0);
    esp_rom_delay_us(10);
    gpio_set_level(GPIO_NUM_46, 1);
    esp_rom_delay_us(10);
    gpio_set_level(GPIO_NUM_46, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Seeed reTerminal Sticky",
    .width = SCREEN_W,
    .height = SCREEN_H,
    .epaper = true,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 3.97f,
    .talk_button = "AI button",
    .frame_ms = 700,    /* about the fastest a fast refresh (610 ms) keeps up with */
    .init = init,
    .display_pause = muse_eink_display_pause,
    .touch_read = touch_read,
    .mic_init = mic_init,
    .mic_power = mic_power,
    .mic_read = mic_read,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
