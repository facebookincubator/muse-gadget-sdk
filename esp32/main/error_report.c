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

#include "error_report.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ARCH_RISCV
#include "riscv/rvruntime-frames.h"
#endif
#if CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED
#define RETAINED_ATTR RTC_NOINIT_ATTR
#else
#define RETAINED_ATTR __NOINIT_ATTR
#endif
#elif !defined(RETAINED_ATTR)
// Host tests may supply it to keep these across simulated resets.
#define RETAINED_ATTR
#endif

#define TAG "link.error_report"

#define ERROR_REPORT_SCHEMA_VERSION 1
#define RECORD_MAGIC 0x4c455231u
#define PENDING_MAGIC 0x4c455232u
#define RECORD_FLAG_PANIC 0x1u
#define RECORD_FLAG_ADDRESS 0x2u
#define RECORD_FLAG_RETURN_ADDRESS 0x4u
// The boot crashed before error_report_init(), so nothing else is known.
#define RECORD_FLAG_EARLY 0x8u
// A pending report keeps its esp_reset_reason_t in the flags' upper bits.
#define RECORD_REASON_SHIFT 8

// All-uint32 header and char arrays: no padding, so the checksum covers every
// byte before it.
typedef struct {
    uint32_t magic;
    uint32_t consecutive_count;
    uint32_t flags;
    uint32_t uptime_seconds;
    uint32_t fault_address;
    uint32_t return_address;
    char version[32];
    char build_sha[20];
    char cause[48];
    char detail[96];
    uint32_t checksum;
} crash_record_t;

// Retained RAM. `record` is this boot's, which a panic fills in. It comes
// first and keeps its layout, so an image a rollback returns to still reads
// it. `pending` is the report waiting to be sent: retained too, so a restart
// before the device registers (Muse boards restart right after pairing)
// keeps it.
typedef struct {
    crash_record_t record;
    crash_record_t pending;
} retained_t;
_Static_assert(sizeof(crash_record_t) == 224 && offsetof(retained_t, record) == 0,
               "older images read the record here, in this layout");
static RETAINED_ATTR retained_t s_retained;

// Ordinary RAM, which ESP-IDF clears first thing at every boot. s_armed is
// set once error_report_init() has armed the record in this boot; until then
// the record is whatever an earlier boot left.
static atomic_bool s_armed = ATOMIC_VAR_INIT(false);
static bool s_noted_early;
static atomic_bool s_has_pending = ATOMIC_VAR_INIT(false);

static uint32_t record_checksum(const crash_record_t *record) {
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(crash_record_t, checksum); i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

static bool record_is_valid(const crash_record_t *record, uint32_t magic) {
    return record->magic == magic && record->checksum == record_checksum(record);
}

static uint32_t one_more(uint32_t count) {
    return count < UINT32_MAX ? count + 1 : count;
}

// Keeps printable ASCII and zero-fills the rest of dst, so a stored string is
// always terminated and the checksum is deterministic.
static void copy_printable(char *dst, size_t size, const char *src) {
    size_t n = 0;
    while (src && src[n] && n + 1 < size) {
        char c = src[n];
        dst[n++] = (c >= 0x20 && c < 0x7f) ? c : ' ';
    }
    while (n < size) dst[n++] = '\0';
}

// NULL for resets that are expected in normal operation.
static const char *abnormal_reset_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return NULL;
    }
}

static const char *pending_reason(void) {
    return abnormal_reset_name(
        (esp_reset_reason_t)(s_retained.pending.flags >> RECORD_REASON_SHIFT));
}

static void arm_record(uint32_t consecutive_count) {
    crash_record_t *record = &s_retained.record;
    memset(record, 0, sizeof(*record));
    record->magic = RECORD_MAGIC;
    record->consecutive_count = consecutive_count;
    const esp_app_desc_t *app = esp_app_get_description();
    copy_printable(record->version, sizeof(record->version), app->version);
    // The same 16 hex digits get_device_info reports as build_sha.
    for (size_t i = 0; i < 8; i++) {
        snprintf(record->build_sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
    record->checksum = record_checksum(record);
}

void error_report_init(void) {
    crash_record_t *record = &s_retained.record;
    crash_record_t *pending = &s_retained.pending;
    esp_reset_reason_t reset = esp_reset_reason();
    const char *reason = abnormal_reset_name(reset);
    bool valid = record_is_valid(record, RECORD_MAGIC);
    uint32_t count = 0;
    if (reason) {
        count = one_more(valid ? record->consecutive_count : 0);
        // Replaces a report still waiting from an earlier crash.
        if (valid) {
            *pending = *record;
        } else {
            memset(pending, 0, sizeof(*pending));
        }
        pending->magic = PENDING_MAGIC;
        pending->consecutive_count = count;
        pending->flags = (pending->flags & ((1u << RECORD_REASON_SHIFT) - 1))
                         | ((uint32_t)reset << RECORD_REASON_SHIFT);
        pending->checksum = record_checksum(pending);
        ESP_LOGW(TAG, "previous boot ended in %s (%" PRIu32 " in a row)",
                 reason, count);
    } else if (reset == ESP_RST_POWERON) {
        // Retained RAM doesn't keep its contents without power.
        memset(pending, 0, sizeof(*pending));
    }
    atomic_store(&s_has_pending,
                 record_is_valid(pending, PENDING_MAGIC) && pending_reason());
    arm_record(count);
    atomic_store(&s_armed, true);
}

// A fault before error_report_init() in this boot, perhaps before esp_timer
// has started. The record is the previous boot's, whose version, binary and
// fault needn't be this one's, so keep only the run of crashes: one longer if
// that boot crashed too, as its record says, else starting here. The next boot
// reports the reset and its count, and nothing it can't vouch for.
static void note_early_panic(void) {
    if (s_noted_early) return;  // a panic inside the panic handler
    s_noted_early = true;
    crash_record_t *record = &s_retained.record;
    uint32_t count = 0;
    if (record_is_valid(record, RECORD_MAGIC)
        && (record->flags & (RECORD_FLAG_PANIC | RECORD_FLAG_EARLY))) {
        count = one_more(record->consecutive_count);
    }
    memset(record, 0, sizeof(*record));
    record->magic = RECORD_MAGIC;
    record->consecutive_count = count;
    record->flags = RECORD_FLAG_EARLY;
    record->checksum = record_checksum(record);
}

void error_report_note_panic(const void *address, const void *return_address,
                             const char *cause, const char *detail) {
    if (!atomic_load(&s_armed)) {
        note_early_panic();
        return;
    }
    crash_record_t *record = &s_retained.record;
    // A panic inside the panic handler re-enters here: keep the first one.
    if (!record_is_valid(record, RECORD_MAGIC)
        || (record->flags & RECORD_FLAG_PANIC)) {
        return;
    }
    record->flags = RECORD_FLAG_PANIC | (address ? RECORD_FLAG_ADDRESS : 0)
                    | (return_address ? RECORD_FLAG_RETURN_ADDRESS : 0);
    record->fault_address = (uint32_t)(uintptr_t)address;
    record->return_address = (uint32_t)(uintptr_t)return_address;
    record->uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000LL);
    copy_printable(record->cause, sizeof(record->cause), cause);
    copy_printable(record->detail, sizeof(record->detail), detail);
    record->checksum = record_checksum(record);
}

static cJSON *add_object(cJSON *parent, const char *name) {
    cJSON *child = cJSON_CreateObject();
    if (!child) return NULL;
    if (!cJSON_AddItemToObject(parent, name, child)) {
        cJSON_Delete(child);
        return NULL;
    }
    return child;
}

static bool add_fault(cJSON *report) {
    const crash_record_t *pending = &s_retained.pending;
    cJSON *fault = add_object(report, "fault");
    if (!fault) return false;
    if (pending->flags & RECORD_FLAG_ADDRESS) {
        char address[11];
        snprintf(address, sizeof(address), "0x%08" PRIx32,
                 pending->fault_address);
        if (!cJSON_AddStringToObject(fault, "address", address)) return false;
    }
    if (pending->flags & RECORD_FLAG_RETURN_ADDRESS) {
        char address[11];
        snprintf(address, sizeof(address), "0x%08" PRIx32,
                 pending->return_address);
        if (!cJSON_AddStringToObject(fault, "return_address", address)) {
            return false;
        }
    }
    if (pending->cause[0]
        && !cJSON_AddStringToObject(fault, "cause", pending->cause)) {
        return false;
    }
    if (pending->detail[0]
        && !cJSON_AddStringToObject(fault, "detail", pending->detail)) {
        return false;
    }
    return true;
}

char *error_report_pending_json(void) {
    if (!atomic_load(&s_has_pending)) return NULL;
    const crash_record_t *pending = &s_retained.pending;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    bool panic = (pending->flags & RECORD_FLAG_PANIC) != 0;
    cJSON *report = NULL;
    bool ok = cJSON_AddStringToObject(root, "method", "link.error_report")
              && (report = add_object(root, "report"))
              && cJSON_AddNumberToObject(report, "schema_version",
                                         ERROR_REPORT_SCHEMA_VERSION)
              && cJSON_AddStringToObject(report, "kind", "crash")
              && cJSON_AddStringToObject(report, "reason", pending_reason())
              && cJSON_AddNumberToObject(report, "consecutive_count",
                                         pending->consecutive_count)
              && (!pending->version[0]
                  || cJSON_AddStringToObject(report, "version",
                                             pending->version))
              && (!pending->build_sha[0]
                  || cJSON_AddStringToObject(report, "build_sha",
                                             pending->build_sha))
              && (!panic
                  || (cJSON_AddNumberToObject(report, "uptime_seconds",
                                              pending->uptime_seconds)
                      && add_fault(report)));
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

void error_report_discard(void) {
    atomic_store(&s_has_pending, false);
    memset(&s_retained.pending, 0, sizeof(s_retained.pending));
}

void error_report_mark_sent(void) {
    error_report_discard();
}

#ifdef ESP_PLATFORM
void __real_esp_panic_handler(panic_info_t *info);

// The caller, when the faulting function calls nothing itself; otherwise a
// point in the faulting function just after the last call it made.
static const void *frame_return_address(const panic_info_t *info) {
#if CONFIG_IDF_TARGET_ARCH_RISCV
    const RvExcFrame *frame = info->frame;
    return frame ? (const void *)frame->ra : NULL;
#else
    (void)info;
    return NULL;
#endif
}

// Linked in place of esp_panic_handler (see main/CMakeLists.txt). An abort
// reaches here through a deliberate fault inside panic_abort, so its address
// and exception name say nothing; its message does.
void __wrap_esp_panic_handler(panic_info_t *info) {
    if (g_panic_abort) {
        error_report_note_panic(NULL, NULL, "abort", g_panic_abort_details);
    } else {
        error_report_note_panic(info->addr, frame_return_address(info),
                                info->reason, NULL);
    }
    __real_esp_panic_handler(info);
}
#endif
