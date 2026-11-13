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

#include "link_events.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint64_t at_us;
    uint32_t dma_free;
    uint32_t dma_largest;
    link_event_kind_t kind;
    char detail[LINK_EVENTS_DETAIL_MAX];
} link_event_t;

static link_event_t *s_ring = NULL;
static size_t s_head = 0;  // next slot to write
static size_t s_len = 0;
static SemaphoreHandle_t s_lock = NULL;

static const char *kind_name(link_event_kind_t kind) {
    switch (kind) {
        case LINK_EVENT_WIFI_DOWN: return "wifi_down";
        case LINK_EVENT_WIFI_UP: return "wifi_up";
        case LINK_EVENT_SESSION_UP: return "session_up";
        case LINK_EVENT_SESSION_DOWN: return "session_down";
        case LINK_EVENT_TUNNEL_UP: return "tunnel_up";
        case LINK_EVENT_TUNNEL_DOWN: return "tunnel_down";
        case LINK_EVENT_TUNNEL_TX_BLOCKED: return "tunnel_tx_blocked";
        case LINK_EVENT_STALL_PROBE: return "stall_probe";
        default: return "unknown";
    }
}

bool link_events_init(void) {
    if (s_ring) return true;
    link_event_t *ring = heap_caps_malloc(
        LINK_EVENTS_CAPACITY * sizeof(link_event_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) return false;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) {
        free(ring);
        return false;
    }
    s_lock = lock;
    s_ring = ring;
    return true;
}

void link_events_record(link_event_kind_t kind, const char *fmt, ...) {
    if (!s_ring) return;
    link_event_t ev;
    ev.at_us = (uint64_t)esp_timer_get_time();
    ev.dma_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    ev.dma_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ev.kind = kind;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ev.detail, sizeof(ev.detail), fmt, args);
    va_end(args);

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_ring[s_head] = ev;
    s_head = (s_head + 1) % LINK_EVENTS_CAPACITY;
    if (s_len < LINK_EVENTS_CAPACITY) s_len++;
    xSemaphoreGive(s_lock);
}

bool link_events_add_json(struct cJSON *parent, const char *name, size_t max) {
    cJSON *array = cJSON_CreateArray();
    if (!array || !cJSON_AddItemToObject(parent, name, array)) {
        cJSON_Delete(array);
        return false;
    }
    if (!s_ring) return true;

    // Copy out under the lock so formatting never blocks a recorder.
    link_event_t *copy = heap_caps_malloc(
        LINK_EVENTS_CAPACITY * sizeof(link_event_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        free(copy);
        return false;
    }
    size_t len = s_len < max ? s_len : max;
    size_t start = (s_head + LINK_EVENTS_CAPACITY - len) % LINK_EVENTS_CAPACITY;
    for (size_t i = 0; i < len; i++) {
        copy[i] = s_ring[(start + i) % LINK_EVENTS_CAPACITY];
    }
    xSemaphoreGive(s_lock);

    uint64_t now = (uint64_t)esp_timer_get_time();
    bool ok = true;
    for (size_t i = 0; ok && i < len; i++) {
        const link_event_t *ev = &copy[i];
        uint64_t ago_s = now > ev->at_us ? (now - ev->at_us) / 1000000 : 0;
        char line[LINK_EVENTS_DETAIL_MAX + 96];
        snprintf(line, sizeof(line), "%llus ago %s: %s (dma_free=%u dma_largest=%u)",
                 (unsigned long long)ago_s, kind_name(ev->kind), ev->detail,
                 (unsigned)ev->dma_free, (unsigned)ev->dma_largest);
        cJSON *item = cJSON_CreateString(line);
        ok = item && cJSON_AddItemToArray(array, item);
        if (!ok) cJSON_Delete(item);
    }
    free(copy);
    return ok;
}
