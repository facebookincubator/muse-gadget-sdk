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

/*
 * The crash record: a panic, the interrupt
 * watchdog or the task watchdog leaves a breadcrumb in RTC memory (every
 * reset but a power loss keeps it) that the next boot logs, beside its reset
 * reason, and the BLE console's "guard?" repeats: what happened (the
 * exception kind), on which core and at which pc, how long the boot had been
 * up, the task running on each core, and for the task watchdog the tasks or
 * watchdog users that didn't reset it in time (esp_task_wdt_isr_user_handler,
 * muse_crashrec.c). The record is checked against a checksum, so a power-on's
 * leftovers never pass. Every board, Link-only builds too.
 *
 * The pure part (muse_crashrec_core.c) has no ESP-IDF in it so the host tests
 * run it (tests/test_watchdogs.py).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_CRASHREC_MAGIC 0x43524543u   /* "CREC" */
#define MUSE_CRASHREC_NAME 16
#define MUSE_CRASHREC_STARVED 64

/* What the panic was (panic_info_t's exception, plus one: 0 is none). */
typedef enum {
    MUSE_CRASH_NONE = 0,
    MUSE_CRASH_DEBUG,        /* a debug exception (a watchpoint, a breakpoint) */
    MUSE_CRASH_INT_WDT,      /* the interrupt watchdog */
    MUSE_CRASH_TASK_WDT,     /* the task watchdog */
    MUSE_CRASH_ABORT,        /* abort(), a failed assert */
    MUSE_CRASH_FAULT,        /* a CPU fault (a bad load or store, an illegal instruction) */
} muse_crash_kind_t;

typedef struct {
    uint32_t magic;
    uint32_t fresh;          /* 1: written by a crash no boot has reported yet */
    uint32_t count;          /* crashes recorded since the last power-on */
    uint32_t kind;           /* muse_crash_kind_t */
    uint32_t core;
    uint32_t pc;
    int64_t up_us;           /* how long that boot had been up */
    char running[2][MUSE_CRASHREC_NAME];       /* the task on each core then */
    char starved[MUSE_CRASHREC_STARVED];       /* the task watchdog: who didn't reset it ("a, b") */
    uint32_t check;          /* muse_crashrec_sum() */
} muse_crashrec_t;

uint32_t muse_crashrec_sum(const muse_crashrec_t *r);
bool muse_crashrec_valid(const muse_crashrec_t *r);
void muse_crashrec_seal(muse_crashrec_t *r);
/* A power-on (or anything that fails the check): an empty record. */
void muse_crashrec_reset(muse_crashrec_t *r);
const char *muse_crashrec_kind_name(uint32_t kind);

/* The kind to record (the panic wrapper's; pure, host-tested). exception:
 * panic_info_t's (0 debug, 1 interrupt watchdog, 2 task watchdog, 3 abort,
 * 4 fault); twdt_isr: the task watchdog's ISR ran this boot (its user
 * handler, just before the panic); hint: esp_reset_reason_get_hint() at the
 * panic (5 interrupt watchdog, 6 task watchdog, as esp_reset_reason_t). The
 * task watchdog's panic is a simulated abort that ESP-IDF's panic handler
 * reports as a fault (task_wdt_timeout_abort, on the C6: "CPU fault on
 * core 0" beside the boot line's "task watchdog"), so the ISR's mark or the
 * hint it sets first decides: a watchdog is never told as a fault. */
uint32_t muse_crashrec_classify(int exception, bool twdt_isr, int hint);
/* esp_task_wdt_print_triggered_tasks()'s pieces, in order (its caption, then
 * "\n - ", a name, " (CPU n)" for each): the names, joined with ", ",
 * truncated to fit. `seen` counts the pieces (0 before the first). */
void muse_crashrec_add_starved(muse_crashrec_t *r, unsigned *seen, const char *piece);
/* One line: "task watchdog on core 0, 312.4 s up, pc 0x4200abcd; running
 * mg_face / IDLE1; didn't reset the task watchdog: IDLE0 (crash 2 since
 * power-on)". */
void muse_crashrec_describe(const muse_crashrec_t *r, char *out, size_t cap);

/* ---- on the device ---------------------------------------------------------- */

/* Reads the record at boot (muse_restart_log_boot): the previous boot's crash,
 * if it ended in one, and marks it reported. */
void muse_crashrec_boot(void);
/* This boot began with a crash: its line in out (true); else false. */
bool muse_crashrec_this_boot(char *out, size_t cap);
/* The last crash since power-on, this boot's or an earlier one's (true), for
 * "guard?" and status; false if none. */
bool muse_crashrec_last(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
