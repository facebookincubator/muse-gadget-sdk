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
 * WeatherXM WG1200 Indoor (BMP390) + Outdoor (SX1262 LoRa WS1001/WS1300)
 * Weather Station subsystem and gadget tool handlers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WG1200_REGION_EU868 = 0,
    WG1200_REGION_US915 = 1,
    WG1200_REGION_AS923 = 2,
    WG1200_REGION_COUNT = 3,
} wg1200_weather_region_t;

typedef struct {
    /* Indoor BMP390 barometer (I2C0 0x77, powered via PCA9535 IO1_2) */
    bool bmp390_detected;
    bool bmp390_valid;
    uint8_t bmp390_chip_id;
    float indoor_temp_c;
    float indoor_pressure_hpa;
    int64_t indoor_updated_us;

    /* Semtech SX1262 sub-GHz LoRa radio (SPI3 + PCA9535 IO0_0..IO0_3 + GPIO42) */
    bool sx1262_detected;
    uint16_t sx1262_sync_word;
    wg1200_weather_region_t region;
    uint32_t freq_hz;
    uint32_t rx_packets_total;
    uint32_t rx_packets_valid;

    /* Outdoor WeatherXM station telemetry (WS1001 / WS1300) */
    bool outdoor_valid;
    char station_model[12];
    uint32_t station_id;
    uint32_t packet_count;
    uint8_t fw_version;
    float outdoor_temp_c;
    float outdoor_humidity_pct;
    float wind_speed_ms;
    float wind_gust_ms;
    uint16_t wind_dir_deg;
    float rain_rate_mm_h;
    float rain_daily_mm;
    float rain_accum_mm;
    float uv_index;
    uint32_t illuminance_lux;
    float solar_rad_wm2;
    uint16_t battery_mv;
    bool battery_low;
    int16_t rssi_dbm;
    int8_t snr_db;
    int64_t outdoor_updated_us;

    /* The paired station's last valid GPS fix (WS1001 WEATHER_GPS_SIGNED
     * packets), kept in muse_nvs so it survives restarts. */
    bool gps_valid;
    double gps_lat;
    double gps_lon;
} wg1200_weather_state_t;

/**
 * Power on and initialize the indoor BMP390 barometer and the SX1262 LoRa
 * outdoor weather station receiver task. Also inspects the factory NVS
 * partition in read-only mode to inherit the user's configured WeatherXM
 * frequency region (EU868 / US915 / AS923) if not overridden in muse_nvs.
 */
esp_err_t wg1200_weather_init(void);

/**
 * Copy a thread-safe snapshot of the latest indoor + outdoor weather state.
 */
void wg1200_weather_get_state(wg1200_weather_state_t *out);

/**
 * Change the active SX1262 LoRa listening region (persisted in muse_nvs).
 */
esp_err_t wg1200_weather_set_region(wg1200_weather_region_t region);

/**
 * Return human-readable region label ("EU 868 MHz", "US 915 MHz", "AS 923 MHz").
 */
const char *wg1200_weather_region_name(wg1200_weather_region_t region);

/**
 * Handle the `sensors.read` gadget command for WG1200.
 */
cJSON *wg1200_weather_sensors_command(void);

/**
 * Handle the `device.boot_weatherxm` gadget command for WG1200.
 */
cJSON *wg1200_boot_weatherxm_command(const cJSON *params);

#ifdef __cplusplus
}
#endif
