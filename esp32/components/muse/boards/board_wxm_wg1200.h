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
 * WeatherXM WG1200 (Seeed SenseCAP Indicator D1L base) board interface.
 *
 * Exposes the shared I2C0 bus mutex and PCA9535 16-bit I/O expander helpers
 * so that the indoor BMP390 barometer and outdoor SX1262 LoRa radio driver
 * (wg1200_weather.c) can safely share I2C0 and PCA9535 with the display,
 * touch controller, and buzzer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PCA9535 16-bit I/O expander pin numbers (0..7 = Port 0, 8..15 = Port 1) */
#define WG1200_EXP_LORA_NSS     0   /* IO0_0: SX1262 SPI CS (active low) */
#define WG1200_EXP_LORA_RST     1   /* IO0_1: SX1262 reset (active low) */
#define WG1200_EXP_LORA_BUSY    2   /* IO0_2: SX1262 busy input (active high) */
#define WG1200_EXP_LORA_DIO1    3   /* IO0_3: SX1262 DIO1 input (also wired to GPIO42) */
#define WG1200_EXP_LCD_CS       4   /* IO0_4: ST7701S 9-bit SPI CS (active low) */
#define WG1200_EXP_LCD_RST      5   /* IO0_5: ST7701S reset (active low) */
#define WG1200_EXP_TP_INT       6   /* IO0_6: FT6336U touch interrupt input */
#define WG1200_EXP_TP_RST       7   /* IO0_7: FT6336U touch reset (active low) */
#define WG1200_EXP_RP2040_RST   8   /* IO1_0: RP_RST (unconnected on WG1200 v1.1) */
#define WG1200_EXP_ECC_PWR      9   /* IO1_1: ECC_PWR_SW (DNP on WG1200 v1.1) */
#define WG1200_EXP_BMP_PWR      10  /* IO1_2: BMP_PWR_SW (1 = ON, powers BMP390 & I2C FETs) */
#define WG1200_EXP_SD_CS        11  /* IO1_3: ESP_SD_CS (output, keep HIGH when SD unused) */
#define WG1200_EXP_SD_DET       12  /* IO1_4: ESP_SD_DET (input, active-low microSD detect) */
#define WG1200_EXP_BUZZER_EN    13  /* IO1_5: BUZZER (drives Q7 BC817W -> B1 8.5x8.5mm buzzer) */

/* Shared I2C0 master bus (GPIO39 SDA, GPIO40 SCL) and recursive lock */
i2c_master_bus_handle_t wg1200_i2c_bus(void);
bool wg1200_i2c_lock(int timeout_ms);
void wg1200_i2c_unlock(void);

/* Thread-safe PCA9535 pin operations (0..15) */
esp_err_t wg1200_io_set_level(uint8_t pin, uint8_t level);
int wg1200_io_get_level(uint8_t pin);
esp_err_t wg1200_io_set_direction(uint8_t pin, bool is_output);

/* Short non-blocking or blocking buzzer beep via PCA9535 IO1_5 */
void wg1200_buzzer_beep(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
