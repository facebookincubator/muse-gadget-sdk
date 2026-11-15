/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
/* Idempotent I2C-only recovery, before panel/audio/tasks. Charge writes preserve
 * PWR_CFG's other bits and read the register back. Also used by normal boot. */
esp_err_t muse_triviapod_charge(bool enabled);
/* Diagnostic only: IO9/PWM1, 2000 Hz, 0x8999 on / zero off, with readback. */
esp_err_t muse_triviapod_motor(bool enabled);
void muse_triviapod_pause_static(bool pause);
