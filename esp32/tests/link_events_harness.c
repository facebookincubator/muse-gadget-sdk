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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "link_events.h"

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            exit(1);                                                  \
        }                                                             \
    } while (0)

static int64_t g_now_us;
static bool g_fail_psram;
static size_t g_dma_free = 30000;
static int g_lock_depth;

int64_t esp_timer_get_time(void) { return g_now_us; }

void *heap_caps_malloc(size_t size, int caps) {
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "events must live in PSRAM");
    return g_fail_psram ? NULL : malloc(size);
}
size_t heap_caps_get_free_size(int caps) {
    CHECK(caps == MALLOC_CAP_DMA, "unexpected caps %d", caps);
    return g_dma_free;
}
size_t heap_caps_get_minimum_free_size(int caps) { (void)caps; return 0; }
size_t heap_caps_get_largest_free_block(int caps) {
    CHECK(caps == MALLOC_CAP_DMA, "unexpected caps %d", caps);
    return 4096;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)&g_lock_depth; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    (void)s; (void)ticks;
    CHECK(g_lock_depth == 0, "lock is not reentrant");
    g_lock_depth++;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    CHECK(g_lock_depth == 1, "unbalanced give");
    g_lock_depth--;
    return pdTRUE;
}

static cJSON *events_json_max(size_t max) {
    cJSON *root = cJSON_CreateObject();
    CHECK(link_events_add_json(root, "events", max), "add_json failed");
    cJSON *events = cJSON_GetObjectItem(root, "events");
    CHECK(cJSON_IsArray(events), "events is not an array");
    return root;
}

static cJSON *events_json(void) {
    return events_json_max(LINK_EVENTS_CAPACITY);
}

static int count(const cJSON *array) {
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) n++;
    return n;
}

static const char *at(const cJSON *array, int index) {
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (index-- == 0) return item->valuestring;
    }
    return NULL;
}

int main(void) {
    // Without PSRAM: recording is a no-op and health still gets an empty array.
    g_fail_psram = true;
    CHECK(!link_events_init(), "init must fail without PSRAM");
    link_events_record(LINK_EVENT_WIFI_DOWN, "dropped");
    cJSON *root = events_json();
    CHECK(count(cJSON_GetObjectItem(root, "events")) == 0, "no-PSRAM ring recorded");
    cJSON_Delete(root);

    g_fail_psram = false;
    CHECK(link_events_init(), "init failed");
    g_now_us = 1000000;
    g_dma_free = 12345;
    link_events_record(LINK_EVENT_TUNNEL_DOWN, "keepalive timeout, last ping %us ago", 61u);
    g_now_us = 91000000;
    root = events_json();
    cJSON *events = cJSON_GetObjectItem(root, "events");
    CHECK(count(events) == 1, "expected 1 event");
    CHECK(strcmp(at(events, 0),
                 "90s ago tunnel_down: keepalive timeout, last ping 61s ago "
                 "(dma_free=12345 dma_largest=4096)") == 0,
          "bad line: %s", at(events, 0));
    char *printed = cJSON_PrintUnformatted(root);
    CHECK(printed && strstr(printed, "\"events\":[\"90s ago tunnel_down"), "bad JSON: %s",
          printed ? printed : "(null)");
    free(printed);
    cJSON_Delete(root);

    // Overlong details are truncated, not overflowed.
    char long_detail[200];
    memset(long_detail, 'x', sizeof(long_detail) - 1);
    long_detail[sizeof(long_detail) - 1] = '\0';
    link_events_record(LINK_EVENT_SESSION_DOWN, "%s", long_detail);
    root = events_json();
    events = cJSON_GetObjectItem(root, "events");
    CHECK(strlen(strstr(at(events, 1), "xxx")) < LINK_EVENTS_DETAIL_MAX + 48,
          "detail not truncated");
    cJSON_Delete(root);

    // Wrap keeps the newest CAPACITY events, oldest first.
    for (int i = 0; i < LINK_EVENTS_CAPACITY + 5; i++) {
        g_now_us += 1000000;
        link_events_record(LINK_EVENT_WIFI_UP, "n=%d", i);
    }
    root = events_json();
    events = cJSON_GetObjectItem(root, "events");
    CHECK(count(events) == LINK_EVENTS_CAPACITY, "expected full ring, got %d", count(events));
    CHECK(strstr(at(events, 0), "wifi_up: n=5 "), "oldest wrong: %s", at(events, 0));
    char newest[32];
    snprintf(newest, sizeof(newest), "0s ago wifi_up: n=%d ", LINK_EVENTS_CAPACITY + 4);
    CHECK(strstr(at(events, LINK_EVENTS_CAPACITY - 1), newest),
          "newest wrong: %s", at(events, LINK_EVENTS_CAPACITY - 1));
    cJSON_Delete(root);

    // A smaller max keeps only the newest events, still oldest first.
    root = events_json_max(LINK_EVENTS_HEALTH_MAX);
    events = cJSON_GetObjectItem(root, "events");
    CHECK(count(events) == LINK_EVENTS_HEALTH_MAX, "expected %d, got %d",
          LINK_EVENTS_HEALTH_MAX, count(events));
    char oldest[32];
    snprintf(oldest, sizeof(oldest), "wifi_up: n=%d ",
             LINK_EVENTS_CAPACITY + 5 - LINK_EVENTS_HEALTH_MAX);
    CHECK(strstr(at(events, 0), oldest), "oldest wrong: %s", at(events, 0));
    CHECK(strstr(at(events, LINK_EVENTS_HEALTH_MAX - 1), newest),
          "newest wrong: %s", at(events, LINK_EVENTS_HEALTH_MAX - 1));
    cJSON_Delete(root);
    CHECK(g_lock_depth == 0, "lock leaked");

    puts("link_events harness OK");
    return 0;
}
