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

#include "esp_err.h"
#include "esp_log.h"

/* As IDF's: log and return the error, or return err_code if a is false. */
#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...) do {        \
        esp_err_t err_rc_ = (x);                                 \
        if (err_rc_ != ESP_OK) {                                 \
            ESP_LOGE(log_tag, format, ##__VA_ARGS__);            \
            return err_rc_;                                      \
        }                                                        \
    } while (0)

#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, format, ...) do { \
        if (!(a)) {                                                 \
            ESP_LOGE(log_tag, format, ##__VA_ARGS__);               \
            return err_code;                                        \
        }                                                           \
    } while (0)
