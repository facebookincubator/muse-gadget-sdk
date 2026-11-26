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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Restarts the firmware means to make (a console restart, an OTA, a setup
 * reset), told apart from the ones it doesn't.
 *
 * On the ESP32-S31 (the ESP-Mosaico, ESP-IDF master) esp_restart() can come
 * back as a watchdog reset (ESP_RST_WDT): esp_restart_noos() arms the RTC
 * watchdog for 1 s as the restart's own safety net, and when the chip's CPU
 * reset doesn't get it booting (README.md) that watchdog does. Reported as is,
 * every software restart would read as a watchdog reset, hiding real ones, and
 * ESP-Iris's crash-loop counter would count each as a failed boot (three
 * within 30 s and it boots the recovery firmware). So muse_restart() leaves a
 * marker in RTC memory first (it survives every reset but a power loss), and
 * the next boot's reason takes it into account: an RTC watchdog reset right
 * after a marked restart is that restart; any other watchdog reset is real.
 */
typedef enum {
    MUSE_RESTART_OTHER = 1,
    MUSE_RESTART_CONSOLE,    /* the bench console's restart */
    MUSE_RESTART_OTA,        /* a firmware update done */
    MUSE_RESTART_SETUP,      /* pairing done, setup reset, a new SDK token */
    MUSE_RESTART_SAFE_OFF,   /* out of SAFE mode by the user's key hold (muse_bootguard.h) */
    MUSE_RESTART_SAFE_ON,    /* into SAFE mode by the user's key hold after boot (muse_bootguard.h) */
    MUSE_RESTART_WHY_COUNT,
} muse_restart_why_t;

/* esp_reset_reason_t's values (esp_system.h), for the code that runs on the host too. */
enum {
    MUSE_RST_UNKNOWN = 0,
    MUSE_RST_POWERON = 1,
    MUSE_RST_SW = 3,
    MUSE_RST_PANIC = 4,
    MUSE_RST_INT_WDT = 5,
    MUSE_RST_TASK_WDT = 6,
    MUSE_RST_WDT = 7,
    MUSE_RST_DEEPSLEEP = 8,
};

/* The marker, in RTC memory: checked against its own complement, so what a
 * power-on leaves there never passes. */
typedef struct {
    uint32_t magic;
    uint32_t why;     /* muse_restart_why_t */
    uint32_t core;    /* the core that called muse_restart() */
    uint32_t check;   /* ~(magic ^ why ^ core) */
} muse_restart_mark_t;

#define MUSE_RESTART_MAGIC 0x4D525354u   /* "MRST" */

void muse_restart_mark_set(muse_restart_mark_t *m, muse_restart_why_t why, uint32_t core);
bool muse_restart_mark_valid(const muse_restart_mark_t *m);
void muse_restart_mark_clear(muse_restart_mark_t *m);

/*
 * The other marker: a restart out of a failure on the S31, where
 * esp_restart_noos() (a panic's restart, a plain esp_restart()) comes back as
 * an RTC watchdog reset 1 s later too (raw 0x10), so a panic reads as
 * "watchdog". The wrapped esp_restart_noos() (muse_restart.c) leaves this
 * marker with the reason the panic handler set (panic, task or interrupt
 * watchdog; none for esp_restart(): software) and resets the system the way
 * muse_restart() does; the next boot reports that reason. Same layout, its own
 * magic; why holds an esp_reset_reason_t. Inline: it runs in the panic
 * handler, from IRAM.
 */
#define MUSE_RESTART_FAIL_MAGIC 0x4D464149u   /* "MFAI" */

static inline void muse_restart_fail_set(muse_restart_mark_t *m, int reason, uint32_t core)
{
    m->magic = MUSE_RESTART_FAIL_MAGIC;
    m->why = (uint32_t)reason;
    m->core = core;
    m->check = ~(m->magic ^ m->why ^ m->core);
}

/* The reason a valid failure marker holds (software when the handler set none), else 0. */
int muse_restart_fail_reason(const muse_restart_mark_t *m);

/*
 * The reason this boot reports, from the chip's (esp_reset_reason()) and
 * whether the last boot left a marker: SW for an intentional restart, whether
 * the chip said SW or RTC watchdog; the chip's reason otherwise. Task and
 * interrupt watchdogs, panics and the rest are never masked (they fire before
 * esp_restart() arms its own watchdog, or have nothing to do with it).
 */
int muse_restart_effective_reason(int chip_reason, bool marked);

/* The same with a failure marker's reason too (0: none): after a software or
 * RTC watchdog reset, the failure the marker names. */
int muse_restart_effective_reason2(int chip_reason, bool marked, int fail_reason);

/* Short names for logs: "software", "watchdog", ... */
const char *muse_restart_reason_name(int reason);
const char *muse_restart_why_name(muse_restart_why_t why);

/* ---- on the device ---------------------------------------------------------- */

/* Leaves the marker (and tells ESP-Iris it's planned, on boards with it), then
 * esp_restart() (on the S31, its shutdown and a whole-system reset: see
 * muse_restart.c). Only for restarts that are the plan: a reboot to get out of a
 * failure (a stuck panel, a BLE stack that won't stop) stays a plain
 * esp_restart(), so a loop of them still counts as one. */
void muse_restart(muse_restart_why_t why) __attribute__((noreturn));
/* A step muse_restart() runs first, once (mg_ble closes the phone's link
 * cleanly so it sees the restart at once); it must return within ~300 ms. */
void muse_restart_set_pre(void (*fn)(void));

/* The same from a task pinned to that core, delay_ms later (a caller that must
 * return first, such as an RPC handler that answers). False if it can't start. */
bool muse_restart_later(muse_restart_why_t why, int core, uint32_t delay_ms);

/* Bench: a failure on purpose, delay_ms later from a task of its own (after a
 * console RPC answers): abort() (a panic, with its core dump) or a plain
 * esp_restart(), to check how the next boot reports them. */
bool muse_restart_test_failure(bool panic, uint32_t delay_ms);

/* Bench (the watchdogs' hardware check, the consoles' hang=task|int, automation): a
 * hang on purpose, delay_ms later, from a task pinned to core 0, for the
 * watchdogs to catch. TASK spins at the highest priority with interrupts on,
 * starving core 0's idle task: the task watchdog panics (15 s). INT spins with
 * interrupts off: the interrupt watchdog panics (1.5 s). Each leaves its crash
 * record and counts toward the boot guard like a real one. Refused (false) when
 * the build has that watchdog off. */
typedef enum { MUSE_HANG_TASK = 1, MUSE_HANG_INT = 2 } muse_hang_t;
bool muse_restart_test_hang(muse_hang_t kind, uint32_t delay_ms);
/* The consoles' word: "hang=task" or "hang=int" (true if it's the word; out
 * gets the answer, "hang task in 15 s" or the usage). */
bool muse_restart_hang_word(const char *line, char *out, size_t n);

/* This boot's reason, the marker taken into account (read once, at the first call). */
int muse_restart_boot_reason(void);

/* What the last boot left: true if this boot began with an intentional restart. */
bool muse_restart_boot_intentional(muse_restart_why_t *why, uint32_t *core);

/* One line about how this boot began, for the boot log and status. */
void muse_restart_boot_describe(char *buf, size_t cap);

/* Logs it now on tag, and again 15 s later (a log capture that starts after
 * boot, such as ESP-Iris's on the ESP-Mosaico, gets it then). */
void muse_restart_log_boot(const char *tag);

/* Boot milestones (restart to connected, PERFORMANCE.md): records the log
 * timestamp (ms since reset, ROM and bootloader included) under a static
 * name, up to 24 per boot; muse_boot_marks_log() prints them on one line,
 * for boards whose early log isn't captured (the ESP-Mosaico's ESP-Iris). */
void muse_boot_mark(const char *name);
void muse_boot_marks_log(const char *tag);

/* S31 only (muse_faultlog.c): flash operations and the panic handler noted in
 * RTC memory; init arms it (muse_restart_log_boot), report logs the previous
 * boot's if it ended in a panic. */
void muse_faultlog_init(void);
void muse_faultlog_report(const char *tag);
/* This boot's last flash operations (the bench console's >flashops). */
void muse_faultlog_dump(const char *tag);

#ifdef __cplusplus
}
#endif
