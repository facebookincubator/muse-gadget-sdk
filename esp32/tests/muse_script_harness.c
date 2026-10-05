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

// Host harness for main/muse_script_sandbox.c on the real Lua
// (components/lua): the sandbox's walls, device commands (sync, async and
// timed out), events and timers, and the budgets that stop runaway scripts.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "muse_script_sandbox.h"

// ---- A fake device and a clock we move by hand -----------------------------

static int64_t s_now = 1000000;
static int64_t now_us(void) { return s_now; }
static void *mem_realloc(void *p, size_t n) { return realloc(p, n); }

static char s_log[64 * 1024];
static void log_cb(const char *script, const char *line) {
    char l[256];
    snprintf(l, sizeof(l), "%s| %s\n", script, line);
    strncat(s_log, l, sizeof(s_log) - strlen(s_log) - 1);
}
static bool logged(const char *text) { return strstr(s_log, text) != NULL; }

static int s_calls;
static int64_t s_last_token;
static char s_notified[128];

static cJSON *device_call(const char *script, const char *command, const cJSON *params) {
    (void)script;
    s_calls++;
    cJSON *r = cJSON_CreateObject();
    if (!strcmp(command, "camera.capture")) {   // answers later
        cJSON_AddBoolToObject(r, "_async", true);
        cJSON_AddNumberToObject(r, "_token", (double)++s_last_token);
        return r;
    }
    if (!strcmp(command, "led.set")) {
        const cJSON *c = cJSON_GetObjectItem(params, "color");
        if (cJSON_IsString(c) && !strcmp(c->valuestring, "nope")) {
            cJSON_AddBoolToObject(r, "ok", false);
            cJSON *e = cJSON_AddObjectToObject(r, "error");
            cJSON_AddStringToObject(e, "code", "invalid_params");
            cJSON_AddStringToObject(e, "message", "bad colour");
            return r;
        }
    }
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(r, "payload");
    if (!strcmp(command, "device.status")) cJSON_AddNumberToObject(payload, "battery_percent", 87);
    return r;
}

static void notify_cb(const char *script, const char *text) {
    (void)script;
    strncpy(s_notified, text, sizeof(s_notified) - 1);
}

static const char *const COMMANDS[] = { "device.status", "led.set", "camera.capture", NULL };

static sb_runtime_t *new_runtime(void) {
    sb_platform_t pf = {
        .now_us = now_us, .mem_realloc = mem_realloc, .mem_free = free,
        .log = log_cb, .device_call = device_call, .notify = notify_cb,
    };
    sb_limits_t lim;
    sb_default_limits(&lim);
    lim.commands = COMMANDS;
    lim.async_timeout_ms = 5000;
    lim.slice_ms = 100000;   // the fake clock doesn't move inside a slice: count instructions only
    return sb_new(&pf, &lim);
}

static bool start(sb_runtime_t *rt, const char *name, const char *src, char *err) {
    return sb_start(rt, name, src, strlen(src), err, 200);
}

static const char *state(sb_runtime_t *rt, const char *name, const char **error) {
    static char st[32], er[200];
    st[0] = er[0] = '\0';
    cJSON *l = sb_list(rt), *s;
    cJSON_ArrayForEach(s, l) {
        if (strcmp(cJSON_GetObjectItem(s, "name")->valuestring, name)) continue;
        strncpy(st, cJSON_GetObjectItem(s, "state")->valuestring, sizeof(st) - 1);
        cJSON *e = cJSON_GetObjectItem(s, "error");
        if (cJSON_IsString(e)) strncpy(er, e->valuestring, sizeof(er) - 1);
    }
    cJSON_Delete(l);
    if (error) *error = er;
    return st;
}

static void advance(sb_runtime_t *rt, int64_t ms) {
    s_now += ms * 1000;
    sb_run_due(rt);
}

// ---- Tests ---------------------------------------------------------------------

static void test_walls(sb_runtime_t *rt) {
    char err[200];
    assert(start(rt, "walls",
                 "for _, n in ipairs{'io','os','debug','package','require','dofile','loadfile'} do\n"
                 "  print(n, _G[n] == nil)\n"
                 "end\n"
                 "print('dump', string.dump == nil)\n"
                 "print('bytecode', (load('\\27Lua', 'x', 'b')) == nil)\n"
                 "print('gc', pcall(setmetatable, {}, {__gc = print}))\n"
                 "print('vm', pcall(device.call, 'device.set_vm', {}))\n",
                 err));
    const char *names[] = { "io", "os", "debug", "package", "require", "dofile", "loadfile" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char want[48];
        snprintf(want, sizeof(want), "walls| %s\ttrue", names[i]);
        assert(logged(want));
    }
    assert(logged("walls| dump\ttrue") && logged("walls| bytecode\ttrue"));
    assert(logged("walls| gc\tfalse") && logged("walls| vm\tfalse"));
    assert(!strcmp(state(rt, "walls", NULL), "finished"));
}

static void test_commands(sb_runtime_t *rt) {
    char err[200];
    int calls = s_calls;
    assert(start(rt, "cmds",
                 "local st = device.status()\n"
                 "print('battery', st.battery_percent)\n"
                 "print('bad', led.set{color = 'nope'})\n"
                 "print('ok', led.set{color = 'red'})\n"
                 "print('notify', notify('hello'))\n"
                 "print('again', notify('too soon'))\n",
                 err));
    assert(s_calls == calls + 3);
    assert(logged("cmds| battery\t87"));
    assert(logged("cmds| bad\tnil\tbad colour\tinvalid_params"));
    assert(logged("cmds| ok\ttrue"));
    assert(logged("cmds| notify\ttrue") && !strcmp(s_notified, "hello"));
    assert(logged("cmds| again\tnil\trate limited"));
}

static void test_async(sb_runtime_t *rt) {
    char err[200];
    assert(start(rt, "async",
                 "print('before')\n"
                 "local photo, msg = camera.capture{resolution = '240x240'}\n"
                 "print('photo', photo and photo.bytes, msg)\n"
                 "local late, why, code = camera.capture{}\n"
                 "print('late', late, why, code)\n",
                 err));
    assert(logged("async| before") && !logged("async| photo"));
    assert(!strcmp(state(rt, "async", NULL), "running"));   // waiting for the photo

    // The result arrives: the script resumes with the payload.
    int64_t token = s_last_token;
    cJSON *result = cJSON_Parse("{\"ok\":true,\"payload\":{\"bytes\":2325}}");
    assert(sb_async_result(rt, token, result));
    cJSON_Delete(result);
    assert(logged("async| photo\t2325\tnil"));
    assert(!sb_async_result(rt, token, NULL));   // nobody waits for it any more

    // The second never answers: it times out.
    advance(rt, 4000);
    assert(!logged("async| late"));
    advance(rt, 1500);
    assert(logged("async| late\tnil\ttimed out\ttimeout"));
    assert(!strcmp(state(rt, "async", NULL), "finished"));
}

static void test_events_and_timers(sb_runtime_t *rt) {
    char err[200];
    assert(start(rt, "events",
                 "local n = 0\n"
                 "on('tap', function(e) print('tap', e.x, e.y) end)\n"
                 "on('detection', function(e) print('saw', e.label, e.score) end)\n"
                 "local id = every(1000, function() n = n + 1; print('tick', n) end)\n"
                 "after(2500, function() cancel(id); print('cancelled') end)\n"
                 "after(100, function() print('waiting'); wait(300); print('woke') end)\n",
                 err));
    cJSON *tap = cJSON_Parse("{\"x\":120,\"y\":200}");
    sb_event(rt, "tap", tap);
    cJSON_Delete(tap);
    assert(logged("events| tap\t120\t200"));
    cJSON *det = cJSON_Parse("{\"label\":\"person\",\"score\":91}");
    sb_event(rt, "detection", det);
    cJSON_Delete(det);
    assert(logged("events| saw\tperson\t91"));
    sb_event(rt, "swipe", NULL);   // nobody listens: nothing happens

    for (int i = 0; i < 30; i++) advance(rt, 100);
    assert(logged("events| waiting") && logged("events| woke"));
    assert(logged("events| tick\t1") && logged("events| tick\t2") && !logged("events| tick\t3"));
    assert(logged("events| cancelled"));
    assert(!strcmp(state(rt, "events", NULL), "running"));   // still has its handlers
    assert(sb_stop(rt, "events"));
    assert(!strcmp(state(rt, "events", NULL), "stopped"));
}

static void test_budgets(sb_runtime_t *rt) {
    char err[200];
    const char *error;
    assert(!start(rt, "runaway", "while true do end", err));
    assert(strstr(err, "instructions"));
    assert(!start(rt, "pcall_evade", "while true do pcall(function() while true do end end) end", err));
    assert(!start(rt, "coro_evade",
                  "local co = coroutine.wrap(function() while true do end end)\nwhile true do pcall(co) end", err));
    assert(!start(rt, "mem_hog", "local t = {} for i = 1, 1e9 do t[i] = ('x'):rep(100) .. i end", err));
    assert(strstr(err, "memory"));
    assert(start(rt, "mem_caught",
                 "local ok, e = pcall(function() local t = {} for i = 1, 1e9 do t[i] = ('y'):rep(64) .. i end end)\n"
                 "print('caught', ok, e)\n",
                 err));
    assert(logged("mem_caught| caught\tfalse\tnot enough memory"));
    assert(!start(rt, "pattern", "local s = ('a'):rep(5000) .. 'b'\nprint(s:find(('a*'):rep(40) .. 'c'))", err));
    assert(start(rt, "cstack",
                 "local function deep(n) return pcall(deep, n + 1) end\n"
                 "print('deep', pcall(deep, 1))\n",
                 err));
    assert(!strcmp(state(rt, "runaway", &error), "failed") && strstr(error, "instructions"));
    // A syntax error is caught before anything runs.
    assert(!sb_check_syntax(rt, "print('x' .. )", 14, err, sizeof(err)) && strstr(err, "near"));
    assert(sb_check_syntax(rt, "print('fine')", 13, err, sizeof(err)));
}

static void test_memory_returned(sb_runtime_t *rt) {
    char err[200];
    assert(start(rt, "sleeper", "after(0, function() wait(100000) end)", err));
    advance(rt, 10);
    assert(sb_mem_total(rt) > 0);
    cJSON *l = sb_list(rt), *s;
    cJSON_ArrayForEach(s, l) sb_stop(rt, cJSON_GetObjectItem(s, "name")->valuestring);
    cJSON_Delete(l);
    assert(sb_mem_total(rt) == 0);   // every closed state gave back all it had
}

int main(void) {
    sb_runtime_t *rt = new_runtime();
    assert(rt);
    test_walls(rt);
    test_commands(rt);
    test_async(rt);
    test_events_and_timers(rt);
    test_budgets(rt);
    test_memory_returned(rt);
    sb_free(rt);
    printf("ok\n");
    return 0;
}
