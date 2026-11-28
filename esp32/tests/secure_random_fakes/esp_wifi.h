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

/* Host fake of the one esp_wifi function secure_random.c calls (builds that link
 * Wi-Fi); the test defines it: ESP_OK only while Wi-Fi is "started". */
#pragma once

#include <stdint.h>

#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#endif
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002

esp_err_t esp_wifi_get_max_tx_power(int8_t *power);
