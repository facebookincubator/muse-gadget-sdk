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
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    uint32_t address, size;
    char label[17];
    int type, subtype;
} esp_partition_t;
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size);
