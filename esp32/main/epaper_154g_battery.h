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

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The Waveshare 1.54G's cell, read through the divider on GPIO4. The board
// gives the chip no charge-status or VBUS signal, so there is no charging
// flag here; the USB serial port can only say that a host is talking to it.
typedef struct {
    int millivolts;
    int percent;      // an estimate from the voltage, 0 to 100
    bool usb_host;    // a USB host's frames (SOF) seen on the chip's own serial port in the last few ms
} epaper_154g_battery_t;

// Sets up the ADC and starts a task that logs the voltage once a minute.
void epaper_154g_battery_init(void);
// A fresh measurement. ESP_ERR_INVALID_STATE before init or without ADC
// calibration.
esp_err_t epaper_154g_battery_read(epaper_154g_battery_t *out);
// Last successful estimate, or -1; no new ADC measurement.
int epaper_154g_battery_cached_percent(void);

#ifdef __cplusplus
}
#endif
