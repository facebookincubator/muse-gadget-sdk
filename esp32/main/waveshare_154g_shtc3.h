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

// The Waveshare ESP32-S3-ePaper-1.54G's onboard air sensor: a Sensirion
// SHTC3 temperature and humidity sensor on I2C. A background task polls it
// every 10 seconds and keeps the latest of each reading; sensors.read
// answers with those.

#pragma once

#include "cJSON.h"

// Join the board's I2C bus (creating it if needed, and keeping it for the
// codec and RTC) and start the polling task, which keeps retrying while the
// sensor doesn't answer.
void waveshare_154g_shtc3_init(void);

// sensors.read: the latest temperature and humidity readings, with their
// age. A sensor with no recent reading is null; with none at all, an error.
cJSON *waveshare_154g_shtc3_command(void);
