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

// The reTerminal E100x's battery gauge: the cell voltage reaches GPIO1
// (ADC1 channel 0) through a 2:1 divider that GPIO21 switches in. There is no
// dedicated fuel-gauge reading here, so only calibrated voltage and an
// estimated percentage derived from it are reported.
// device.health leaves charging and usb_power null on these boards, as
// its contract allows for values a board can't measure.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Switch the divider in and set up the ADC before serving health requests.
// Logs and leaves reads failing when ADC setup or calibration fails.
void reterminal_battery_init(void);

// Sample battery voltage in millivolts and estimate charge in percent (0-100).
// Voltage depends on load and charging; this is not a fuel-gauge measurement.
// Returns false when no reading is available, including when the voltage
// is far below an empty cell. Outputs are unchanged on failure.
bool reterminal_battery_read(int *battery_mv, int *battery_pct);

#ifdef __cplusplus
}
#endif
