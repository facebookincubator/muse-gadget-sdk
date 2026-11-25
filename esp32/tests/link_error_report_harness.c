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

// Each boot runs in a child process forked from a parent that never calls
// into error_report.c, so it starts with that file's ordinary statics as a
// fresh boot does. Only its RETAINED_ATTR variables, which the test build
// puts in their own section, carry over from one boot to the next, as RTC
// memory does on the chip.
//
//     if (started(ESP_RST_PANIC)) { ...code that runs during this boot... }
//
// Code outside a boot's braces is what happens between boots (flashing
// another image). What counts is that it runs in the parent; the previous
// boot's process also runs it on its way to the next boot() call, where it
// ends.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "error_report.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"

#ifdef __APPLE__
extern unsigned char g_retained_start[] __asm(
    "section$start$__DATA$__err_retained");
extern unsigned char g_retained_end[] __asm(
    "section$end$__DATA$__err_retained");
#else
extern unsigned char __start_error_report_retained[];
extern unsigned char __stop_error_report_retained[];
#define g_retained_start __start_error_report_retained
#define g_retained_end __stop_error_report_retained
#endif

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
// ESP-IDF starts esp_timer partway through startup, before app_main.
static bool g_timer_started;

const esp_app_desc_t *esp_app_get_description(void) { return &g_app; }
esp_reset_reason_t esp_reset_reason(void) { return g_reset_reason; }
void esp_restart(void) { abort(); }
int64_t esp_timer_get_time(void) {
    CHECK(g_timer_started, "esp_timer_get_time() before esp_timer started");
    return g_now_us;
}

static bool g_in_boot;           // in the child process running a boot
static unsigned char *g_rtc;     // the retained section between boots
static size_t g_rtc_size;

// Plain byte copies: sanitizers may pad the section's variables.
__attribute__((no_sanitize_address)) static void copy_retained(
    volatile unsigned char *dst, const volatile unsigned char *src) {
    for (size_t i = 0; i < g_rtc_size; i++) dst[i] = src[i];
}

// The board resets here: this boot's process hands back the retained
// section and ends. Does nothing in the parent.
static void reset(void) {
    if (!g_in_boot) return;
    copy_retained(g_rtc, g_retained_start);
    fflush(NULL);
    _exit(0);
}

// Starts a boot that runs up to, but not including, app_main's
// error_report_init(). True in the boot's own process.
static bool boot(esp_reset_reason_t reason) {
    reset();
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        g_in_boot = true;
        copy_retained(g_retained_start, g_rtc);
        g_reset_reason = reason;
        g_now_us = 0;
        g_timer_started = false;
        return true;
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed");
    // The boot already said what went wrong.
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) exit(1);
    return false;
}

// A boot that reached error_report_init() in app_main.
static bool started(esp_reset_reason_t reason) {
    if (!boot(reason)) return false;
    g_timer_started = true;
    error_report_init();
    return true;
}

// Power comes back: what RTC memory holds is undefined.
static void power_on(void) { memset(g_rtc, 0xa5, g_rtc_size); }

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

static void expect_queued(const char *label) {
    char *json = error_report_pending_json();
    CHECK(json, "%s: expected a queued report", label);
    free(json);
}

static void expect_nothing_queued(void) {
    char *json = error_report_pending_json();
    CHECK(!json, "unexpected queued report: %s", json);
}

static void test_reset_without_a_record(void) {
    if (started(ESP_RST_BROWNOUT)) emit_report("no_record");
}

static void test_normal_resets_queue_nothing(void) {
    static const esp_reset_reason_t normal[] = {
        ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
        ESP_RST_DEEPSLEEP, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG,
    };
    for (size_t i = 0; i < sizeof(normal) / sizeof(normal[0]); i++) {
        if (started(normal[i])) expect_nothing_queued();
    }
}

static void test_abnormal_resets_in_a_row(void) {
    static const esp_reset_reason_t abnormal[] = {
        ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
        ESP_RST_BROWNOUT, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP,
    };
    started(ESP_RST_POWERON);
    for (size_t i = 0; i < sizeof(abnormal) / sizeof(abnormal[0]); i++) {
        if (started(abnormal[i])) emit_report("in_a_row");
    }
    // A normal reset ends the run.
    started(ESP_RST_SW);
    if (started(ESP_RST_PANIC)) emit_report("after_normal_reset");
}

static void test_panic_summary(void) {
    static const uint8_t elf_sha[] = {0xa1, 0xb2, 0xc3, 0xd4,
                                      0xe5, 0xf6, 0x07, 0x18, 0x99};
    snprintf(g_app.version, sizeof(g_app.version), "1.0.9");
    memcpy(g_app.app_elf_sha256, elf_sha, sizeof(elf_sha));
    if (started(ESP_RST_POWERON)) {
        g_now_us = 5321999999LL;
        error_report_note_panic((const void *)(uintptr_t)0x42012345u,
                                (const void *)(uintptr_t)0x4200a1b2u,
                                "Load access fault", NULL);
        // A fault inside the panic handler must not replace the first one.
        error_report_note_panic((const void *)(uintptr_t)0x40000000u,
                                (const void *)(uintptr_t)0x40000004u,
                                "Illegal instruction", NULL);
    }

    // The bootloader rolled back: the report still names what crashed.
    snprintf(g_app.version, sizeof(g_app.version), "1.0.8");
    g_app.app_elf_sha256[0] = 0x0f;
    if (started(ESP_RST_PANIC)) emit_report("panic");

    // The panic detail belonged to that boot only.
    if (started(ESP_RST_WDT)) emit_report("after_panic");
}

static void test_abort_detail_is_bounded_and_printable(void) {
    char detail[400];
    memset(detail, 'x', sizeof(detail) - 1);
    detail[sizeof(detail) - 1] = '\0';
    memcpy(detail, "assert \"a\"\r\n\x01\xff", 14);

    if (started(ESP_RST_POWERON)) {
        g_now_us = 7 * 1000000LL;
        error_report_note_panic(NULL, NULL, "abort", detail);
    }
    if (started(ESP_RST_PANIC)) emit_report("abort");
}

static void test_panic_before_init(void) {
    snprintf(g_app.version, sizeof(g_app.version), "1.0.9");
    started(ESP_RST_POWERON);

    // An update restarts into 1.1.0, which aborts in early startup (PSRAM
    // didn't start) before esp_timer runs: the abort has nothing to do with
    // 1.0.9's record, and must not touch esp_timer.
    snprintf(g_app.version, sizeof(g_app.version), "1.1.0");
    if (boot(ESP_RST_SW)) {
        error_report_note_panic(NULL, NULL, "abort", "PSRAM init failed");
    }
    if (started(ESP_RST_PANIC)) emit_report("early_abort");

    // A fault later in startup, still before error_report_init().
    started(ESP_RST_SW);
    if (boot(ESP_RST_SW)) {
        g_timer_started = true;
        g_now_us = 2 * 1000000LL;
        error_report_note_panic((const void *)(uintptr_t)0x42001000u, NULL,
                                "Load access fault", NULL);
    }
    if (started(ESP_RST_PANIC)) emit_report("startup_panic");
}

static void test_crashes_before_init_still_count(void) {
    started(ESP_RST_POWERON);
    if (started(ESP_RST_SW)) {
        error_report_note_panic((const void *)(uintptr_t)0x42006000u, NULL,
                                "Load access fault", NULL);
    }
    // The next boot faults before error_report_init(), twice over (a fault
    // inside the panic handler), and the one after that does too.
    if (boot(ESP_RST_PANIC)) {
        error_report_note_panic(NULL, NULL, "abort", "PSRAM init failed");
        error_report_note_panic(NULL, NULL, "abort", "again");
    }
    if (started(ESP_RST_PANIC)) emit_report("crash_then_early");
    if (started(ESP_RST_SW)) {
        error_report_note_panic((const void *)(uintptr_t)0x42007000u, NULL,
                                "Load access fault", NULL);
    }
    if (boot(ESP_RST_PANIC)) {
        error_report_note_panic(NULL, NULL, "abort", "PSRAM init failed");
    }
    if (boot(ESP_RST_PANIC)) {
        error_report_note_panic(NULL, NULL, "abort", "PSRAM init failed");
    }
    if (started(ESP_RST_PANIC)) emit_report("crash_then_two_early");
}

static void test_report_waits_across_restarts(void) {
    static const uint8_t elf_sha[] = {0x12, 0x34, 0x56, 0x78,
                                      0x9a, 0xbc, 0xde, 0xf0};
    snprintf(g_app.version, sizeof(g_app.version), "1.0.9");
    memcpy(g_app.app_elf_sha256, elf_sha, sizeof(elf_sha));
    if (started(ESP_RST_POWERON)) {
        g_now_us = 42 * 1000000LL;
        error_report_note_panic((const void *)(uintptr_t)0x42002000u, NULL,
                                "Store access fault", NULL);
    }
    // Setup completes and the board restarts before it registers, then
    // sleeps and wakes, still before sending.
    if (started(ESP_RST_PANIC)) expect_queued("before_pairing_restart");
    if (started(ESP_RST_SW)) expect_queued("after_pairing_restart");
    if (started(ESP_RST_DEEPSLEEP)) emit_report("unsent");
    if (started(ESP_RST_SW)) expect_nothing_queued();

    // Those restarts ended the run of crashes, and a newer crash replaces a
    // report still waiting.
    if (started(ESP_RST_SW)) {
        error_report_note_panic((const void *)(uintptr_t)0x42003000u, NULL,
                                "Load access fault", NULL);
    }
    started(ESP_RST_PANIC);
    if (started(ESP_RST_SW)) {
        error_report_note_panic((const void *)(uintptr_t)0x42004000u, NULL,
                                "Illegal instruction", NULL);
    }
    if (started(ESP_RST_PANIC)) emit_report("replaced");

    // Power loss discards one, even if RTC memory happened to keep it.
    if (started(ESP_RST_SW)) {
        error_report_note_panic((const void *)(uintptr_t)0x42005000u, NULL,
                                "Load access fault", NULL);
    }
    started(ESP_RST_PANIC);
    if (started(ESP_RST_POWERON)) expect_nothing_queued();
}

static void run(void (*test)(void)) {
    power_on();
    test();
    reset();
}

int main(void) {
    g_rtc_size = (size_t)(g_retained_end - g_retained_start);
    CHECK(g_rtc_size > 0, "no retained section");
    g_rtc = mmap(NULL, g_rtc_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(g_rtc != MAP_FAILED, "mmap failed");

    run(test_reset_without_a_record);
    run(test_normal_resets_queue_nothing);
    run(test_abnormal_resets_in_a_row);
    run(test_panic_summary);
    run(test_abort_detail_is_bounded_and_printable);
    run(test_panic_before_init);
    run(test_crashes_before_init_still_count);
    run(test_report_waits_across_restarts);
    return 0;
}
