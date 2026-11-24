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

// The retained record is an ordinary static on the host, so each
// error_report_init() call below stands for one boot.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error_report.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            exit(1);                                                  \
        }                                                             \
    } while (0)

static esp_app_desc_t g_app = {.version = "1.0.9", .idf_ver = "v6.0.3"};
static esp_reset_reason_t g_reset_reason = ESP_RST_POWERON;
static int64_t g_now_us;

const esp_app_desc_t *esp_app_get_description(void) { return &g_app; }
esp_reset_reason_t esp_reset_reason(void) { return g_reset_reason; }
void esp_restart(void) { abort(); }
int64_t esp_timer_get_time(void) { return g_now_us; }

static void boot(esp_reset_reason_t reason) {
    g_reset_reason = reason;
    g_now_us = 0;
    error_report_init();
}

// Prints one labelled report for the Python test to check, then sends it.
static void emit_report(const char *label) {
    char *json = error_report_pending_json();
    CHECK(json, "%s: expected a queued report", label);
    char *again = error_report_pending_json();
    CHECK(again && strcmp(json, again) == 0,
          "%s: report must stay queued until marked sent", label);
    printf("%s=%s\n", label, json);
    free(json);
    free(again);
    error_report_mark_sent();
    json = error_report_pending_json();
    CHECK(!json, "%s: still queued after send", label);
}

static void expect_nothing_queued(void) {
    char *json = error_report_pending_json();
    CHECK(!json, "unexpected queued report: %s", json);
}

static void test_reset_without_a_record(void) {
    // Nothing armed yet: a panic before the first init must not fabricate one.
    error_report_note_panic((const void *)(uintptr_t)0x1234, NULL, "early",
                            NULL);
    boot(ESP_RST_BROWNOUT);
    emit_report("no_record");
}

static void test_normal_resets_queue_nothing(void) {
    static const esp_reset_reason_t normal[] = {
        ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
        ESP_RST_DEEPSLEEP, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG,
    };
    for (size_t i = 0; i < sizeof(normal) / sizeof(normal[0]); i++) {
        boot(normal[i]);
        expect_nothing_queued();
    }
}

static void test_abnormal_resets_in_a_row(void) {
    static const esp_reset_reason_t abnormal[] = {
        ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
        ESP_RST_BROWNOUT, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP,
    };
    boot(ESP_RST_POWERON);
    for (size_t i = 0; i < sizeof(abnormal) / sizeof(abnormal[0]); i++) {
        boot(abnormal[i]);
        emit_report("in_a_row");
    }
    // A normal reset ends the run.
    boot(ESP_RST_SW);
    boot(ESP_RST_PANIC);
    emit_report("after_normal_reset");
}

static void test_panic_summary(void) {
    static const uint8_t elf_sha[] = {0xa1, 0xb2, 0xc3, 0xd4,
                                      0xe5, 0xf6, 0x07, 0x18, 0x99};
    snprintf(g_app.version, sizeof(g_app.version), "1.0.9");
    memcpy(g_app.app_elf_sha256, elf_sha, sizeof(elf_sha));
    boot(ESP_RST_POWERON);
    g_now_us = 5321999999LL;
    error_report_note_panic((const void *)(uintptr_t)0x42012345u,
                            (const void *)(uintptr_t)0x4200a1b2u,
                            "Load access fault", NULL);
    // A fault inside the panic handler must not replace the first one.
    error_report_note_panic((const void *)(uintptr_t)0x40000000u,
                            (const void *)(uintptr_t)0x40000004u,
                            "Illegal instruction", NULL);

    // The bootloader rolled back: the report still names what crashed.
    snprintf(g_app.version, sizeof(g_app.version), "1.0.8");
    g_app.app_elf_sha256[0] = 0x0f;
    boot(ESP_RST_PANIC);
    emit_report("panic");

    // The panic detail belonged to that boot only.
    boot(ESP_RST_WDT);
    emit_report("after_panic");
}

static void test_abort_detail_is_bounded_and_printable(void) {
    char detail[400];
    memset(detail, 'x', sizeof(detail) - 1);
    detail[sizeof(detail) - 1] = '\0';
    memcpy(detail, "assert \"a\"\r\n\x01\xff", 14);

    boot(ESP_RST_POWERON);
    g_now_us = 7 * 1000000LL;
    error_report_note_panic(NULL, NULL, "abort", detail);
    boot(ESP_RST_PANIC);
    emit_report("abort");
}

int main(void) {
    test_reset_without_a_record();
    test_normal_resets_queue_nothing();
    test_abnormal_resets_in_a_row();
    test_panic_summary();
    test_abort_detail_is_bounded_and_printable();
    return 0;
}
