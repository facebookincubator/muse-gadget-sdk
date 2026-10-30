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

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

typedef esp_err_t (*esp_pm_light_sleep_cb_t)(int64_t sleep_time_us, void *arg);

typedef struct {
    esp_pm_light_sleep_cb_t enter_cb;
    esp_pm_light_sleep_cb_t exit_cb;
    void *enter_cb_user_arg;
    void *exit_cb_user_arg;
    uint32_t enter_cb_prior;
    uint32_t exit_cb_prior;
} esp_pm_sleep_cbs_register_config_t;

// The harness keeps the callback and writes the dump.
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs_conf);
esp_err_t esp_pm_dump_locks(FILE *stream);
