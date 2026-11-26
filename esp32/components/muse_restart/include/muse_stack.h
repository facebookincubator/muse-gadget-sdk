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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * App tasks whose stacks may live in PSRAM (CONFIG_MUSE_STACKS_IN_PSRAM, its
 * Kconfig help says when that's safe). Flash writes are fine there; flash
 * mapping is not: never for a task that can reach esp_partition_mmap,
 * spi_flash_mmap, bootloader_mmap or what calls them (esp_ota_get_state_partition,
 * esp_ota_get_boot_partition, esp_ota_mark_app_*, esp_ota_set_boot_partition,
 * esp_ota_end, esp_image_verify, esp_https_ota): mapping freezes the cache and
 * a PSRAM stack asserts. Nor for a task that powers the chip off, sleeps it
 * or turns the cache off itself, nor one that hands its stack to a DMA
 * engine. With the option off, both calls below are
 * xTaskCreatePinnedToCore() and the task ends with vTaskDelete(NULL), as
 * before.
 *
 * muse_task_create(): a task that never ends. With the option on it's a
 * WithCaps task (stack in PSRAM, or internal if PSRAM is out; TCB internal).
 *
 * muse_task_slot_run(): a task that ends and may be started again (a session,
 * a scan, the boot). It must end with MUSE_TASK_END(), never vTaskDelete():
 * with the option on, the first run creates one worker for the slot (stack
 * in PSRAM, never deleted) that parks between runs, so a run allocates
 * nothing and nothing is freed, no matter how often it runs. (A WithCaps task
 * that deletes itself makes ESP-IDF v6.0.1 create a cleanup task in internal
 * RAM, and abort() if it can't.) A run asked for while the last one is still
 * finishing waits up to 500 ms for it; if it's still busy, or the first run
 * can't create the worker, it returns pdFAIL and nothing runs. *task, if
 * given, is set before the run starts. The worker's name, stack, priority
 * and core are the first run's.
 */
typedef struct {
    TaskHandle_t worker;
    TaskFunction_t fn;
    void *arg;
    volatile bool busy;
} muse_task_slot_t;

#if CONFIG_MUSE_STACKS_IN_PSRAM
static inline BaseType_t muse_task_create(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                          UBaseType_t prio, TaskHandle_t *task, BaseType_t core)
{
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, prio, task, core,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        return pdPASS;
    }
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, prio, task, core,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
BaseType_t muse_task_slot_run(muse_task_slot_t *slot, TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                              void *arg, UBaseType_t prio, TaskHandle_t *task, BaseType_t core);
/* The end of a slot's run: back to its parked worker. */
#define MUSE_TASK_END() return
#else
static inline BaseType_t muse_task_create(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                          UBaseType_t prio, TaskHandle_t *task, BaseType_t core)
{
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, task, core);
}
static inline BaseType_t muse_task_slot_run(muse_task_slot_t *slot, TaskFunction_t fn, const char *name,
                                            uint32_t stack_bytes, void *arg, UBaseType_t prio, TaskHandle_t *task,
                                            BaseType_t core)
{
    (void)slot;
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, task, core);
}
#define MUSE_TASK_END() vTaskDelete(NULL)
#endif

#ifdef __cplusplus
}
#endif
