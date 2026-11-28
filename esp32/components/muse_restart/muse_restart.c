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

#include "muse_restart.h"
#include "muse_crashrec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "muse_retained.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "esp_private/esp_sys_event_internal.h"
#endif

_Static_assert((int)ESP_RST_SW == (int)MUSE_RST_SW && (int)ESP_RST_WDT == (int)MUSE_RST_WDT &&
                   (int)ESP_RST_TASK_WDT == (int)MUSE_RST_TASK_WDT && (int)ESP_RST_INT_WDT == (int)MUSE_RST_INT_WDT &&
                   (int)ESP_RST_PANIC == (int)MUSE_RST_PANIC,
               "muse_restart.h's reasons are esp_reset_reason_t's");

static const char *TAG = "muse_restart";

static MUSE_RETAINED_ATTR muse_restart_mark_t s_mark;
static MUSE_RETAINED_ATTR muse_restart_mark_t s_fail;   /* a restart out of a failure (S31) */

/* Read once: the marker is taken (and cleared) by the first look at this boot's reason. */
static bool s_read, s_marked;
static muse_restart_why_t s_why;
static uint32_t s_core, s_fail_core;
static int s_chip_reason, s_fail_reason;

esp_reset_reason_t __real_esp_reset_reason(void);

static void read_boot(void)
{
    if (s_read) {
        return;
    }
    s_chip_reason = (int)__real_esp_reset_reason();
    s_marked = muse_restart_mark_valid(&s_mark);
    s_why = s_marked ? (muse_restart_why_t)s_mark.why : MUSE_RESTART_OTHER;
    s_core = s_marked ? s_mark.core : 0;
    s_fail_reason = muse_restart_fail_reason(&s_fail);
    s_fail_core = s_fail.core;
    muse_restart_mark_clear(&s_mark);   /* this boot's only: a later crash isn't covered by it */
    muse_restart_mark_clear(&s_fail);
    s_read = true;
}

/*
 * Every caller of esp_reset_reason() in the image (ESP-Iris's crash-loop
 * counter and crash report, the battery log, bug reports) gets the reason
 * with the marker taken into account (CMakeLists.txt wraps the symbol).
 */
esp_reset_reason_t __wrap_esp_reset_reason(void)
{
    return (esp_reset_reason_t)muse_restart_boot_reason();
}

int muse_restart_boot_reason(void)
{
    read_boot();
    return muse_restart_effective_reason2(s_chip_reason, s_marked, s_fail_reason);
}

bool muse_restart_boot_intentional(muse_restart_why_t *why, uint32_t *core)
{
    read_boot();
    if (why) {
        *why = s_why;
    }
    if (core) {
        *core = s_core;
    }
    return s_marked && (s_chip_reason == ESP_RST_SW || s_chip_reason == ESP_RST_WDT);
}

void muse_restart_boot_describe(char *buf, size_t cap)
{
    read_boot();
    int eff = muse_restart_effective_reason2(s_chip_reason, s_marked, s_fail_reason);
    int raw0 = (int)esp_rom_get_reset_reason(0);
#if SOC_CPU_CORES_NUM > 1
    int raw1 = (int)esp_rom_get_reset_reason(1);
#else
    int raw1 = -1;
#endif
    if (s_marked && (s_chip_reason == ESP_RST_SW || s_chip_reason == ESP_RST_WDT)) {
        snprintf(buf, cap, "intentional restart (%s, from core %u)%s; chip: %s, raw 0x%02x/0x%02x",
                 muse_restart_why_name(s_why), (unsigned)s_core,
                 s_chip_reason == ESP_RST_WDT ? ", completed by esp_restart()'s RTC watchdog" : "",
                 muse_restart_reason_name(s_chip_reason), raw0, raw1);
    } else if (s_fail_reason && eff == s_fail_reason) {
        snprintf(buf, cap, "%s (restart out of a failure, from core %u%s); chip: %s, raw 0x%02x/0x%02x",
                 muse_restart_reason_name(eff), (unsigned)s_fail_core,
                 eff == ESP_RST_SW ? ": esp_restart()" : ", the panic handler's restart",
                 muse_restart_reason_name(s_chip_reason), raw0, raw1);
    } else {
        snprintf(buf, cap, "%s%s; raw 0x%02x/0x%02x", muse_restart_reason_name(eff),
                 s_marked ? " (a restart was asked for, but this is something else)" : "", raw0, raw1);
    }
}

static const char *s_log_tag;

static void log_boot_now(void *arg)
{
    (void)arg;
    char why[192];
    muse_restart_boot_describe(why, sizeof(why));
    ESP_LOGI(s_log_tag, "boot: %s", why);
    char crash[200];
    if (muse_crashrec_this_boot(crash, sizeof(crash))) {
        ESP_LOGW(s_log_tag, "crash: %s", crash);   /* what the restart came out of */
    }
#if CONFIG_IDF_TARGET_ESP32S31
    muse_faultlog_report(s_log_tag);   /* after a panic: when, where, and the flash operations around it */
#endif
}

void muse_restart_log_boot(const char *tag)
{
    muse_crashrec_boot();
#if CONFIG_IDF_TARGET_ESP32S31
    muse_faultlog_init();
#endif
    s_log_tag = tag;
    log_boot_now(NULL);
    const esp_timer_create_args_t args = { .callback = log_boot_now, .name = "boot_log" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 15 * 1000 * 1000);
    }
}

esp_err_t esp_iris_mark_planned_restart(void) __attribute__((weak));

static void (*s_pre)(void);
static volatile bool s_pre_ran;

void muse_restart_set_pre(void (*fn)(void))
{
    s_pre = fn;
}

static struct {
    const char *name;
    uint32_t ms;
} s_marks[24];
static volatile uint32_t s_nmarks;

void muse_boot_mark(const char *name)
{
    uint32_t i = __atomic_fetch_add(&s_nmarks, 1, __ATOMIC_RELAXED);
    if (i < sizeof(s_marks) / sizeof(s_marks[0])) {
        s_marks[i].ms = esp_log_timestamp();
        s_marks[i].name = name;
    }
}

void muse_boot_marks_log(const char *tag)
{
    char line[420];
    size_t n = 0, count = s_nmarks < 24 ? s_nmarks : 24;
    line[0] = 0;
    for (size_t i = 0; i < count && n + 2 < sizeof(line); i++) {
        if (!s_marks[i].name) continue;
        int w = snprintf(line + n, sizeof(line) - n, "%s%s %u", n ? ", " : "", s_marks[i].name, (unsigned)s_marks[i].ms);
        if (w < 0) break;
        n += (size_t)w;
    }
    ESP_LOGI(tag, "boot marks (ms since reset): %s", line);
}

void muse_restart(muse_restart_why_t why)
{
    if (s_pre && !s_pre_ran) {
        s_pre_ran = true;
        s_pre();   /* close links cleanly first */
    }
    if (esp_iris_mark_planned_restart) {
        (void)esp_iris_mark_planned_restart();   /* ESP-Iris's own record (ESP-Mosaico) */
    }
    ESP_LOGI(TAG, "restarting (%s)", muse_restart_why_name(why));
    muse_restart_mark_set(&s_mark, why, (uint32_t)esp_cpu_get_core_id());
#if CONFIG_IDF_TARGET_ESP32S31
    /*
     * esp_restart()'s own steps (the shutdown handlers: radios, USB, ...),
     * ending in the ROM's whole-system software reset instead of
     * esp_restart_noos()'s CPU reset. On the S31 with ESP-IDF master
     * (7b9cc1ac) that CPU reset doesn't get the chip booting while the app
     * runs (from either core, in or out of an RPC handler: raw reset reason
     * 0x10, the RTC watchdog's system reset 1 s later; esp_restart_noos()
     * arms it as the restart's safety net). Its module reset before the CPU
     * reset is still a TODO there (IDF-14841: on the P4 it stops the DMAs and
     * resets MSPI, timers and UARTs first). The system reset resets every
     * module, and reads as a software reset (0x03); RTC memory, so the
     * marker, survives it. README.md has the measurements.
     */
    esp_sys_event_trigger(ESP_SYS_EVENT_SHUTDOWN, NULL);
#if (CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE)
    vTaskPreemptionDisable(NULL);
#else
    vTaskSuspendAll();
#endif
    esp_rom_software_reset_system();
    for (;;) {
    }
#else
    esp_restart();
#endif
}

#if CONFIG_IDF_TARGET_ESP32S31
#include "esp_private/system_internal.h"

/*
 * Every restart out of a failure (the panic handler's, a plain esp_restart())
 * ends in esp_restart_noos(), whose CPU reset doesn't get the S31 booting: the
 * RTC watchdog it arms resets the chip 1 s later and the boot reads as a
 * watchdog reset, so a panic (with its core dump saved) looked like a hang.
 * CMakeLists.txt wraps it: the failure marker with the panic handler's reason,
 * then the ROM's system reset, as muse_restart() ends. Called with interrupts
 * off and, in a panic, the other core stalled; IRAM only, no flash or PSRAM.
 */
void __real_esp_restart_noos(void);

void IRAM_ATTR __attribute__((noreturn)) __wrap_esp_restart_noos(void)
{
    esp_cpu_intr_disable(0xFFFFFFFFu);
    muse_restart_fail_set(&s_fail, (int)esp_reset_reason_get_hint(), (uint32_t)esp_cpu_get_core_id());
    esp_rom_software_reset_system();
    for (;;) {
    }
}
#endif

typedef struct {
    muse_restart_why_t why;
    uint32_t delay_ms;
} later_t;

static void later_task(void *arg)
{
    later_t l = *(later_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(l.delay_ms));
    muse_restart(l.why);
}

static void failure_task(void *arg)
{
    uint32_t v = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(v >> 1));
    if (v & 1) {
        ESP_LOGW(TAG, "bench: a panic on purpose");
        abort();
    }
    ESP_LOGW(TAG, "bench: esp_restart() on purpose");
    esp_restart();
}

static void hang_task(void *arg)
{
    uint32_t v = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(v >> 2));
    if ((v & 3) == MUSE_HANG_INT) {
        ESP_LOGW(TAG, "bench: interrupts off and spinning on core %d (the interrupt watchdog, 1.5 s)",
                 (int)esp_cpu_get_core_id());
        vTaskDelay(pdMS_TO_TICKS(50));   /* the line out first */
        portDISABLE_INTERRUPTS();
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
        }
    }
    ESP_LOGW(TAG, "bench: spinning at top priority on core %d, starving its idle task (the task watchdog, 15 s)",
             (int)esp_cpu_get_core_id());
    vTaskDelay(pdMS_TO_TICKS(50));
    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}

/* Whether the watchdog that catches `kind` is on in this build: without it the hang would last until a reset. */
static bool hang_caught(muse_hang_t kind)
{
#if CONFIG_ESP_TASK_WDT_EN && CONFIG_ESP_TASK_WDT_PANIC && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    if (kind == MUSE_HANG_TASK) {
        return true;
    }
#endif
#if CONFIG_ESP_INT_WDT
    if (kind == MUSE_HANG_INT) {
        return true;
    }
#endif
    return false;
}

bool muse_restart_test_hang(muse_hang_t kind, uint32_t delay_ms)
{
    if ((kind != MUSE_HANG_TASK && kind != MUSE_HANG_INT) || !hang_caught(kind)) {
        return false;
    }
    uintptr_t v = ((uintptr_t)delay_ms << 2) | (uintptr_t)kind;
    return xTaskCreatePinnedToCore(hang_task, kind == MUSE_HANG_INT ? "hang_int" : "hang_task", 2048, (void *)v,
                                   configMAX_PRIORITIES - 1, NULL, 0) == pdPASS;
}

bool muse_restart_hang_word(const char *line, char *out, size_t n)
{
    if (strncmp(line, "hang", 4) != 0 || (line[4] && line[4] != '=')) {
        return false;
    }
    muse_hang_t kind = !strcmp(line, "hang=task") ? MUSE_HANG_TASK : !strcmp(line, "hang=int") ? MUSE_HANG_INT : 0;
    if (!kind) {
        snprintf(out, n, "hang: hang=task (the task watchdog) or hang=int (the interrupt watchdog)");
        return true;
    }
    if (!hang_caught(kind)) {
        snprintf(out, n, "hang %s refused: that watchdog is off in this build", kind == MUSE_HANG_INT ? "int" : "task");
        return true;
    }
    bool ok = muse_restart_test_hang(kind, 300);
    snprintf(out, n, "hang %s %s", kind == MUSE_HANG_INT ? "int" : "task",
             !ok ? "failed" : kind == MUSE_HANG_INT ? "in 0.3 s, reset ~1.5 s later" : "in 0.3 s, reset ~15 s later");
    return true;
}

bool muse_restart_test_failure(bool panic, uint32_t delay_ms)
{
    uintptr_t v = ((uintptr_t)delay_ms << 1) | (panic ? 1u : 0u);
    return xTaskCreate(failure_task, "muse_fail", 3072, (void *)v, configMAX_PRIORITIES - 2, NULL) == pdPASS;
}

bool muse_restart_later(muse_restart_why_t why, int core, uint32_t delay_ms)
{
    static later_t l;
    l.why = why;
    l.delay_ms = delay_ms;
    BaseType_t c = core >= 0 && core < portNUM_PROCESSORS ? core : tskNO_AFFINITY;
    return xTaskCreatePinnedToCore(later_task, "muse_restart", 3072, &l, configMAX_PRIORITIES - 2, NULL, c) == pdPASS;
}
