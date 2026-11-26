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
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint8_t hash_appended; } esp_image_header_t;
typedef struct {
    esp_image_header_t image;
    uint32_t image_len;
    uint8_t image_digest[32];
} esp_image_metadata_t;
esp_err_t esp_image_get_metadata(const esp_partition_pos_t *part, esp_image_metadata_t *metadata);
