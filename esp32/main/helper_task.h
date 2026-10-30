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

/*
 * Short-lived setup tasks (pairing timers, Wi-Fi scan refresh, provisioning)
 * whose stacks go to PSRAM where that is safe, sparing internal RAM.
 *
 * Only where code and read-only data run from PSRAM: flash and NVS writes then
 * leave the cache on. Elsewhere a flash write from a task with a PSRAM stack
 * asserts in esp_task_stack_is_sane_cache_disabled(), so the stack stays
 * internal.
 *
 * A task started with helper_task_create() must end with helper_task_delete(),
 * whether it deletes itself (NULL) or another task cancels it: a plain
 * vTaskDelete() frees neither the stack nor the TCB of a task created WithCaps.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_SPIRAM && CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#define HELPER_TASK_STACK_IN_PSRAM 1
#else
#define HELPER_TASK_STACK_IN_PSRAM 0
#endif

static inline BaseType_t helper_task_create(TaskFunction_t fn, const char *name,
                                            uint32_t stack_bytes, void *arg,
                                            UBaseType_t priority, TaskHandle_t *out)
{
#if HELPER_TASK_STACK_IN_PSRAM
    return xTaskCreateWithCaps(fn, name, stack_bytes, arg, priority, out,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(fn, name, stack_bytes, arg, priority, out);
#endif
}

static inline void helper_task_delete(TaskHandle_t task)
{
#if HELPER_TASK_STACK_IN_PSRAM
    vTaskDeleteWithCaps(task);
#else
    vTaskDelete(task);
#endif
}
