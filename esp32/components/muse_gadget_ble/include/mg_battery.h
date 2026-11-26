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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* report_battery_status (device_action 9, mgcommands.h): what the board's PMIC or gauge measures. Unknown values are 0, and
 * flags (mg_battery_status_flag_t) say which are real. */
typedef struct {
    uint16_t voltage_mv;
    int16_t current_100ua;     /* negative discharging */
    uint8_t soc_percent;
    int16_t temperature_c;
    uint16_t capacity_mah;     /* remaining */
    uint16_t full_capacity_mah;
    uint32_t timestamp_ms;     /* device ms of the reading */
    uint8_t flags;
} mg_battery_status_t;

#define MG_BATTERY_REPORT_LEN 18
/* [device_action, report_battery_status, the fields little-endian]. */
size_t mg_battery_encode(const mg_battery_status_t *s, uint8_t out[MG_BATTERY_REPORT_LEN]);

#ifdef __cplusplus
}
#endif
