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

/* Host fakes of the ESP-IDF pieces mg_smp_esp.c uses (tests/mg_smp_esp_server.c implements them). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_bootloader_desc.h"
#include "esp_err.h"
#include "esp_partition.h"
typedef uint32_t esp_ota_handle_t;
typedef enum {
    ESP_OTA_IMG_NEW = 0x0,
    ESP_OTA_IMG_PENDING_VERIFY = 0x1,
    ESP_OTA_IMG_VALID = 0x2,
    ESP_OTA_IMG_INVALID = 0x3,
    ESP_OTA_IMG_ABORTED = 0x4,
    ESP_OTA_IMG_UNDEFINED = -1,
} esp_ota_img_states_t;
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);
const esp_partition_t *esp_ota_get_boot_partition(void);
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t image_size, esp_ota_handle_t *out);
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback(void);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *desc);
esp_err_t esp_ota_get_bootloader_description(const esp_partition_t *bootloader, esp_bootloader_desc_t *desc);
