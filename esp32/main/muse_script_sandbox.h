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
 * sandbox: sandboxed Lua 5.4 scripts for a single "scripting task".
 *
 * Portable C (host and ESP-IDF). Every function here must be called from the
 * one task that owns the runtime; nothing is thread-safe by design. Other tasks
 * talk to that task through a queue (see esp/script_task_esp.c).
 *
 * Each script gets its own lua_State with:
 *   - a memory budget enforced by the allocator (per script and in total),
 *   - a CPU budget per resume, enforced by a count hook plus a wall-clock
 *     deadline, with kills that pcall/xpcall/coroutine.resume can't swallow,
 *   - base/string/table/math/utf8/coroutine only (no io, os, debug, package,
 *     dofile, loadfile, string.dump; load is text-only; no __gc metamethods),
 *   - an event API: on(event, fn), every(ms, fn), after(ms, fn), cancel(id),
 *     wait(ms), now(), print(...), notify(text), stop(), and device.call(cmd,
 *     params) plus one sugar function per allowed device command
 *     (led.set{...}, audio.beep{...}, ...).
 */
#pragma once

/*
 * The Lua sandbox behind on-device scripts (muse_script.c): one lua_State per
 * script, a memory budget each, instruction budgets per run, and cooperative
 * scheduling (wait, every, after, on) on coroutines. Portable C: the host
 * tests build it too (tests/test_muse_script.py).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_runtime sb_runtime_t;

typedef struct {
    int64_t (*now_us)(void);                       /* monotonic */
    void *(*mem_realloc)(void *ptr, size_t size);  /* realloc semantics; on ESP: heap_caps_realloc(.., MALLOC_CAP_SPIRAM) */
    void (*mem_free)(void *ptr);
    void (*log)(const char *script, const char *line);
    /* A device command on behalf of a script. Returns a result shaped like the
     * cloud's: {"ok":true,"payload":{...}} or {"ok":false,"error":{"code","message"}}.
     * {"_async":true,"_token":N}: the result follows through sb_async_result(N),
     * and the calling callback waits for it. Caller owns the result. */
    cJSON *(*device_call)(const char *script, const char *command, const cJSON *params);
    void (*notify)(const char *script, const char *text);
    void (*cpu_yield)(void);                       /* optional: vTaskDelay(1) on ESP */
    /* optional: a script stopped running (stopped, failed, killed, finished or
     * restarted), so what it started for itself can stop with it. */
    void (*ended)(const char *script);
} sb_platform_t;

typedef struct {
    size_t mem_per_script;       /* bytes of Lua heap per script */
    size_t mem_total;            /* bytes across all scripts */
    int32_t slice_instructions;  /* VM instructions per resume before a kill */
    int32_t slice_ms;            /* wall-clock backstop per resume (C code, e.g. patterns) */
    int32_t hook_every;          /* instructions between hook calls */
    int32_t rtos_yield_ms;       /* call cpu_yield after running this long */
    int max_threads;             /* callbacks alive at once (waiting in wait()) */
    int max_timers;
    int max_handlers;
    int max_errors;              /* uncaught callback errors before the script is stopped */
    int notify_min_interval_ms;
    int async_timeout_ms;        /* a command that answers later: how long a callback waits */
    size_t max_source;
    const char *const *commands; /* device commands scripts may call, NULL-terminated */
} sb_limits_t;

void sb_default_limits(sb_limits_t *lim);

sb_runtime_t *sb_new(const sb_platform_t *pf, const sb_limits_t *lim);
void sb_free(sb_runtime_t *rt);

/* Compiles without running (for script.install). */
bool sb_check_syntax(sb_runtime_t *rt, const char *src, size_t len, char *err, size_t errlen);
/* Starts (or restarts) a script: runs its body until it returns or waits. */
bool sb_start(sb_runtime_t *rt, const char *name, const char *src, size_t len, char *err, size_t errlen);
bool sb_stop(sb_runtime_t *rt, const char *name);
/* Forgets a stopped script's record (its logs and last error). */
bool sb_forget(sb_runtime_t *rt, const char *name);

/* Delivers an event to every script with on(event, fn). data may be NULL. */
void sb_event(sb_runtime_t *rt, const char *event, const cJSON *data);
/* The result of a device command that answered {"_async":true,"_token":token}:
 * resumes the callback waiting for it. False if none is (it timed out, or its
 * script stopped). The caller keeps result. */
bool sb_async_result(sb_runtime_t *rt, int64_t token, const cJSON *result);

/* When sb_run_due next has work: esp_timer time in us, INT64_MAX if none. */
int64_t sb_next_wake_us(sb_runtime_t *rt);
/* Runs due timers and finished waits. Returns how many callbacks ran. */
int sb_run_due(sb_runtime_t *rt);

/* [{name, state, error, mem, mem_peak, mem_limit, timers, handlers, waiting, errors, cpu_ms}] */
cJSON *sb_list(sb_runtime_t *rt);
/* The script's last log lines, oldest first; NULL if unknown. */
cJSON *sb_logs(sb_runtime_t *rt, const char *name);
size_t sb_mem_total(sb_runtime_t *rt);

/* Host tests only: lets __cstack() report C stack use relative to here. */
void sb_set_stack_base(sb_runtime_t *rt, const void *base);

#ifdef __cplusplus
}
#endif
