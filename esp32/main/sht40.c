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

#include "sht40.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SHT40_ADDR 0x44
#define SHT40_CMD_MEASURE_HIGH_PREC 0xFD
#define SHT40_MEASURE_DELAY_MS 10
#define SHT40_CRC_POLY 0x31
#define SHT40_CRC_INIT 0xFF

// Serve device.health from cache; a high-precision measurement takes ~10 ms.
#define SHT40_CACHE_TTL_US (5LL * 1000 * 1000)

static const char *TAG = "sht40";

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_probed = false;
static bool s_present = false;
static float s_temp_c = 0.0f;
static float s_humidity_pct = 0.0f;
static int64_t s_cached_at_us = 0;

static uint8_t sht40_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = SHT40_CRC_INIT;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ SHT40_CRC_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static bool sht40_init(void) {
    if (s_probed) {
        return s_present;
    }
    s_probed = true;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_HOMEHUB_SHT40_SDA_GPIO,
        .scl_io_num = CONFIG_HOMEHUB_SHT40_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        return false;
    }
    if (i2c_master_probe(bus, SHT40_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "no SHT40 at 0x%02x", SHT40_ADDR);
        i2c_del_master_bus(bus);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT40_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "SHT40 device add failed");
        i2c_del_master_bus(bus);
        return false;
    }
    s_present = true;
    ESP_LOGI(TAG, "SHT40 found at 0x%02x", SHT40_ADDR);
    return true;
}

static bool sht40_measure(float *temp_c, float *humidity_pct) {
    uint8_t cmd = SHT40_CMD_MEASURE_HIGH_PREC;
    uint8_t buf[6];
    if (i2c_master_transmit(s_dev, &cmd, 1, 100) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(SHT40_MEASURE_DELAY_MS));
    if (i2c_master_receive(s_dev, buf, sizeof(buf), 100) != ESP_OK) {
        return false;
    }
    if (sht40_crc8(buf, 2) != buf[2] || sht40_crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "SHT40 CRC mismatch");
        return false;
    }
    uint16_t raw_t = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t raw_rh = (uint16_t)((buf[3] << 8) | buf[4]);
    *temp_c = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    *humidity_pct = -6.0f + 125.0f * (float)raw_rh / 65535.0f;
    if (*humidity_pct < 0.0f) {
        *humidity_pct = 0.0f;
    }
    if (*humidity_pct > 100.0f) {
        *humidity_pct = 100.0f;
    }
    return true;
}

bool sht40_read(float *temp_c, float *humidity_pct) {
    if (!sht40_init()) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (s_cached_at_us != 0 && now - s_cached_at_us < SHT40_CACHE_TTL_US) {
        *temp_c = s_temp_c;
        *humidity_pct = s_humidity_pct;
        return true;
    }
    float t = 0.0f;
    float rh = 0.0f;
    if (!sht40_measure(&t, &rh)) {
        return false;
    }
    s_temp_c = t;
    s_humidity_pct = rh;
    s_cached_at_us = now;
    *temp_c = t;
    *humidity_pct = rh;
    return true;
}
