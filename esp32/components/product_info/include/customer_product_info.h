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

#define CUSTOMER_PRODUCT_INFO_SN_LEN 14u
#define CUSTOMER_PRODUCT_INFO_TEXT_LEN 10u

typedef struct {
    char sn[CUSTOMER_PRODUCT_INFO_SN_LEN + 1u];
} customer_product_info_t;

esp_err_t customer_product_info_get_sn(char *sn, size_t sn_len);
esp_err_t customer_product_info_get_region_text(char *region, size_t region_len);
esp_err_t customer_product_info_get_colour_text(char *colour, size_t colour_len);
esp_err_t customer_product_info_read(customer_product_info_t *info);
