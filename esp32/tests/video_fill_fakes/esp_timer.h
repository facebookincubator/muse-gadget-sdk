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
/* Host fake for tests/video_fill_harness.c. */
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_timer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *arg);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

static inline int64_t esp_timer_get_time(void)
{
    static int64_t t = 1000000;
    return t += 10;
}
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    (void)a;
    *h = (esp_timer_handle_t)0;
    return ESP_FAIL;
}
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us) { (void)h; (void)us; return ESP_FAIL; }
