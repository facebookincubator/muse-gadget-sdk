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

#include "muse_stack.h"

#if CONFIG_MUSE_STACKS_IN_PSRAM

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* A slot's worker: one run per notification, then parked again (never deleted). */
static void slot_worker(void *arg)
{
    muse_task_slot_t *slot = arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        slot->fn(slot->arg);
        __atomic_store_n(&slot->busy, false, __ATOMIC_RELEASE);
    }
}

BaseType_t muse_task_slot_run(muse_task_slot_t *slot, TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                              void *arg, UBaseType_t prio, TaskHandle_t *task, BaseType_t core)
{
    /* The last run may be between its own "I'm done" and returning to the
     * worker (the Noise session clears its handle just before it ends). */
    for (int waited_ms = 0; __atomic_exchange_n(&slot->busy, true, __ATOMIC_ACQ_REL); waited_ms += 10) {
        if (waited_ms >= 500) {
            return pdFAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    slot->fn = fn;
    slot->arg = arg;
    if (!slot->worker && muse_task_create(slot_worker, name, stack_bytes, slot, prio, &slot->worker, core) != pdPASS) {
        slot->worker = NULL;
        __atomic_store_n(&slot->busy, false, __ATOMIC_RELEASE);
        return pdFAIL;
    }
    if (task) {
        *task = slot->worker;
    }
    xTaskNotifyGive(slot->worker);
    return pdPASS;
}

#endif
