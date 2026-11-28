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

/* The crash record's pure part (muse_crashrec.h): no ESP-IDF, no library
 * calls in what the panic path uses (sum, seal), so it can run there. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* strnlen: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include "muse_crashrec.h"

#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define CREC_IRAM IRAM_ATTR
#else
#define CREC_IRAM
#endif

uint32_t CREC_IRAM muse_crashrec_sum(const muse_crashrec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    size_t n = offsetof(muse_crashrec_t, check);
    uint32_t h = 2166136261u;   /* FNV-1a over everything before the check */
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return ~h;
}

bool muse_crashrec_valid(const muse_crashrec_t *r)
{
    return r->magic == MUSE_CRASHREC_MAGIC && r->check == muse_crashrec_sum(r) && r->kind <= MUSE_CRASH_FAULT &&
           r->fresh <= 1 && r->core < 8;
}

void CREC_IRAM muse_crashrec_seal(muse_crashrec_t *r)
{
    r->check = muse_crashrec_sum(r);
}

void muse_crashrec_reset(muse_crashrec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->magic = MUSE_CRASHREC_MAGIC;
    muse_crashrec_seal(r);
}

uint32_t CREC_IRAM muse_crashrec_classify(int exception, bool twdt_isr, int hint)
{
    if (twdt_isr || hint == 6) {
        return MUSE_CRASH_TASK_WDT;
    }
    if (hint == 5 || exception == 1) {
        return MUSE_CRASH_INT_WDT;
    }
    return exception >= 0 && exception + 1 <= MUSE_CRASH_FAULT ? (uint32_t)exception + 1 : MUSE_CRASH_FAULT;
}

const char *muse_crashrec_kind_name(uint32_t kind)
{
    switch (kind) {
    case MUSE_CRASH_DEBUG: return "debug exception";
    case MUSE_CRASH_INT_WDT: return "interrupt watchdog";
    case MUSE_CRASH_TASK_WDT: return "task watchdog";
    case MUSE_CRASH_ABORT: return "abort";
    case MUSE_CRASH_FAULT: return "CPU fault";
    default: return "none";
    }
}

void muse_crashrec_add_starved(muse_crashrec_t *r, unsigned *seen, const char *piece)
{
    unsigned i = (*seen)++;
    if (i == 0 || !piece) {
        return;   /* the caption */
    }
    /* Then, per user: "\n - ", the name, " (CPU n)". */
    if ((i - 1) % 3 != 1) {
        return;
    }
    size_t have = strnlen(r->starved, sizeof(r->starved));
    size_t room = sizeof(r->starved) - 1 - have;
    if (have && room >= 2) {
        memcpy(r->starved + have, ", ", 2);
        have += 2;
        room -= 2;
    } else if (have) {
        return;
    }
    size_t n = strnlen(piece, MUSE_CRASHREC_NAME);
    if (n > room) {
        n = room;
    }
    memcpy(r->starved + have, piece, n);
    r->starved[have + n] = '\0';
}

static const char *name_or_dash(const char *s, size_t cap, char *buf)
{
    size_t n = strnlen(s, cap);
    if (!n) {
        return "-";
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    return buf;
}

void muse_crashrec_describe(const muse_crashrec_t *r, char *out, size_t cap)
{
    char t0[MUSE_CRASHREC_NAME + 1], t1[MUSE_CRASHREC_NAME + 1], st[MUSE_CRASHREC_STARVED + 1];
    const char *a = name_or_dash(r->running[0], MUSE_CRASHREC_NAME, t0);
    const char *b = name_or_dash(r->running[1], MUSE_CRASHREC_NAME, t1);
    size_t sn = strnlen(r->starved, MUSE_CRASHREC_STARVED);
    memcpy(st, r->starved, sn);
    st[sn] = '\0';
    long long ms = (long long)(r->up_us / 1000);
    snprintf(out, cap, "%s on core %u, %lld.%01lld s up, pc 0x%08lx; running %s / %s%s%s (crash %u since power-on)",
             muse_crashrec_kind_name(r->kind), (unsigned)r->core, ms / 1000, (ms % 1000) / 100,
             (unsigned long)r->pc, a, b, sn ? "; didn't reset the task watchdog: " : "", st, (unsigned)r->count);
}
