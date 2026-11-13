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

static inline QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size) { (void)n; (void)size; return 0; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdFALSE; }
static inline BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdFALSE; }
