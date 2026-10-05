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

// A crash's where and why, kept across the reset it causes. ESP-IDF's panic
// handler is wrapped (-Wl,--wrap=esp_panic_handler, main/CMakeLists.txt) to
// note the reason, task, PC and backtrace in RTC memory before it prints them
// and restarts; the next boot logs them and device.status reports them. A
// core dump would say more, but its stacks take ~4 KB of internal RAM.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "xtensa_context.h"

#include "muse_board.h"
#include "muse_hw_commands_priv.h"
#include "muse_ui.h"

static const char *TAG = "link.crash";

#define MAGIC 0x4352A5E2u   // the record's layout: change with it
#define DEPTH 16
#define REPORT_BOOTS 8    // reported for this many boots after, then forgotten

typedef struct {
    uint32_t magic;
    uint32_t pc, cause, vaddr;
    uint32_t bt[DEPTH];
    uint8_t depth, corrupted, core, pseudo;
    char task[16];
    char reason[64];
    char firmware[9];     // the crashed build's ELF hash, to resolve the backtrace against
    uint32_t count;       // crashes since the board was powered on
    uint32_t boots;       // boots since this one (0: the boot it caused)
    uint32_t sum;
} crash_t;

static RTC_NOINIT_ATTR crash_t s_rtc;
static crash_t *s_last;   // the crash this boot followed, in PSRAM
static char s_elf[9];

static uint32_t checksum(const crash_t *c) {
    const uint8_t *p = (const uint8_t *)c;
    uint32_t h = 2166136261u;   // FNV-1a over all but the sum
    for (size_t i = 0; i < offsetof(crash_t, sum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool valid(const crash_t *c) {
    return c->magic == MAGIC && c->sum == checksum(c);
}

void __real_esp_panic_handler(panic_info_t *info);

// ESP-IDF's: an abort() or failed assert panics as IllegalInstruction, the real
// reason ("assert failed: ...") is here.
extern bool g_panic_abort;
extern char *g_panic_abort_details;

// In the panic handler: no locks, no allocation, nothing that can block.
void __wrap_esp_panic_handler(panic_info_t *info) {
    uint32_t count = valid(&s_rtc) ? s_rtc.count : 0;
    crash_t c = { .magic = MAGIC, .count = count + 1, .boots = 0 };
    const XtExcFrame *f = info->frame;
    c.pc = (uint32_t)info->addr;
    c.core = (uint8_t)info->core;
    c.pseudo = info->pseudo_excause;
    if (f) {
        c.cause = f->exccause;
        c.vaddr = f->excvaddr;
    }
    const char *reason = g_panic_abort && g_panic_abort_details ? g_panic_abort_details : info->reason;
    if (reason) strlcpy(c.reason, reason, sizeof(c.reason));
    memcpy(c.firmware, s_elf, sizeof(c.firmware));
    // The TCB may be what got corrupted: look before reading, and read no more
    // than a name's worth.
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
    const char *name = t && esp_ptr_byte_accessible(t) ? pcTaskGetName(t) : NULL;
    if (name && esp_ptr_byte_accessible(name)) {
        size_t k = 0;
        while (k < sizeof(c.task) - 1 && k < configMAX_TASK_NAME_LEN && name[k]) {
            c.task[k] = name[k];
            k++;
        }
        c.task[k] = '\0';
    }
    if (f && esp_stack_ptr_is_sane(f->a1)) {
        esp_backtrace_frame_t fr = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f };
        c.bt[c.depth++] = esp_cpu_process_stack_pc(fr.pc);
        while (c.depth < DEPTH && fr.next_pc) {
            if (!esp_backtrace_get_next_frame(&fr)) {
                c.corrupted = 1;
                break;
            }
            c.bt[c.depth++] = esp_cpu_process_stack_pc(fr.pc);
        }
    } else {
        c.corrupted = 1;
    }
    c.sum = checksum(&c);
    s_rtc = c;
    __real_esp_panic_handler(info);
}

// ---- A frozen screen -----------------------------------------------------------

// Nothing else notices if the display lock is never given back: the screen,
// and everything waiting on it, freezes while the rest runs on. Checked every
// 5 s; held for a minute (a bench screenshot takes 40 s), every task's
// backtrace goes to the log and the board restarts, leaving a record as a
// crash does.
#define STALL_S 60

static void stall_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // Once the UI is built, the lock exists: from then on any minute counts,
        // even before the first check got the lock.
        if (!muse_board || !muse_board->display_lock || !muse_ui_ready()) continue;
        if (muse_board->display_lock(STALL_S * 1000)) {
            muse_board->display_unlock();
            continue;
        }
        ESP_LOGE(TAG, "the display has been locked for %d s; the tasks, then a restart:", STALL_S);
        esp_backtrace_print_all_tasks(12);
        uint32_t count = valid(&s_rtc) ? s_rtc.count : 0;
        // A plain restart counts a boot on the way up: start one behind, at 0.
        crash_t c = { .magic = MAGIC, .count = count + 1, .boots = UINT32_MAX };
        strlcpy(c.reason, "the display stayed locked", sizeof(c.reason));
        strlcpy(c.task, "?", sizeof(c.task));
        memcpy(c.firmware, s_elf, sizeof(c.firmware));
        c.corrupted = 1;
        c.sum = checksum(&c);
        s_rtc = c;
        esp_restart();
    }
}

void muse_crash_init(void) {
    esp_app_get_elf_sha256(s_elf, sizeof(s_elf));
    // Its stack in PSRAM: it never writes flash (esp_restart doesn't).
    xTaskCreatePinnedToCoreWithCaps(stall_task, "muse_stall", 4096, NULL, 1, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || !valid(&s_rtc)) {
        memset(&s_rtc, 0, sizeof(s_rtc));   // RTC memory holds noise after power-on
        return;
    }
    // Still told for a few boots after, in case one came between (a restart).
    if (r != ESP_RST_PANIC) s_rtc.boots++;
    s_rtc.sum = checksum(&s_rtc);
    if (s_rtc.boots > REPORT_BOOTS) return;
    s_last = heap_caps_malloc(sizeof(*s_last), MALLOC_CAP_SPIRAM);
    if (s_last) *s_last = s_rtc;
    char bt[DEPTH * 11 + 1] = "";
    for (int i = 0; i < s_rtc.depth && i < DEPTH; i++) {
        snprintf(bt + strlen(bt), sizeof(bt) - strlen(bt), "%s0x%08" PRIx32, i ? " " : "", s_rtc.bt[i]);
    }
    ESP_LOGE(TAG, "%" PRIu32 " boots ago it crashed (%" PRIu32 " since power-on): %s in task %s at 0x%08" PRIx32
             " (cause %" PRIu32 ", address 0x%08" PRIx32 "); backtrace %s%s; firmware %s",
             s_rtc.boots, s_rtc.count, s_rtc.reason, s_rtc.task, s_rtc.pc, s_rtc.cause, s_rtc.vaddr, bt,
             s_rtc.corrupted ? " (cut short)" : "", s_rtc.firmware[0] ? s_rtc.firmware : s_elf);
}

void muse_crash_json(cJSON *pl) {
    const crash_t *c = s_last;
    if (!c) return;
    cJSON *o = cJSON_AddObjectToObject(pl, "last_crash");
    if (!o) return;
    char hex[16];
    cJSON_AddStringToObject(o, "reason", c->reason);
    cJSON_AddStringToObject(o, "task", c->task);
    snprintf(hex, sizeof(hex), "0x%08" PRIx32, c->pc);
    cJSON_AddStringToObject(o, "pc", hex);
    cJSON_AddNumberToObject(o, "cause", c->cause);
    snprintf(hex, sizeof(hex), "0x%08" PRIx32, c->vaddr);
    cJSON_AddStringToObject(o, "address", hex);
    cJSON *bt = cJSON_AddArrayToObject(o, "backtrace");
    for (int i = 0; bt && i < c->depth && i < DEPTH; i++) {
        snprintf(hex, sizeof(hex), "0x%08" PRIx32, c->bt[i]);
        cJSON_AddItemToArray(bt, cJSON_CreateString(hex));
    }
    cJSON_AddNumberToObject(o, "boots_ago", c->boots);
    cJSON_AddNumberToObject(o, "since_power_on", c->count);
    cJSON_AddStringToObject(o, "firmware", c->firmware[0] ? c->firmware : s_elf);
}
