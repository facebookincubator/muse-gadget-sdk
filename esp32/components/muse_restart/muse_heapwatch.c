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
#include "muse_heapwatch.h"

#include "sdkconfig.h"

#if CONFIG_MUSE_HEAP_WATCH
#include <stdio.h>

#include "esp_heap_caps.h"
#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"

/* Every internal allocation of 512 B or more, and when it was freed. */
#define RING 160
typedef struct {
    void *ptr;
    uint32_t size;
    uint32_t t_ms, freed_ms;
    uint32_t pc[6];
} big_alloc_t;
static DRAM_ATTR big_alloc_t s_ring[RING];
static DRAM_ATTR volatile uint32_t s_next;

void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    if (!ptr || size < 512 || !esp_ptr_internal(ptr)) return;
    uint32_t i = __atomic_fetch_add(&s_next, 1, __ATOMIC_RELAXED) % RING;
    big_alloc_t *b = &s_ring[i];
    b->ptr = ptr;
    b->size = size;
    b->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    b->freed_ms = 0;
    esp_backtrace_frame_t f = { 0 };
    esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
    for (int k = 0; k < 6; k++) {
        b->pc[k] = f.pc;
        if (!f.next_pc || !esp_backtrace_get_next_frame(&f)) {
            for (k++; k < 6; k++) b->pc[k] = 0;
            break;
        }
    }
}

void esp_heap_trace_free_hook(void *ptr)
{
    if (!ptr || !esp_ptr_internal(ptr)) return;
    for (uint32_t i = 0; i < RING; i++) {
        if (s_ring[i].ptr == ptr && !s_ring[i].freed_ms) {
            s_ring[i].freed_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (!s_ring[i].freed_ms) s_ring[i].freed_ms = 1;
            break;
        }
    }
}

void muse_heapwatch_print_allocs(void)
{
    uint32_t n = s_next, start = n > RING ? n - RING : 0;
    printf("@heap.allocs %u recorded (internal, >= 512 B)\n", (unsigned)n);
    for (uint32_t j = start; j < n; j++) {
        const big_alloc_t *b = &s_ring[j % RING];
        uint32_t pc[6];
        for (int k = 0; k < 6; k++) {   /* Xtensa: window bits off, back to the call */
            pc[k] = b->pc[k] ? ((b->pc[k] & 0x3fffffffu) | 0x40000000u) - 3 : 0;
        }
        printf("@heap.alloc t=%u size=%u freed=%u bt=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
               (unsigned)b->t_ms, (unsigned)b->size, (unsigned)b->freed_ms, (unsigned)pc[0],
               (unsigned)pc[1], (unsigned)pc[2], (unsigned)pc[3], (unsigned)pc[4], (unsigned)pc[5]);
    }
    fflush(stdout);
}

static const char *TAG = "heap.watch";

static void watch_task(void *arg)
{
    (void)arg;
    size_t low = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "internal free %u at start (lowest ever %u)", (unsigned)low,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    for (;;) {
        size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (f + 512 <= low) {
            low = f;
            ESP_LOGW(TAG, "internal low %u (largest block %u) at %lld ms", (unsigned)f,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (long long)(esp_timer_get_time() / 1000));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void muse_heapwatch_start(void)
{
    /* Its stack in PSRAM, so the watcher itself takes no internal RAM. */
    if (xTaskCreatePinnedToCoreWithCaps(watch_task, "heap_watch", 3072, NULL, 1, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "not started");
    }
}

void muse_heapwatch_print_tasks(void)
{
#if !CONFIG_HEAP_TASK_TRACKING
    printf("@heap.tasks internal free %u, lowest ever %u (per task: build with HEAP_TASK_TRACKING)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    fflush(stdout);
#else
    /* Internal RAM only: each task's current and peak use on the internal
     * heaps (the overview mixes in PSRAM). */
    heap_all_tasks_stat_t st = { .task_count = 24, .heap_count = 24 * 8, .alloc_count = 0 };
    if (heap_caps_alloc_all_task_stat_arrays(&st) != ESP_OK) {
        printf("@heap.tasks no memory\n");
        return;
    }
    if (heap_caps_get_all_task_stat(&st) == ESP_OK) {
        printf("@heap.tasks internal free %u, lowest ever %u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        for (size_t t = 0; t < st.task_count; t++) {
            const task_stat_t *ts = &st.stat_arr[t];
            size_t cur = 0, peak = 0;
            for (size_t h = 0; h < ts->heap_count; h++) {
                if (ts->heap_stat[h].caps & MALLOC_CAP_INTERNAL) {
                    cur += ts->heap_stat[h].current_usage;
                    peak += ts->heap_stat[h].peak_usage;
                }
            }
            if (peak) printf("@heap.task %-16s %s internal now %6u peak %6u\n", ts->name,
                             ts->is_alive ? "alive" : "gone ", (unsigned)cur, (unsigned)peak);
        }
    }
    fflush(stdout);
    heap_caps_free_all_task_stat_arrays(&st);
#endif
}
#else
void muse_heapwatch_start(void) {}
void muse_heapwatch_print_tasks(void) {}
void muse_heapwatch_print_allocs(void) {}
#endif
