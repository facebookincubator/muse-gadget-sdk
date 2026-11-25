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
#else
#define RETAINED_ATTR
#endif

#define TAG "link.error_report"

#define ERROR_REPORT_SCHEMA_VERSION 1
#define RECORD_MAGIC 0x4c455231u
#define RECORD_FLAG_PANIC 0x1u
#define RECORD_FLAG_ADDRESS 0x2u
#define RECORD_FLAG_RETURN_ADDRESS 0x4u

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

static RETAINED_ATTR crash_record_t s_record;

static crash_record_t s_pending;
static const char *s_pending_reason;
static atomic_bool s_has_pending = ATOMIC_VAR_INIT(false);

static uint32_t record_checksum(const crash_record_t *record) {
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(crash_record_t, checksum); i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

static bool record_is_valid(const crash_record_t *record) {
    return record->magic == RECORD_MAGIC
           && record->checksum == record_checksum(record);
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

static void arm_record(uint32_t consecutive_count) {
    memset(&s_record, 0, sizeof(s_record));
    s_record.magic = RECORD_MAGIC;
    s_record.consecutive_count = consecutive_count;
    const esp_app_desc_t *app = esp_app_get_description();
    copy_printable(s_record.version, sizeof(s_record.version), app->version);
    // The same 16 hex digits get_device_info reports as build_sha.
    for (size_t i = 0; i < 8; i++) {
        snprintf(s_record.build_sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
    s_record.checksum = record_checksum(&s_record);
}

void error_report_init(void) {
    const char *reason = abnormal_reset_name(esp_reset_reason());
    bool valid = record_is_valid(&s_record);
    uint32_t count = 0;
    if (reason) {
        uint32_t previous = valid ? s_record.consecutive_count : 0;
        count = previous < UINT32_MAX ? previous + 1 : previous;
        if (valid) {
            s_pending = s_record;
        } else {
            memset(&s_pending, 0, sizeof(s_pending));
        }
        s_pending.consecutive_count = count;
        s_pending_reason = reason;
        atomic_store(&s_has_pending, true);
        ESP_LOGW(TAG, "previous boot ended in %s (%" PRIu32 " in a row)",
                 reason, count);
    }
    arm_record(count);
}

void error_report_note_panic(const void *address, const void *return_address,
                             const char *cause, const char *detail) {
    // A panic inside the panic handler re-enters here: keep the first one.
    if (!record_is_valid(&s_record) || (s_record.flags & RECORD_FLAG_PANIC)) {
        return;
    }
    s_record.flags = RECORD_FLAG_PANIC | (address ? RECORD_FLAG_ADDRESS : 0)
                     | (return_address ? RECORD_FLAG_RETURN_ADDRESS : 0);
    s_record.fault_address = (uint32_t)(uintptr_t)address;
    s_record.return_address = (uint32_t)(uintptr_t)return_address;
    s_record.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000LL);
    copy_printable(s_record.cause, sizeof(s_record.cause), cause);
    copy_printable(s_record.detail, sizeof(s_record.detail), detail);
    s_record.checksum = record_checksum(&s_record);
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
    cJSON *fault = add_object(report, "fault");
    if (!fault) return false;
    if (s_pending.flags & RECORD_FLAG_ADDRESS) {
        char address[11];
        snprintf(address, sizeof(address), "0x%08" PRIx32,
                 s_pending.fault_address);
        if (!cJSON_AddStringToObject(fault, "address", address)) return false;
    }
    if (s_pending.flags & RECORD_FLAG_RETURN_ADDRESS) {
        char address[11];
        snprintf(address, sizeof(address), "0x%08" PRIx32,
                 s_pending.return_address);
        if (!cJSON_AddStringToObject(fault, "return_address", address)) {
            return false;
        }
    }
    if (s_pending.cause[0]
        && !cJSON_AddStringToObject(fault, "cause", s_pending.cause)) {
        return false;
    }
    if (s_pending.detail[0]
        && !cJSON_AddStringToObject(fault, "detail", s_pending.detail)) {
        return false;
    }
    return true;
}

char *error_report_pending_json(void) {
    if (!atomic_load(&s_has_pending)) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    bool panic = (s_pending.flags & RECORD_FLAG_PANIC) != 0;
    cJSON *report = NULL;
    bool ok = cJSON_AddStringToObject(root, "method", "link.error_report")
              && (report = add_object(root, "report"))
              && cJSON_AddNumberToObject(report, "schema_version",
                                         ERROR_REPORT_SCHEMA_VERSION)
              && cJSON_AddStringToObject(report, "kind", "crash")
              && cJSON_AddStringToObject(report, "reason", s_pending_reason)
              && cJSON_AddNumberToObject(report, "consecutive_count",
                                         s_pending.consecutive_count)
              && (!s_pending.version[0]
                  || cJSON_AddStringToObject(report, "version",
                                             s_pending.version))
              && (!s_pending.build_sha[0]
                  || cJSON_AddStringToObject(report, "build_sha",
                                             s_pending.build_sha))
              && (!panic
                  || (cJSON_AddNumberToObject(report, "uptime_seconds",
                                              s_pending.uptime_seconds)
                      && add_fault(report)));
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

void error_report_mark_sent(void) {
    atomic_store(&s_has_pending, false);
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
