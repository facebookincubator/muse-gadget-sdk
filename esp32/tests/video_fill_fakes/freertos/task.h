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

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void taskYIELD(void) {}
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t st, void *a, UBaseType_t p,
                                                 TaskHandle_t *h, BaseType_t core)
{
    (void)f; (void)n; (void)st; (void)a; (void)p; (void)core;
    if (h) {
        *h = 0;
    }
    return pdFALSE;
}
static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t f, const char *n, uint32_t st, void *a,
                                                         UBaseType_t p, TaskHandle_t *h, BaseType_t core, unsigned caps)
{
    (void)caps;
    return xTaskCreatePinnedToCore(f, n, st, a, p, h, core);
}
static inline BaseType_t xTaskNotifyGive(TaskHandle_t h) { (void)h; return pdPASS; }
static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t t) { (void)clear; (void)t; return 1; }
static inline BaseType_t xTaskGetCoreID(TaskHandle_t h) { (void)h; return 0; }
static inline TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core) { (void)core; return 0; }
