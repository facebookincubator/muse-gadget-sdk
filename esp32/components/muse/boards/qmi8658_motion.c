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

/* Register programming checked against Waveshare's 1.75C SensorLib examples:
 * https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C
 * 0x6b, WHO_AM_I=5, little-endian auto increment, +/-2 g, low-power 21 Hz.
 * Only the input task owns this device. Reuse the BSP's shared bus. */
#include "qmi8658_motion.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2c_master_dev_handle_t s_dev;

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, n, 20);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[] = { reg, value };
    return i2c_master_transmit(s_dev, data, sizeof(data), 20);
}

esp_err_t muse_qmi8658_enable(i2c_master_bus_handle_t bus, bool enable)
{
    if (!s_dev) {
        if (!enable) return ESP_OK;
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x6b, .scl_speed_hz = 400000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) return err;
    }
    if (!enable) return write_reg(0x08, 0);   /* gyro and accelerometer off */
    uint8_t id;
    esp_err_t err = read_regs(0, &id, 1);
    if (err != ESP_OK) return err;
    if (id != 5) return ESP_ERR_NOT_FOUND;
    /* Reset also clears a previous firmware's FIFO/interrupt configuration. */
    if ((err = write_reg(0x60, 0xb0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t result;
    if ((err = read_regs(0x4d, &result, 1)) != ESP_OK) return err;
    if (result != 0x80) return ESP_ERR_INVALID_RESPONSE;
    if ((err = write_reg(0x02, 0x40)) != ESP_OK) return err;
    if ((err = write_reg(0x03, 0x0d)) != ESP_OK) return err;
    /* Disable data-ready interrupt routing: this version polls status. */
    return write_reg(0x08, 0x21);
}

esp_err_t muse_qmi8658_read(muse_motion_sample_t *out)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t status, data[6];
    esp_err_t err = read_regs(0x2e, &status, 1);
    if (err != ESP_OK) return err;
    if (!(status & 1)) return ESP_ERR_NOT_FINISHED;
    if ((err = read_regs(0x35, data, sizeof(data))) != ESP_OK) return err;
    int16_t *axes[] = { &out->x, &out->y, &out->z };
    for (int i = 0; i < 3; i++) {
        int16_t raw = (int16_t)((uint16_t)data[2*i] | (uint16_t)data[2*i+1] << 8);
        *axes[i] = (int32_t)raw * 1000 / 16384;
    }
    return ESP_OK;
}
