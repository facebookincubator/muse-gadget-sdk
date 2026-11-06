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

// The reTerminal E100x's onboard air sensor: a Sensirion SHT4x temperature
// and humidity sensor on I2C. A background task polls it every 10 seconds
// and keeps the latest of each reading; sensors.read answers with those.

#pragma once

#include "cJSON.h"

// Start the I2C bus and the polling task. Logs and does nothing else when
// the sensor is missing.
void reterminal_sht4x_init(void);

// sensors.read: the latest temperature and humidity readings, with their
// age. A sensor with no recent reading is null; with none at all, an error.
cJSON *reterminal_sht4x_command(void);
