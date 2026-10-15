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

#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

// The harness keeps the keys, with a file for the flash: "flash <file>" before the boot.
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
