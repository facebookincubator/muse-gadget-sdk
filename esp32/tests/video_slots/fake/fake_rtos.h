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

/* Host fake for tests/video_slots/harness.c (tests/test_video_untrusted.py). */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
typedef int BaseType_t;
/* A spinlock section: a mutex will do on pthreads. */
typedef struct {
    pthread_mutex_t m;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { PTHREAD_MUTEX_INITIALIZER }
#define portENTER_CRITICAL(mux) pthread_mutex_lock(&(mux)->m)
#define portEXIT_CRITICAL(mux) pthread_mutex_unlock(&(mux)->m)
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef struct fake_queue *QueueHandle_t;
typedef QueueHandle_t SemaphoreHandle_t;
typedef struct fake_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY (-1)
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
typedef struct {
    int unused;
} StaticSemaphore_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *h, BaseType_t core);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *h, BaseType_t core, unsigned caps);
BaseType_t xTaskGetCoreID(TaskHandle_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core);
void xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);
void vTaskDeleteWithCaps(TaskHandle_t t);
void taskYIELD(void);
