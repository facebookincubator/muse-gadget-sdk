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

// Everything is on one I2C bus, SDA GPIO0 / SCL GPIO1, shared with the Grove
// port (https://docs.m5stack.com/en/core/CoreMatrix):
//   - M5PM1 power chip at 0x6e. G0-G2 are BtnA-C, active low
//     (M5Unified 96576a1, M5Unified.inl and utility/power/M5PM1_Class.inl).
//   - M5IOE1 expander at 0x4f. IO4 (bit 3) powers the matrix
//     (M5GFX 28d47949, board_detect/m5/pmic_ops.hpp). Its other pins set the
//     charge current and power the buzzer, SD card and Grove port, so only
//     that bit is changed.
//   - TM1680 LED driver at 0x72 (tm1680.c).

#include "m5stack_corematrix.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tm1680.h"

static const char *TAG = "corematrix";

#define I2C_PIN_SDA     0
#define I2C_PIN_SCL     1
#define TIMEOUT_MS      50
#define ADDR_PM1        0x6e
#define ADDR_IOE1       0x4f
#define ADDR_TM1680     0x72
#define IOE1_LEDS_EN    0x08    // IO4

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pm1;

static esp_err_t read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len) {
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
        err = i2c_master_transmit_receive(dev, &reg, 1, out, len, TIMEOUT_MS);
        if (err != ESP_OK) vTaskDelay(1);
    }
    return err;
}

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(dev, bytes, sizeof(bytes), TIMEOUT_MS);
}

// Changes only the given bits; writes nothing if the read fails.
static esp_err_t update_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t clear, uint8_t set) {
    uint8_t value;
    ESP_RETURN_ON_ERROR(read_reg(dev, reg, &value, 1), TAG, "read 0x%02x", reg);
    return write_reg(dev, reg, (uint8_t)((value & ~clear) | set));
}

static esp_err_t add_device(uint16_t address, i2c_master_dev_handle_t *dev) {
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, dev);
}

esp_err_t corematrix_init(void) {
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_PIN_SDA,
        .scl_io_num = I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus");
    i2c_master_dev_handle_t pm1;
    ESP_RETURN_ON_ERROR(add_device(ADDR_PM1, &pm1), TAG, "M5PM1");

    // Wake it (read_reg retries a refused first transfer; as in M5Unified the
    // id isn't checked), keep it awake and its watchdog off, and make G0-G2
    // plain inputs.
    uint8_t id[2];
    ESP_RETURN_ON_ERROR(read_reg(pm1, 0x00, id, 2), TAG, "M5PM1");
    ESP_RETURN_ON_ERROR(write_reg(pm1, 0x09, 0x00), TAG, "M5PM1");         // I2C idle sleep off
    ESP_RETURN_ON_ERROR(write_reg(pm1, 0x0a, 0x00), TAG, "M5PM1");         // watchdog off
    ESP_RETURN_ON_ERROR(update_reg(pm1, 0x16, 0x3f, 0), TAG, "M5PM1");     // G0-G2: GPIO
    ESP_RETURN_ON_ERROR(update_reg(pm1, 0x10, 0x07, 0), TAG, "M5PM1");     // G0-G2: input
    s_pm1 = pm1;
    return ESP_OK;
}

bool corematrix_button_read(corematrix_button_t button, bool *pressed) {
    uint8_t levels;
    if (!s_pm1 || (unsigned)button > COREMATRIX_BTN_C) return false;
    if (read_reg(s_pm1, 0x12, &levels, 1) != ESP_OK) return false;   // G0-G4 levels
    *pressed = !(levels & (1u << button));
    return true;
}

esp_err_t corematrix_new_matrix(esp_lcd_panel_handle_t *panel) {
    ESP_RETURN_ON_FALSE(s_bus && panel, ESP_ERR_INVALID_STATE, TAG, "corematrix_init() first");
    // IOE1: keep it awake and drive IO4 high, push-pull.
    i2c_master_dev_handle_t ioe1;
    uint8_t uid[2];
    ESP_RETURN_ON_ERROR(add_device(ADDR_IOE1, &ioe1), TAG, "M5IOE1");
    esp_err_t err = read_reg(ioe1, 0x00, uid, 2);
    if (err == ESP_OK) err = write_reg(ioe1, 0x23, 0x00);                        // idle sleep off
    if (err == ESP_OK) err = update_reg(ioe1, 0x13, IOE1_LEDS_EN, 0);            // push-pull
    if (err == ESP_OK) err = update_reg(ioe1, 0x03, 0, IOE1_LEDS_EN);            // output
    if (err == ESP_OK) err = update_reg(ioe1, 0x05, 0, IOE1_LEDS_EN);            // high
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        err = tm1680_new_panel(s_bus, ADDR_TM1680, panel);
        if (err == ESP_OK) {
            err = esp_lcd_panel_init(*panel);
            if (err != ESP_OK) esp_lcd_panel_del(*panel);
        }
        if (err != ESP_OK) update_reg(ioe1, 0x05, IOE1_LEDS_EN, 0);   // power back off
    }
    i2c_master_bus_rm_device(ioe1);
    if (err != ESP_OK) ESP_LOGE(TAG, "matrix: %s", esp_err_to_name(err));
    return err;
}
