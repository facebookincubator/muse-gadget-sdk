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
 * WeatherXM WG1200 (Seeed SenseCAP Indicator D1L) board driver for Muse.
 *
 * Hardware summary:
 * - MCU: ESP32-S3R8 (8 MB Octal PSRAM, 16 MB Quad Flash, Secure Boot V2)
 * - I2C0: SDA=GPIO39, SCL=GPIO40 (400 kHz)
 *   - PCA9535 16-bit I/O expander at 0x20 (or 0x39)
 *   - FT6336U capacitive touch at 0x48 (GX panel) or 0x38 (DX panel)
 *   - BMP390 indoor barometer/thermometer at 0x77 (powered via PCA9535 IO1_2)
 * - Display: 4.0" 480x480 RGB565 panel (ST7701S controller)
 *   - 9-bit SPI init shares SCK=GPIO41, MOSI=GPIO48 with SX1262; CS=IO0_4, RST=IO0_5
 *   - 16-bit RGB parallel interface: PCLK=21, HSYNC=16, VSYNC=17, DE=18,
 *     DATA[0..15] = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}
 *   - Backlight: GPIO45 (LEDC PWM 20 kHz)
 * - Button: GPIO38 (top active-low user button)
 *   - Short press: sleep/wake display
 *   - Double press: toggle BLE phone setup
 *   - 5-second hold, then release: reset Muse (pairing, Wi-Fi, Muse token)
 *   - 10-second hold: hardware failsafe to reboot into WeatherXM firmware
 */

#include "boards/board_wxm_wg1200.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "wxm_boot_mgr.h"

#define LCD_W 480
#define LCD_H 480

#define WG1200_I2C_SDA          GPIO_NUM_39
#define WG1200_I2C_SCL          GPIO_NUM_40
#define WG1200_SPI_SCK          GPIO_NUM_41
#define WG1200_SPI_MOSI         GPIO_NUM_48
#define WG1200_LCD_BL_GPIO      GPIO_NUM_45
#define WG1200_BTN_TOP_GPIO     GPIO_NUM_38

/* PCA9535 registers */
#define PCA9535_REG_IN0         0x00
#define PCA9535_REG_IN1         0x01
#define PCA9535_REG_OUT0        0x02
#define PCA9535_REG_OUT1        0x03
#define PCA9535_REG_CFG0        0x06
#define PCA9535_REG_CFG1        0x07

/* Initial PCA9535 state (per WeatherXM-WG1200-v1.1_SCH.pdf Page 4 & Page 6):
 * Outputs: LORA_NSS(IO0_0)=1, LORA_RST(IO0_1)=1, LCD_CS(IO0_4)=1, LCD_RST(IO0_5)=1, TP_RST(IO0_7)=1,
 *          BMP_PWR(IO1_2)=1, SD_CS(IO1_3)=1, BUZZER(IO1_5)=0.
 * Inputs (1 in CFG): LORA_BUSY(IO0_2), LORA_DIO1(IO0_3), TP_INT(IO0_6),
 *                    RP_RST(IO1_0), ECC_PWR(IO1_1), SD_DET(IO1_4), IO1_6, IO1_7.
 */
#define PCA9535_INIT_OUT0       ((1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 7))
#define PCA9535_INIT_OUT1       ((1u << 2) | (1u << 3))
#define PCA9535_INIT_CFG0       ((1u << 2) | (1u << 3) | (1u << 6))
#define PCA9535_INIT_CFG1       ((1u << 0) | (1u << 1) | (1u << 4) | (1u << 6) | (1u << 7))

static const char *TAG = "board_wg1200";

static i2c_master_bus_handle_t s_i2c;
static SemaphoreHandle_t s_i2c_lock;
static i2c_master_dev_handle_t s_pca_dev;
static i2c_master_dev_handle_t s_tp_dev;
static uint8_t s_pca_out[2] = { PCA9535_INIT_OUT0, PCA9535_INIT_OUT1 };
static uint8_t s_pca_cfg[2] = { PCA9535_INIT_CFG0, PCA9535_INIT_CFG1 };
static uint8_t s_tp_addr = 0x48;

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_lv_lock;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;
static bool s_touch_was_pressed;
static muse_gpio_button_t s_btn_top;
static bool s_bl_ready;

i2c_master_bus_handle_t wg1200_i2c_bus(void)
{
    return s_i2c;
}

bool wg1200_i2c_lock(int timeout_ms)
{
    if (!s_i2c_lock) {
        return false;
    }
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_i2c_lock, ticks) == pdTRUE;
}

void wg1200_i2c_unlock(void)
{
    if (s_i2c_lock) {
        xSemaphoreGiveRecursive(s_i2c_lock);
    }
}

static esp_err_t pca_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pca_dev, buf, sizeof(buf), 50);
}

static esp_err_t pca_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pca_dev, &reg, 1, val, 1, 50);
}

esp_err_t wg1200_io_set_level(uint8_t pin, uint8_t level)
{
    if (pin >= 16 || !s_pca_dev) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t port = pin >> 3;
    uint8_t bit = 1u << (pin & 7);
    if (!wg1200_i2c_lock(200)) {
        return ESP_ERR_TIMEOUT;
    }
    if (level) {
        s_pca_out[port] |= bit;
    } else {
        s_pca_out[port] &= (uint8_t)~bit;
    }
    esp_err_t err = pca_write_reg(port ? PCA9535_REG_OUT1 : PCA9535_REG_OUT0, s_pca_out[port]);
    if (err == ESP_OK && (s_pca_cfg[port] & bit)) {
        s_pca_cfg[port] &= (uint8_t)~bit;
        err = pca_write_reg(port ? PCA9535_REG_CFG1 : PCA9535_REG_CFG0, s_pca_cfg[port]);
    }
    wg1200_i2c_unlock();
    return err;
}

int wg1200_io_get_level(uint8_t pin)
{
    if (pin >= 16 || !s_pca_dev) {
        return -1;
    }
    uint8_t port = pin >> 3;
    uint8_t bit = 1u << (pin & 7);
    uint8_t val = 0;
    if (!wg1200_i2c_lock(200)) {
        return -1;
    }
    esp_err_t err = pca_read_reg(port ? PCA9535_REG_IN1 : PCA9535_REG_IN0, &val);
    wg1200_i2c_unlock();
    if (err != ESP_OK) {
        return -1;
    }
    return (val & bit) ? 1 : 0;
}

esp_err_t wg1200_io_set_direction(uint8_t pin, bool is_output)
{
    if (pin >= 16 || !s_pca_dev) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t port = pin >> 3;
    uint8_t bit = 1u << (pin & 7);
    if (!wg1200_i2c_lock(200)) {
        return ESP_ERR_TIMEOUT;
    }
    if (is_output) {
        s_pca_cfg[port] &= (uint8_t)~bit;
    } else {
        s_pca_cfg[port] |= bit;
    }
    esp_err_t err = pca_write_reg(port ? PCA9535_REG_CFG1 : PCA9535_REG_CFG0, s_pca_cfg[port]);
    wg1200_i2c_unlock();
    return err;
}

void wg1200_buzzer_beep(uint32_t duration_ms)
{
    if (duration_ms == 0 || !s_pca_dev || !muse_settings_speaker_on()) {
        return;
    }
    /*
     * On WeatherXM-WG1200-v1.1 (Page 4 U25 PCA9535PW & Page 6 B1), the buzzer
     * (8.5x8.5x3.0mm passive SMD magnetic buzzer, Vop=2.5-4.5V) is driven by
     * NPN transistor Q7 (BC817W) from PCA9535 IO1_5 (Port 1, bit 5).
     * Because B1 is a passive magnetic transducer, we oscillate IO1_5 at ~2.5 kHz
     * over the 400 kHz I2C bus and leave IO1_5 LOW afterwards.
     */
    const uint8_t bit = (1u << (WG1200_EXP_BUZZER_EN & 7));
    if (!wg1200_i2c_lock(50)) {
        return;
    }
    if (s_pca_cfg[1] & bit) {
        s_pca_cfg[1] &= (uint8_t)~bit;
        pca_write_reg(PCA9535_REG_CFG1, s_pca_cfg[1]);
    }
    wg1200_i2c_unlock();

    /* Acquire and release s_i2c_lock on each pulse cycle (leaving IO1_5 LOW
     * before unlocking) so FT6336U touch, BMP390, and SX1262 NSS/BUSY never
     * starve on long notification beeps. */
    int64_t end_us = esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
    while (esp_timer_get_time() < end_us) {
        if (!wg1200_i2c_lock(50)) {
            break;
        }
        uint8_t hi = (uint8_t)(s_pca_out[1] | bit);
        uint8_t lo = (uint8_t)(s_pca_out[1] & (uint8_t)~bit);
        pca_write_reg(PCA9535_REG_OUT1, hi);
        esp_rom_delay_us(110);
        s_pca_out[1] = lo;
        pca_write_reg(PCA9535_REG_OUT1, lo);
        wg1200_i2c_unlock();
        esp_rom_delay_us(110);
    }
    (void)wg1200_io_set_level(WG1200_EXP_BUZZER_EN, 0);
}

/* --- ST7701S 9-bit SPI bit-bang helpers (used only during boot before SPI3 init) --- */

static void st7701_spi_write_9bit(uint8_t dc, uint8_t data)
{
    gpio_set_level(WG1200_SPI_SCK, 0);
    gpio_set_level(WG1200_SPI_MOSI, dc ? 1 : 0);
    esp_rom_delay_us(1);
    gpio_set_level(WG1200_SPI_SCK, 1);
    esp_rom_delay_us(1);

    for (int i = 7; i >= 0; i--) {
        gpio_set_level(WG1200_SPI_SCK, 0);
        gpio_set_level(WG1200_SPI_MOSI, (data >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(WG1200_SPI_SCK, 1);
        esp_rom_delay_us(1);
    }
    gpio_set_level(WG1200_SPI_SCK, 0);
}

static inline void st7701_cmd(uint8_t cmd)
{
    st7701_spi_write_9bit(0, cmd);
}

static inline void st7701_data(uint8_t data)
{
    st7701_spi_write_9bit(1, data);
}

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t delay_ms;
    uint8_t data[16];
} st7701_cmd_t;

/* Seeed SenseCAP Indicator / WeatherXM WG1200 ("GX" screen) ST7701S init table:
 * includes 0xC7=0x04 (SDIR normal left-to-right scan) and 0x21 (inversion on). */
static const st7701_cmd_t s_st7701_init[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0D, 0x02}},
    {0xC2, 2, 0, {0x31, 0x05}},
    {0xC7, 1, 0, {0x04}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}},
    {0xB1, 16, 0, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x60}},
    {0xB1, 1, 0, {0x32}},
    {0xB2, 1, 0, {0x07}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x49}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x21}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 20, {0x78}},
    {0xE0, 3, 0, {0x00, 0x1B, 0x02}},
    {0xE1, 11, 0, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}},
    {0xE2, 12, 0, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE4, 2, 0, {0x44, 0x44}},
    {0xE5, 16, 0, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE7, 2, 0, {0x44, 0x44}},
    {0xE8, 16, 0, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}},
    {0xEB, 7, 0, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}},
    {0xEC, 2, 0, {0x3C, 0x00}},
    {0xED, 16, 0, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}},
    {0x36, 1, 0, {0x10}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xE5, 1, 0, {0xE4}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x3A, 1, 0, {0x60}},
    {0x21, 0, 0, {0}},
    {0x11, 0, 120, {0}},
    {0x29, 0, 20, {0}},
};

static esp_err_t st7701_panel_init_sequence(void)
{
    /* Deselect SX1262 and SD card before driving shared GPIO41/48 */
    ESP_RETURN_ON_ERROR(wg1200_io_set_level(WG1200_EXP_LORA_NSS, 1), TAG, "lora nss high");
    ESP_RETURN_ON_ERROR(wg1200_io_set_level(WG1200_EXP_SD_CS, 1), TAG, "sd cs high");
    ESP_RETURN_ON_ERROR(wg1200_io_set_direction(WG1200_EXP_SD_DET, false), TAG, "sd det in");
    ESP_RETURN_ON_ERROR(wg1200_io_set_level(WG1200_EXP_LCD_CS, 1), TAG, "lcd cs high");

    gpio_config_t spi_pins = {
        .pin_bit_mask = (1ULL << WG1200_SPI_SCK) | (1ULL << WG1200_SPI_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&spi_pins), TAG, "st7701 spi gpio");
    gpio_set_level(WG1200_SPI_SCK, 0);
    gpio_set_level(WG1200_SPI_MOSI, 0);

    /* Hardware reset ST7701S */
    wg1200_io_set_level(WG1200_EXP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wg1200_io_set_level(WG1200_EXP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(15));
    wg1200_io_set_level(WG1200_EXP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Frame each 9-bit SPI command with LCD_CS low -> high */
    for (size_t i = 0; i < sizeof(s_st7701_init) / sizeof(s_st7701_init[0]); i++) {
        const st7701_cmd_t *c = &s_st7701_init[i];
        wg1200_io_set_level(WG1200_EXP_LCD_CS, 0);
        st7701_cmd(c->cmd);
        for (int k = 0; k < c->len; k++) {
            st7701_data(c->data[k]);
        }
        wg1200_io_set_level(WG1200_EXP_LCD_CS, 1);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    /* Release GPIO41/48 so SPI3_HOST can claim them for SX1262 */
    gpio_reset_pin(WG1200_SPI_SCK);
    gpio_reset_pin(WG1200_SPI_MOSI);

    ESP_LOGI(TAG, "ST7701S 9-bit SPI init complete; released GPIO41/48 for SPI3_HOST");
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "BL timer");

    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = WG1200_LCD_BL_GPIO,
        .duty = 800,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "BL channel");
    s_bl_ready = true;
    return ESP_OK;
}

static void set_brightness(int pct)
{
    if (!s_bl_ready) {
        return;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)(pct * 1023 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t init(void)
{
    wxm_boot_guard_init();

    s_i2c_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_i2c_lock != NULL, ESP_ERR_NO_MEM, TAG, "i2c mutex");

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = WG1200_I2C_SDA,
        .scl_io_num = WG1200_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG, "I2C0 bus");

    uint8_t pca_addr = 0x20;
    if (i2c_master_probe(s_i2c, pca_addr, 50) != ESP_OK) {
        pca_addr = 0x39;
        ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, pca_addr, 50), TAG, "PCA9535 probe");
    }
    ESP_LOGI(TAG, "PCA9535 detected at 0x%02X", pca_addr);

    const i2c_device_config_t pca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = pca_addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &pca_cfg, &s_pca_dev), TAG, "PCA9535 add");

    ESP_RETURN_ON_ERROR(pca_write_reg(PCA9535_REG_OUT0, s_pca_out[0]), TAG, "PCA9535 out0");
    ESP_RETURN_ON_ERROR(pca_write_reg(PCA9535_REG_OUT1, s_pca_out[1]), TAG, "PCA9535 out1");
    ESP_RETURN_ON_ERROR(pca_write_reg(PCA9535_REG_CFG0, s_pca_cfg[0]), TAG, "PCA9535 cfg0");
    ESP_RETURN_ON_ERROR(pca_write_reg(PCA9535_REG_CFG1, s_pca_cfg[1]), TAG, "PCA9535 cfg1");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_btn_top, WG1200_BTN_TOP_GPIO), TAG, "top button GPIO38");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    return ESP_OK;
}

static esp_err_t panel_start(void)
{
    /* Note on GPIO10: RGB DATA5 is wired to GPIO10. Before starting the RGB
     * peripheral, release the GPIO10 hold that wxm_boot_guard_init() placed;
     * wxm_prepare_safe_restart_gpios() re-latches GPIO10 HIGH before any reset. */
    gpio_hold_dis(GPIO_NUM_10);

    const esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = 12 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_back_porch = 50,
            .hsync_front_porch = 10,
            .hsync_pulse_width = 8,
            .vsync_back_porch = 20,
            .vsync_front_porch = 10,
            .vsync_pulse_width = 8,
            .flags.pclk_active_neg = 0,
        },
        .data_width = 16,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
#else
        .bits_per_pixel = 16,
#endif
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_W * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = GPIO_NUM_16,
        .vsync_gpio_num = GPIO_NUM_17,
        .de_gpio_num = GPIO_NUM_18,
        .pclk_gpio_num = GPIO_NUM_21,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            15, 14, 13, 12, 11, 10, 9, 8,
            7, 6, 5, 4, 3, 2, 1, 0,
        },
        .flags.fb_in_psram = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel), TAG, "RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "RGB reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "RGB init");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "frame buffer");
    if (fb) {
        memset(fb, 0, LCD_W * LCD_H * 2);
    }

    /* Seeed initializes the ST7701S controller after the RGB pixel clock is running */
    ESP_RETURN_ON_ERROR(st7701_panel_init_sequence(), TAG, "st7701 init");
    return ESP_OK;
}

static esp_err_t touch_start(void)
{
    /* Pulse TP_RST (IO0_7) low then high */
    wg1200_io_set_level(WG1200_EXP_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(15));
    wg1200_io_set_level(WG1200_EXP_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    wg1200_i2c_lock(-1);
    /* WG1200 GX screen uses 0x48; DX screen uses 0x38 */
    if (i2c_master_probe(s_i2c, 0x48, 50) == ESP_OK) {
        s_tp_addr = 0x48;
    } else if (i2c_master_probe(s_i2c, 0x38, 50) == ESP_OK) {
        s_tp_addr = 0x38;
    } else {
        ESP_LOGW(TAG, "FT6336U touch not detected at 0x48 or 0x38; defaulting to 0x48");
        s_tp_addr = 0x48;
    }
    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_tp_addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp_dev);
    wg1200_i2c_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "FT6336U add");
    ESP_LOGI(TAG, "FT6336U touch controller ready at 0x%02X", s_tp_addr);
    return ESP_OK;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
    lv_display_flush_ready(disp);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint16_t s_last_x = 0;
    static uint16_t s_last_y = 0;

    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_tp_dev || !wg1200_i2c_lock(15)) {
        return;
    }
    uint8_t reg = 0x02;
    uint8_t buf[5] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_tp_dev, &reg, 1, buf, sizeof(buf), 20);
    wg1200_i2c_unlock();
    if (err != ESP_OK) {
        return;
    }
    uint8_t points = buf[0] & 0x0F;
    if (points == 0 || points > 2) {
        s_touch_was_pressed = false;
        return;
    }
    uint16_t raw_x = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
    uint16_t raw_y = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];

    /* On WG1200 / SenseCAP Indicator (both GX 0x48 and DX 0x38 panels), the FT6336U
     * touch digitizer origin is rotated 180 degrees relative to the ST7701S LCD
     * (matches touchpad_read() in WG1200 main/lv_port.c). */
    uint16_t x = (raw_x < LCD_W) ? (LCD_W - 1 - raw_x) : 0;
    uint16_t y = (raw_y < LCD_H) ? (LCD_H - 1 - raw_y) : 0;

    s_last_x = x;
    s_last_y = y;
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;

    if (!s_touch_was_pressed && muse_link_state() == MUSE_LINK_CONFIRM) {
        if (muse_link_talk_press()) {
            ESP_LOGI(TAG, "pairing confirmed by screen tap");
        }
    }
    s_touch_was_pressed = true;
}

static esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(panel_start(), TAG, "panel");
    ESP_RETURN_ON_ERROR(touch_start(), TAG, "touch");

    /* Allocate off-screen composition buffer so LVGL renders background + avatar + text
     * completely before esp_lcd_panel_draw_bitmap copies finished pixels to the PSRAM FB.
     * This prevents RGB DMA from scanning out intermediate black-cleared regions. */
    const size_t draw_buf_sz = LCD_W * 80 * sizeof(uint16_t);
    void *draw_buf = heap_caps_malloc(draw_buf_sz, MUSE_BIG_CAPS);
    ESP_RETURN_ON_FALSE(draw_buf, ESP_ERR_NO_MEM, TAG, "no draw buffer");

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(LCD_W, LCD_H);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "LVGL display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, draw_buf, NULL, draw_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush);

    s_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_indev, ESP_ERR_NO_MEM, TAG, "LVGL touch");
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read);
    lv_indev_set_display(s_indev, s_disp);
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_start_err = display_init();
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
        xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY,
                                        NULL, MUSE_UI_CORE, MUSE_BIG_CAPS) != pdPASS) {
        return NULL;
    }
    xSemaphoreTake(s_started, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        return NULL;
    }
    wg1200_buzzer_beep(40);
    *touch = s_indev;
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(
               s_lv_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_btn_top);

    if (ev & MUSE_BTN_TALK_PRESS) {
        if (muse_link_state() == MUSE_LINK_CONFIRM && muse_link_talk_press()) {
            ESP_LOGI(TAG, "pairing confirmed by top button");
            return 0;
        }
        return MUSE_BTN_AUX_PRESS;
    }
    if (ev & MUSE_BTN_TALK_RELEASE) {
        return MUSE_BTN_AUX_RELEASE;
    }
    return 0;
}

static const muse_board_t s_board = {
    .name = "WeatherXM WG1200",
    .width = LCD_W,
    .height = LCD_H,
    .avatar_px = 288,
    .round = false,
    .no_ring = false,
    .touch = true,
    .diagonal_in = 4.0f,
    .talk_button = "screen",
    .aux_button = "top",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .poll_buttons = poll_buttons,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
