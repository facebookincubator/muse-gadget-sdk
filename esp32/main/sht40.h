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

// Sensirion SHT40 temperature/humidity sensor over I2C. The reTerminal
// E1002 carries one at I2C address 0x44 (SDA GPIO19, SCL GPIO20).

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the sensor. The first call creates the I2C bus and probes for the
// sensor; when it is absent, this (and later calls) return false without
// retrying. Successful readings are cached for a few seconds so frequent
// device.health queries don't hammer the bus. Safe to call from the
// control task: a fresh measurement takes about 10 ms.
bool sht40_read(float *temp_c, float *humidity_pct);

#ifdef __cplusplus
}
#endif
