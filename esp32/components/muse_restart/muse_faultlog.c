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

/*
 * The ESP-Mosaico's (S31) PSRAM access faults: panics on loads and stores
 * at valid PSRAM addresses (README.md, Fault log). To tell whether they
 * line up with the caches being off for a flash operation, every flash
 * operation's window (spi_flash_disable_interrupts_caches_and_other_cpu()
 * to ..._enable_...) goes into a ring in RTC memory, and the panic handler
 * notes when it ran, on which core, at which pc, and whether a flash
 * operation was open then. The next boot logs it all (muse_faultlog_report).
 *
 * The wrappers run with the caches off: IRAM only, internal memory only,
 * no library calls. CMakeLists.txt wraps the flash symbols on the S31; the
 * panic handler's wrapper is muse_crashrec.c's (every target), which calls
 * muse_faultlog_on_panic() here.
 */

#include "muse_restart.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FLOG_MAGIC 0x464c4f47u   /* "FLOG" */
#define FLOG_N 16
#define FLOG_NAME 12

typedef struct {
    int64_t t_us;       /* the caches went off (this boot's esp_timer) */
    uint32_t dur_us;    /* how long they stayed off (0: still off) */
    uint8_t core;
    char task[FLOG_NAME];
} flog_op_t;

typedef struct {
    uint32_t magic;
    uint32_t count;     /* flash operations this boot */
    uint32_t open;      /* 1 + the ring slot of the operation going on, 0 for none */
    flog_op_t ops[FLOG_N];
    int64_t panic_us;   /* 0: no panic this boot */
    uint32_t panic_pc;
    uint8_t panic_core;
    uint8_t panic_open; /* a flash operation was going on at the panic */
    uint32_t panic_open_us;   /* ...for this long */
    char panic_task[FLOG_NAME];
} flog_t;

static RTC_NOINIT_ATTR flog_t s_rtc;
static flog_t s_prev;   /* the previous boot's, read at start */
static bool s_have_prev;
/* Set by muse_faultlog_init(): until then the ring holds the previous
 * boot's, which the boot's own early flash operations mustn't touch. */
static DRAM_ATTR volatile bool s_armed;

static inline void IRAM_ATTR name_copy(char *dst, const char *src)
{
    int i = 0;
    for (; src && i < FLOG_NAME - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
    /* Caches off, the flash lock held, interrupts off on both cores. */
    if (!s_armed) {
        return;
    }
    uint32_t slot = s_rtc.count % FLOG_N;
    flog_op_t *op = &s_rtc.ops[slot];
    op->t_us = esp_timer_get_time();
    op->dur_us = 0;
    op->core = (uint8_t)esp_cpu_get_core_id();
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    name_copy(op->task, t ? pcTaskGetName(t) : "-");
    s_rtc.open = slot + 1;
}

void IRAM_ATTR __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void)
{
    if (s_armed && s_rtc.open) {
        flog_op_t *op = &s_rtc.ops[s_rtc.open - 1];
        int64_t d = esp_timer_get_time() - op->t_us;
        op->dur_us = d > 0 ? (uint32_t)d : 1;
        s_rtc.open = 0;
        s_rtc.count++;
    }
    __real_spi_flash_enable_interrupts_caches_and_other_cpu();
}

/* From the panic handler's wrapper (muse_crashrec.c, every target). */
void IRAM_ATTR muse_faultlog_on_panic(panic_info_t *info)
{
    if (s_armed && !s_rtc.panic_us) {
        int64_t now = esp_timer_get_time();
        s_rtc.panic_us = now ? now : 1;
        s_rtc.panic_pc = (uint32_t)(uintptr_t)info->addr;
        s_rtc.panic_core = (uint8_t)info->core;
        s_rtc.panic_open = s_rtc.open != 0;
        s_rtc.panic_open_us = s_rtc.open ? (uint32_t)(now - s_rtc.ops[s_rtc.open - 1].t_us) : 0;
        TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
        name_copy(s_rtc.panic_task, t ? pcTaskGetName(t) : "-");
    }
}

void muse_faultlog_init(void)
{
    s_have_prev = s_rtc.magic == FLOG_MAGIC && s_rtc.panic_us;
    if (s_have_prev) {
        s_prev = s_rtc;
    }
    flog_t z = { 0 };
    z.magic = FLOG_MAGIC;
    s_rtc = z;
    s_armed = true;
}

void muse_faultlog_dump(const char *tag)
{
    flog_t c = s_rtc;   /* a snapshot: a flash operation may be under way */
    uint32_t n = c.count;
    int64_t now = esp_timer_get_time();
    ESP_LOGI(tag, "flash operations this boot: %lu; the last ones (ms ago, how long, core, task):", (unsigned long)n);
    for (uint32_t i = n > FLOG_N ? n - FLOG_N : 0; i < n; i++) {
        const flog_op_t *op = &c.ops[i % FLOG_N];
        ESP_LOGI(tag, "  -%lld ms: %lu us, core %u, %s", (long long)((now - op->t_us) / 1000),
                 (unsigned long)op->dur_us, (unsigned)op->core, op->task);
    }
}

void muse_faultlog_report(const char *tag)
{
    if (!s_have_prev) {
        return;
    }
    const flog_t *p = &s_prev;
    ESP_LOGW(tag, "last panic: %lld.%03lld s into that boot, core %u, task %s, pc 0x%08lx; a flash operation %s",
             (long long)(p->panic_us / 1000000), (long long)(p->panic_us / 1000 % 1000), (unsigned)p->panic_core,
             p->panic_task, (unsigned long)p->panic_pc,
             p->panic_open ? "WAS going on (caches off)" : "was not going on");
    if (p->panic_open) {
        ESP_LOGW(tag, "  ...for %lu us by then", (unsigned long)p->panic_open_us);
    }
    ESP_LOGW(tag, "  flash operations that boot: %lu; the last ones (ms before the panic, how long, core, task):",
             (unsigned long)p->count);
    uint32_t n = p->count + (p->open ? 1 : 0);
    uint32_t first = n > FLOG_N ? n - FLOG_N : 0;
    for (uint32_t i = first; i < n; i++) {
        const flog_op_t *op = &p->ops[i % FLOG_N];
        long long before = (long long)((p->panic_us - op->t_us) / 1000);
        ESP_LOGW(tag, "  -%lld ms: %lu us, core %u, %s", before, (unsigned long)op->dur_us, (unsigned)op->core,
                 op->task);
    }
}
