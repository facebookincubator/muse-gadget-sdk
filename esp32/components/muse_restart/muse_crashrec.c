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
 * The crash record on the device (muse_crashrec.h): the panic handler's
 * wrapper (CMakeLists.txt wraps esp_panic_handler on every target) and the
 * task watchdog's user handler write it; the next boot reads and reports it.
 *
 * The panic path may run with the caches off (a fault during a flash
 * operation): it's IRAM, touches only RTC and internal memory, and reads task
 * names only while the cache is on.
 */
#include "muse_crashrec.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_private/system_internal.h"   /* esp_reset_reason_get_hint() */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "muse_retained.h"
#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif

#include "muse_restart.h"

static MUSE_RETAINED_ATTR muse_crashrec_t s_rtc;
static muse_crashrec_t s_last;      /* the last crash since power-on, read at boot */
static bool s_have_last;
static bool s_this_boot;            /* ...and this boot began with it */

static inline void IRAM_ATTR name_copy(char *dst, const char *src)
{
    int i = 0;
    for (; src && i < MUSE_CRASHREC_NAME - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < MUSE_CRASHREC_NAME; i++) {
        dst[i] = 0;
    }
}

void muse_crashrec_boot(void)
{
    if (!muse_crashrec_valid(&s_rtc)) {
        muse_crashrec_reset(&s_rtc);   /* a power-on: nothing to report */
        return;
    }
    if (s_rtc.kind != MUSE_CRASH_NONE) {
        s_last = s_rtc;
        s_have_last = true;
        s_this_boot = s_rtc.fresh != 0;
    }
    s_rtc.fresh = 0;   /* reported by this boot; the record stays for "guard?" */
    s_rtc.starved[0] = '\0';
    muse_crashrec_seal(&s_rtc);
}

bool muse_crashrec_this_boot(char *out, size_t cap)
{
    if (!s_this_boot) {
        return false;
    }
    muse_crashrec_describe(&s_last, out, cap);
    return true;
}

bool muse_crashrec_last(char *out, size_t cap)
{
    if (!s_have_last) {
        return false;
    }
    muse_crashrec_describe(&s_last, out, cap);
    return true;
}

#if CONFIG_ESP_TASK_WDT_EN
/* The task watchdog's ISR, just before its panic: who didn't reset it. */
static unsigned s_pieces;

static void starved_piece(void *opaque, const char *msg)
{
    (void)opaque;
    muse_crashrec_add_starved(&s_rtc, &s_pieces, msg);
}

/* Set in the task watchdog's ISR, read by the panic that follows: its
 * simulated abort comes to the panic handler as a fault (muse_crashrec_classify). */
static DRAM_ATTR volatile bool s_twdt_isr;

void esp_task_wdt_isr_user_handler(void)
{
    s_twdt_isr = true;
    if (!muse_crashrec_valid(&s_rtc)) {
        return;
    }
    s_rtc.starved[0] = '\0';
    s_pieces = 0;
    esp_task_wdt_print_triggered_tasks(starved_piece, NULL, NULL);
    muse_crashrec_seal(&s_rtc);
}
#endif

void __real_esp_panic_handler(panic_info_t *info);

/* Another recorder's turn in the one panic wrapper the link allows, after the
 * crash record and before ESP-IDF's handler (main/error_report.c defines it
 * when CONFIG_HOMEHUB_ERROR_REPORT is on). Weak: nothing by default. */
void __attribute__((weak)) muse_crashrec_panic_hook(panic_info_t *info)
{
    (void)info;
}
#if CONFIG_IDF_TARGET_ESP32S31
void muse_faultlog_on_panic(panic_info_t *info);   /* muse_faultlog.c */
#endif

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    /* The first panic of a boot only (a panic inside the panic handler keeps
     * the first one's record). */
    if (s_rtc.magic == MUSE_CRASHREC_MAGIC && s_rtc.check == muse_crashrec_sum(&s_rtc) && !s_rtc.fresh) {
        s_rtc.fresh = 1;
        s_rtc.count++;
#if CONFIG_ESP_TASK_WDT_EN
        bool twdt = s_twdt_isr;
#else
        bool twdt = false;
#endif
        s_rtc.kind = muse_crashrec_classify((int)info->exception, twdt, (int)esp_reset_reason_get_hint());
        s_rtc.core = (uint32_t)info->core;
        s_rtc.pc = (uint32_t)(uintptr_t)info->addr;
        s_rtc.up_us = esp_timer_get_time();
        if (s_rtc.kind != MUSE_CRASH_TASK_WDT) {
            s_rtc.starved[0] = '\0';
        }
        bool names = spi_flash_cache_enabled();   /* task names may sit behind the cache */
        for (int c = 0; c < 2; c++) {
            TaskHandle_t t = NULL;
            if (names && c < CONFIG_FREERTOS_NUMBER_OF_CORES) {
                t = xTaskGetCurrentTaskHandleForCore(c);
            }
            name_copy(s_rtc.running[c], t ? pcTaskGetName(t) : "");
        }
        muse_crashrec_seal(&s_rtc);
    }
#if CONFIG_IDF_TARGET_ESP32S31
    muse_faultlog_on_panic(info);   /* the S31's PSRAM fault log (muse_faultlog.c) */
#endif
    muse_crashrec_panic_hook(info);
    __real_esp_panic_handler(info);
}
