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
 * sandbox: see sandbox.h. Lua 5.4 (built without LUA_32BITS: 64-bit integers
 * and doubles, standard Lua semantics). Compiles against 5.5 too.
 */
#include "muse_script_sandbox.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#define NAME_MAX_LEN 31
#define LOG_LINES 32
#define LOG_LINE_LEN 160
#define EVENT_NAME_MAX 31
#define JSON_DEPTH_MAX 8
#define JSON_ITEMS_MAX 256

typedef enum { SB_RUNNING, SB_STOPPED, SB_FINISHED, SB_FAILED } sb_state_t;

typedef struct {
    int id;
    int fn_ref;
    int64_t period_us; /* 0: after(), once */
    int64_t next_us;
    lua_State *running; /* its callback, while it waits; no overlapping runs */
} sb_timer_t;

typedef struct {
    lua_State *co;
    int ref;
    int64_t wake_us;   /* wait(): when; a device command: when it times out */
    int64_t token;     /* a device command's result it waits for; 0 for wait() */
} sb_sleeper_t;

typedef struct sb_script {
    struct sb_script *next;
    sb_runtime_t *rt;
    char name[NAME_MAX_LEN + 1];
    lua_State *L;
    sb_state_t state;
    bool close_pending;
    bool stop_requested;
    int depth; /* nested entries from C: close only at depth 0 */

    size_t mem_used, mem_peak, mem_base;

    lua_State *current;  /* the scheduler thread being resumed */
    int32_t slice_left;
    int64_t deadline_us;
    unsigned match_steps;
    const char *kill_reason; /* points at error[] once killed */
    char error[LOG_LINE_LEN];
    int64_t cpu_us;
    int errors;
    int64_t last_notify_us;

    cJSON *pending_json; /* freed if a conversion to Lua raised */

    int next_id;
    int handlers;
    sb_timer_t *timers;
    int ntimers;
    sb_sleeper_t *sleepers;
    int nsleepers;

    char log[LOG_LINES][LOG_LINE_LEN];
    int log_next, log_count;
} sb_script_t;

struct sb_runtime {
    sb_platform_t pf;
    sb_limits_t lim;
    sb_script_t *scripts;
    size_t mem_total;
    int64_t last_yield_us;
    const char *stack_base;
};

static const char WAIT_KEY = 'w';      /* wait() yields this, then the wake time */
static const char ASYNC_KEY = 'a';     /* a device command answering later yields this, then its token */
static const char HANDLERS_KEY = 'h';  /* registry: event -> {id -> fn} */

/* ---- Helpers --------------------------------------------------------------- */

static sb_script_t *script_of(lua_State *L) {
    return *(sb_script_t **)lua_getextraspace(L);
}

static void log_line(sb_script_t *s, const char *fmt, ...) {
    char *line = s->log[s->log_next];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, LOG_LINE_LEN, fmt, ap);
    va_end(ap);
    s->log_next = (s->log_next + 1) % LOG_LINES;
    if (s->log_count < LOG_LINES) s->log_count++;
    if (s->rt->pf.log) s->rt->pf.log(s->name, line);
}

static void fail(sb_script_t *s, const char *fmt, ...) {
    if (s->state != SB_RUNNING) return;
    char msg[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(s->error, sizeof(s->error), "%s", msg);
    s->state = SB_FAILED;
    s->close_pending = true;
    log_line(s, "stopped: %s", s->error);
}

/* ---- Memory: per-script and total budgets ------------------------------------ */

static void *sb_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    sb_script_t *s = ud;
    sb_runtime_t *rt = s->rt;
    size_t old = ptr ? osize : 0; /* with ptr NULL, osize is an object type */
    if (nsize == 0) {
        if (ptr) {
            rt->pf.mem_free(ptr);
            s->mem_used -= old;
            rt->mem_total -= old;
        }
        return NULL;
    }
    if (nsize > old) {
        size_t grow = nsize - old;
        if (s->mem_used + grow > rt->lim.mem_per_script || rt->mem_total + grow > rt->lim.mem_total) {
            return NULL; /* Lua runs an emergency GC and retries, then raises "not enough memory" */
        }
    }
    void *p = rt->pf.mem_realloc(ptr, nsize);
    if (!p) return nsize <= old ? ptr : NULL; /* a failed shrink keeps the block */
    s->mem_used = s->mem_used - old + nsize;
    rt->mem_total = rt->mem_total - old + nsize;
    if (s->mem_used > s->mem_peak) s->mem_peak = s->mem_used;
    return p;
}

/* ---- CPU: count hook, deadline, uncatchable kills ----------------------------- */

static void kill_script(sb_script_t *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error, sizeof(s->error), fmt, ap);
    va_end(ap);
    s->kill_reason = s->error;
}

static void check_budget(lua_State *L, sb_script_t *s) {
    sb_runtime_t *rt = s->rt;
    if (!s->kill_reason) {
        int64_t now = rt->pf.now_us();
        if (s->slice_left <= 0) {
            kill_script(s, "killed: over %ld instructions without waiting (loop without wait()?)",
                        (long)rt->lim.slice_instructions);
        } else if (now > s->deadline_us) {
            kill_script(s, "killed: ran %ld ms without waiting", (long)rt->lim.slice_ms);
        } else if (rt->pf.cpu_yield && now - rt->last_yield_us >= (int64_t)rt->lim.rtos_yield_ms * 1000) {
            rt->pf.cpu_yield(); /* let IDLE and lower-priority tasks run; not charged to the script */
            int64_t after = rt->pf.now_us();
            s->deadline_us += after - now;
            rt->last_yield_us = after;
        }
    }
    if (s->kill_reason) luaL_error(L, "%s", s->kill_reason);
}

static void count_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    sb_script_t *s = script_of(L);
    s->slice_left -= s->rt->lim.hook_every;
    check_budget(L, s);
}

/* Called by the patched lstrlib.c matcher (-DLUAI_MATCHSTEP_FUNC=sb_matchstep). */
void sb_matchstep(lua_State *L) {
    sb_script_t *s = script_of(L);
    if ((++s->match_steps & 1023) == 0) {
        s->slice_left -= 1024;
        check_budget(L, s);
    }
}

/* pcall, xpcall, coroutine.resume and coroutine.close catch errors; once the
 * script is killed, these re-raise instead. The originals stay yieldable. */
static int guarded_k(lua_State *L, int status, lua_KContext ctx) {
    (void)status;
    (void)ctx;
    sb_script_t *s = script_of(L);
    if (s->kill_reason) return luaL_error(L, "%s", s->kill_reason);
    return lua_gettop(L);
}

static int guarded(lua_State *L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_callk(L, lua_gettop(L) - 1, LUA_MULTRET, 0, guarded_k);
    return guarded_k(L, LUA_OK, 0);
}

static void guard(lua_State *L, int table_idx, const char *field) {
    lua_getfield(L, table_idx, field);
    lua_pushcclosure(L, guarded, 1);
    lua_setfield(L, table_idx, field);
}

/* ---- Base library replacements ------------------------------------------------- */

static int l_print(lua_State *L) {
    sb_script_t *s = script_of(L);
    luaL_Buffer b;
    int n = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    log_line(s, "%s", lua_tostring(L, -1));
    return 0;
}

static int l_load(lua_State *L) {
    sb_script_t *s = script_of(L);
    size_t len;
    const char *src = luaL_checklstring(L, 1, &len); /* no reader functions */
    const char *chunkname = luaL_optstring(L, 2, "=load");
    if (len > s->rt->lim.max_source) return luaL_error(L, "load: chunk too large");
    int env = !lua_isnone(L, 4) ? 4 : 0;
    int st = luaL_loadbufferx(L, src, len, chunkname, "t"); /* text only: no bytecode */
    if (st != LUA_OK) {
        luaL_pushfail(L);
        lua_insert(L, -2);
        return 2;
    }
    if (env) {
        lua_pushvalue(L, env);
        if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
    }
    return 1;
}

static int l_setmetatable(lua_State *L) {
    if (lua_istable(L, 2)) {
        if (lua_getfield(L, 2, "__gc") != LUA_TNIL) {
            /* finalizers run with hooks off: an endless one would hang the task */
            return luaL_error(L, "__gc metamethods are not allowed in scripts");
        }
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static int l_collectgarbage(lua_State *L) {
    static const char *const opts[] = {"collect", "count", "step", NULL};
    int o = luaL_checkoption(L, 1, "collect", opts);
    if (o == 1) {
        lua_pushnumber(L, (lua_Number)script_of(L)->mem_used / 1024);
        return 1;
    }
    lua_gc(L, o == 0 ? LUA_GCCOLLECT : LUA_GCSTEP, 0);
    lua_pushinteger(L, 0);
    return 1;
}

/* ---- Scheduler ----------------------------------------------------------------- */

static void drop_thread(sb_script_t *s, lua_State *co, int ref) {
    for (int i = 0; i < s->ntimers; i++) {
        if (s->timers[i].running == co) s->timers[i].running = NULL;
    }
#if LUA_VERSION_NUM >= 504
    lua_closethread(co, s->L); /* runs pending __close handlers (hooked) */
#endif
    luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
}

/* Resumes co (which has its function or wait() continuation ready). */
static void resume_thread(sb_script_t *s, lua_State *co, int ref, int nargs) {
    sb_runtime_t *rt = s->rt;
    int64_t t0 = rt->pf.now_us();
    s->current = co;
    s->slice_left = rt->lim.slice_instructions;
    s->deadline_us = t0 + (int64_t)rt->lim.slice_ms * 1000;
    rt->last_yield_us = t0;
    int nres = 0;
    int st = lua_resume(co, s->L, nargs, &nres);
    s->current = NULL;
    s->cpu_us += rt->pf.now_us() - t0;
    if (s->pending_json) {
        cJSON_Delete(s->pending_json);
        s->pending_json = NULL;
    }

    if (st == LUA_YIELD && !s->kill_reason && nres == 2
        && (lua_touserdata(co, -2) == &WAIT_KEY || lua_touserdata(co, -2) == &ASYNC_KEY)) {
        bool async = lua_touserdata(co, -2) == &ASYNC_KEY;
        int64_t v = lua_tointeger(co, -1);
        lua_pop(co, 2);
        if (s->nsleepers < rt->lim.max_threads) {
            s->sleepers[s->nsleepers++] = async
                ? (sb_sleeper_t){co, ref, rt->pf.now_us() + (int64_t)rt->lim.async_timeout_ms * 1000, v}
                : (sb_sleeper_t){co, ref, v, 0};
            return; /* still alive */
        }
        log_line(s, "error: more than %d callbacks waiting at once", rt->lim.max_threads);
        s->errors++;
    } else if (st == LUA_YIELD) {
        if (!s->kill_reason) log_line(s, "error: coroutine.yield() outside a coroutine");
    } else if (st != LUA_OK) {
        const char *msg = lua_tostring(co, -1);
        if (s->stop_requested) {
            /* stop(): leave() closes the script */
        } else if (s->kill_reason) {
            fail(s, "%s", s->kill_reason);
        } else if (st == LUA_ERRMEM) {
            fail(s, "out of memory (limit %zu KB)", rt->lim.mem_per_script / 1024);
        } else {
            log_line(s, "error: %s", msg ? msg : "(error object is not a string)");
            if (++s->errors >= rt->lim.max_errors) fail(s, "too many errors");
        }
    }
    drop_thread(s, co, ref);
}

/* Main-thread stack: fn, then nargs arguments. Runs them on a new thread. */
static void spawn(sb_script_t *s, int nargs, int timer_id) {
    lua_State *L = s->L;
    if (s->nsleepers >= s->rt->lim.max_threads) {
        lua_pop(L, nargs + 1);
        log_line(s, "error: callback skipped, %d already waiting", s->rt->lim.max_threads);
        return;
    }
    lua_State *co = lua_newthread(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_xmove(L, co, nargs + 1);
    resume_thread(s, co, ref, nargs);
    if (!timer_id) return;
    for (int i = 0; i < s->nsleepers; i++) {
        if (s->sleepers[i].co != co) continue;
        for (int t = 0; t < s->ntimers; t++) {  /* by id: the callback may have cancelled timers */
            if (s->timers[t].id == timer_id) s->timers[t].running = co;
        }
    }
}

static int l_wait(lua_State *L) {
    sb_script_t *s = script_of(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (L != s->current) {
        return luaL_error(L, "wait() works in the script body and in callbacks, not in your own coroutines");
    }
    if (ms < 0) ms = 0;
    lua_pushlightuserdata(L, (void *)&WAIT_KEY);
    lua_pushinteger(L, s->rt->pf.now_us() + (int64_t)ms * 1000);
    return lua_yield(L, 2); /* "attempt to yield across a C-call boundary" in sort/gsub callbacks */
}

static int add_timer(lua_State *L, int64_t delay_ms, int64_t period_ms) {
    sb_script_t *s = script_of(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (s->ntimers >= s->rt->lim.max_timers) return luaL_error(L, "more than %d timers", s->rt->lim.max_timers);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sb_timer_t *t = &s->timers[s->ntimers++];
    *t = (sb_timer_t){.id = ++s->next_id, .fn_ref = ref, .period_us = period_ms * 1000,
                      .next_us = s->rt->pf.now_us() + delay_ms * 1000};
    lua_pushinteger(L, t->id);
    return 1;
}

static int l_every(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 20, 1, "every() needs at least 20 ms");
    return add_timer(L, ms, ms);
}

static int l_after(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    return add_timer(L, ms < 0 ? 0 : ms, 0);
}

static int l_on(lua_State *L) {
    sb_script_t *s = script_of(L);
    size_t len;
    luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len > 0 && len <= EVENT_NAME_MAX, 1, "bad event name");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (s->handlers >= s->rt->lim.max_handlers) return luaL_error(L, "more than %d handlers", s->rt->lim.max_handlers);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &HANDLERS_KEY);
    if (lua_getfield(L, -1, lua_tostring(L, 1)) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, lua_tostring(L, 1));
    }
    int id = ++s->next_id;
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, id);
    s->handlers++;
    lua_pushinteger(L, id);
    return 1;
}

static int l_cancel(lua_State *L) {
    sb_script_t *s = script_of(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; i < s->ntimers; i++) {
        if (s->timers[i].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, s->timers[i].fn_ref);
            s->timers[i] = s->timers[--s->ntimers];
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &HANDLERS_KEY);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_rawgeti(L, -1, id) != LUA_TNIL) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_rawseti(L, -2, id);
            s->handlers--;
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pop(L, 2);
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int l_now(lua_State *L) {
    lua_pushinteger(L, script_of(L)->rt->pf.now_us() / 1000);
    return 1;
}

static int l_stop(lua_State *L) {
    sb_script_t *s = script_of(L);
    s->stop_requested = true;
    return luaL_error(L, "%s", "stopped by the script");
}

/* ---- JSON <-> Lua ------------------------------------------------------------ */

/* Never raises (cJSON memory isn't Lua's): returns NULL with *err set. */
static cJSON *to_json(lua_State *L, int idx, int depth, const char **err) {
    idx = lua_absindex(L, idx);
    if (depth > JSON_DEPTH_MAX) {
        *err = "table nested too deeply";
        return NULL;
    }
    if (!lua_checkstack(L, 3)) {
        *err = "stack overflow";
        return NULL;
    }
    switch (lua_type(L, idx)) {
    case LUA_TNIL: return cJSON_CreateNull();
    case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER: return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        lua_Unsigned n = lua_rawlen(L, idx);
        int count = 0;
        bool array = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            lua_pop(L, 1);
            count++;
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1 || (lua_Unsigned)lua_tointeger(L, -1) > n) array = false;
        }
        if (count > JSON_ITEMS_MAX) {
            *err = "table too large";
            return NULL;
        }
        array = array && count > 0;
        cJSON *out = array ? cJSON_CreateArray() : cJSON_CreateObject();
        if (!out) {
            *err = "out of memory";
            return NULL;
        }
        if (array) {
            for (lua_Unsigned i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, (lua_Integer)i);
                cJSON *v = to_json(L, -1, depth + 1, err);
                lua_pop(L, 1);
                if (!v) goto bad;
                cJSON_AddItemToArray(out, v);
            }
        } else {
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                char key[32];
                if (lua_type(L, -2) == LUA_TSTRING) {
                    snprintf(key, sizeof(key), "%s", lua_tostring(L, -2));
                } else if (lua_isinteger(L, -2)) {
                    snprintf(key, sizeof(key), "%lld", (long long)lua_tointeger(L, -2));
                } else {
                    lua_pop(L, 2);
                    *err = "table keys must be strings or integers";
                    goto bad;
                }
                cJSON *v = to_json(L, -1, depth + 1, err);
                lua_pop(L, 1);
                if (!v) {
                    lua_pop(L, 1);
                    goto bad;
                }
                cJSON_AddItemToObject(out, key, v);
            }
        }
        return out;
    bad:
        cJSON_Delete(out);
        return NULL;
    }
    default:
        *err = "only nil, booleans, numbers, strings and tables can be sent";
        return NULL;
    }
}

/* May raise (memory); callers park the cJSON in pending_json first. */
static void push_json(lua_State *L, const cJSON *j, int depth) {
    luaL_checkstack(L, 3, "json");
    if (!j || cJSON_IsNull(j) || depth > JSON_DEPTH_MAX) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(j)) {
        lua_pushboolean(L, cJSON_IsTrue(j));
    } else if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        if (d >= -9007199254740992.0 && d <= 9007199254740992.0 && d == (double)(lua_Integer)d) {
            lua_pushinteger(L, (lua_Integer)d);
        } else {
            lua_pushnumber(L, d);
        }
    } else if (cJSON_IsString(j)) {
        lua_pushstring(L, j->valuestring);
    } else if (cJSON_IsArray(j)) {
        lua_createtable(L, cJSON_GetArraySize(j), 0);
        int i = 1;
        for (const cJSON *c = j->child; c; c = c->next) {
            push_json(L, c, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsObject(j)) {
        lua_newtable(L);
        for (const cJSON *c = j->child; c; c = c->next) {
            push_json(L, c, depth + 1);
            lua_setfield(L, -2, c->string);
        }
    } else {
        lua_pushnil(L);
    }
}

/* ---- Device commands --------------------------------------------------------- */

static bool command_allowed(sb_runtime_t *rt, const char *cmd) {
    for (const char *const *c = rt->lim.commands; c && *c; c++) {
        if (strcmp(*c, cmd) == 0) return true;
    }
    return false;
}

/* After a device command's result: the values sb_async_result() or a timeout
 * resumed the callback with, which replaced the yielded ones. */
static int async_k(lua_State *L, int status, lua_KContext base) {
    (void)status;
    return lua_gettop(L) - (int)base;
}

/* Pushes a command's result: its payload (or true), or nil + message + code. */
static int push_result(lua_State *L, const cJSON *result) {
    const cJSON *ok = cJSON_GetObjectItem(result, "ok");
    if (cJSON_IsTrue(ok)) {
        const cJSON *payload = cJSON_GetObjectItem(result, "payload");
        if (payload && payload->child) push_json(L, payload, 0);
        else lua_pushboolean(L, 1);
        return 1;
    }
    const cJSON *e = cJSON_GetObjectItem(result, "error");
    luaL_pushfail(L);
    const cJSON *m = cJSON_GetObjectItem(e, "message"), *c = cJSON_GetObjectItem(e, "code");
    lua_pushstring(L, cJSON_IsString(m) ? m->valuestring : "failed");
    lua_pushstring(L, cJSON_IsString(c) ? c->valuestring : "error");
    return 3;
}

/* result, or nil + message + code */
static int call_device(lua_State *L, const char *cmd, int params_idx) {
    sb_script_t *s = script_of(L);
    if (!command_allowed(s->rt, cmd)) return luaL_error(L, "device command '%s' isn't available to scripts", cmd);
    const char *err = NULL;
    cJSON *params = NULL;
    if (params_idx && !lua_isnoneornil(L, params_idx)) {
        luaL_checktype(L, params_idx, LUA_TTABLE);
        params = to_json(L, params_idx, 0, &err);
        if (!params) return luaL_error(L, "%s: %s", cmd, err);
    }
    cJSON *result = s->rt->pf.device_call(s->name, cmd, params);
    cJSON_Delete(params);
    if (!result) return luaL_error(L, "%s: no result", cmd);
    if (cJSON_IsTrue(cJSON_GetObjectItem(result, "_async"))) {
        const cJSON *token = cJSON_GetObjectItem(result, "_token");
        int64_t t = cJSON_IsNumber(token) ? (int64_t)token->valuedouble : 0;
        cJSON_Delete(result);
        if (t && L == s->current) {
            /* Wait here for the result, as wait() does for time. */
            int base = lua_gettop(L);
            lua_pushlightuserdata(L, (void *)&ASYNC_KEY);
            lua_pushinteger(L, t);
            return lua_yieldk(L, 2, (lua_KContext)base, async_k);
        }
        lua_pushboolean(L, 1); /* in the script's own coroutine: started, not awaited */
        return 1;
    }
    s->pending_json = result;
    int n = push_result(L, result);
    s->pending_json = NULL;
    cJSON_Delete(result);
    return n;
}

static int l_device_call(lua_State *L) {
    return call_device(L, luaL_checkstring(L, 1), 2);
}

static int l_command_sugar(lua_State *L) {
    return call_device(L, lua_tostring(L, lua_upvalueindex(1)), 1);
}

static int l_notify(lua_State *L) {
    sb_script_t *s = script_of(L);
    const char *text = luaL_checkstring(L, 1);
    int64_t now = s->rt->pf.now_us();
    if (s->last_notify_us && now - s->last_notify_us < (int64_t)s->rt->lim.notify_min_interval_ms * 1000) {
        luaL_pushfail(L);
        lua_pushstring(L, "rate limited");
        return 2;
    }
    s->last_notify_us = now;
    if (s->rt->pf.notify) s->rt->pf.notify(s->name, text);
    lua_pushboolean(L, 1);
    return 1;
}

#ifdef SB_STACK_PROBE
static int l_cstack(lua_State *L) {
    char here;
    const char *base = script_of(L)->rt->stack_base;
    lua_pushinteger(L, base ? (lua_Integer)(base - &here) : 0);
    return 1;
}
#endif

/* ---- State setup ---------------------------------------------------------------- */

static int setup_p(lua_State *L) {
    sb_script_t *s = lua_touserdata(L, 1);
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base},         {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},   {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},   {LUA_UTF8LIBNAME, luaopen_utf8},
        {NULL, NULL},
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    lua_pushglobaltable(L);
    int G = lua_gettop(L);
    static const char *const remove[] = {"dofile", "loadfile", NULL};
    for (const char *const *r = remove; *r; r++) {
        lua_pushnil(L);
        lua_setfield(L, G, *r);
    }
    lua_getfield(L, G, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    guard(L, G, "pcall");
    guard(L, G, "xpcall");
    lua_getfield(L, G, "coroutine");
    guard(L, lua_gettop(L), "resume");
    guard(L, lua_gettop(L), "close");
    lua_pop(L, 1);
    lua_getfield(L, G, "setmetatable");
    lua_pushcclosure(L, l_setmetatable, 1);
    lua_setfield(L, G, "setmetatable");
    static const luaL_Reg api[] = {
        {"print", l_print}, {"load", l_load},     {"collectgarbage", l_collectgarbage},
        {"wait", l_wait},   {"every", l_every},   {"after", l_after},
        {"on", l_on},       {"cancel", l_cancel}, {"now", l_now},
        {"stop", l_stop},   {"notify", l_notify},
#ifdef SB_STACK_PROBE
        {"__cstack", l_cstack},
#endif
        {NULL, NULL},
    };
    luaL_setfuncs(L, api, 0);

    /* device.call plus one function per allowed command: "led.set" -> led.set{...} */
    lua_newtable(L);
    lua_pushcfunction(L, l_device_call);
    lua_setfield(L, -2, "call");
    lua_setfield(L, G, "device");
    for (const char *const *c = s->rt->lim.commands; c && *c; c++) {
        const char *dot = strchr(*c, '.');
        if (!dot || dot == *c) continue;
        lua_pushlstring(L, *c, (size_t)(dot - *c));
        if (lua_rawget(L, G) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushlstring(L, *c, (size_t)(dot - *c));
            lua_pushvalue(L, -2);
            lua_rawset(L, G);
        }
        lua_pushstring(L, *c);
        lua_pushcclosure(L, l_command_sugar, 1);
        lua_setfield(L, -2, dot + 1);
        lua_pop(L, 1);
    }
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &HANDLERS_KEY);
    lua_pop(L, 1); /* _G */
    return 0;
}

static int panic(lua_State *L) {
    sb_script_t *s = script_of(L);
    fprintf(stderr, "lua panic in %s: %s\n", s ? s->name : "?", lua_tostring(L, -1));
    abort(); /* unreachable while every entry is protected */
}

static bool open_state(sb_runtime_t *rt, sb_script_t *s) {
#if LUA_VERSION_NUM >= 505
    s->L = lua_newstate(sb_alloc, s, (unsigned)rt->pf.now_us());
#else
    s->L = lua_newstate(sb_alloc, s);
#endif
    if (!s->L) return false;
    *(sb_script_t **)lua_getextraspace(s->L) = s; /* copied into every new thread */
    lua_atpanic(s->L, panic);
    lua_sethook(s->L, count_hook, LUA_MASKCOUNT, rt->lim.hook_every); /* inherited by new threads */
    lua_pushcfunction(s->L, setup_p);
    lua_pushlightuserdata(s->L, s);
    if (lua_pcall(s->L, 1, 0, 0) != LUA_OK) {
        lua_close(s->L);
        s->L = NULL;
        return false;
    }
    s->mem_base = s->mem_used;
    return true;
}

/* ---- Entries --------------------------------------------------------------------- */

static void enter(sb_script_t *s) { s->depth++; }

static void leave(sb_script_t *s) {
    if (--s->depth > 0) return;
    if (s->stop_requested && s->state == SB_RUNNING) {
        s->state = SB_STOPPED;
        s->close_pending = true;
        snprintf(s->error, sizeof(s->error), "stopped by the script");
        log_line(s, "stopped by the script");
    }
    if (s->state == SB_RUNNING && s->ntimers == 0 && s->handlers == 0 && s->nsleepers == 0) {
        s->state = SB_FINISHED;
        s->close_pending = true;
    }
    if (s->close_pending && s->L) {
        lua_close(s->L);
        s->L = NULL;
        s->close_pending = false;
        s->ntimers = s->nsleepers = s->handlers = 0;
        if (s->mem_used != 0) log_line(s, "warning: %zu bytes unaccounted after close", s->mem_used);
        if (s->rt->pf.ended) s->rt->pf.ended(s->name);
    }
}

/* Runs fn(L) protected on the script's main thread: a memory error there fails it. */
static void run_protected(sb_script_t *s, lua_CFunction fn, void *arg) {
    if (s->state != SB_RUNNING || !s->L) return;
    enter(s);
    lua_pushcfunction(s->L, fn);
    lua_pushlightuserdata(s->L, arg);
    int st = lua_pcall(s->L, 1, 0, 0);
    if (st != LUA_OK) {
        if (s->kill_reason) fail(s, "%s", s->kill_reason);
        else if (st == LUA_ERRMEM) fail(s, "out of memory (limit %zu KB)", s->rt->lim.mem_per_script / 1024);
        else fail(s, "%s", lua_tostring(s->L, -1));
        lua_pop(s->L, 1);
    }
    leave(s);
}

static sb_script_t *find(sb_runtime_t *rt, const char *name) {
    for (sb_script_t *s = rt->scripts; s; s = s->next) {
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

void sb_default_limits(sb_limits_t *lim) {
    *lim = (sb_limits_t){
        .mem_per_script = 256 * 1024,
        .mem_total = 2 * 1024 * 1024,
        .slice_instructions = 2000000,
        .slice_ms = 200,
        .hook_every = 1000,
        .rtos_yield_ms = 10,
        .max_threads = 16,
        .max_timers = 16,
        .max_handlers = 32,
        .max_errors = 10,
        .notify_min_interval_ms = 10000,
        .async_timeout_ms = 120000,
        .max_source = 16 * 1024,
    };
}

sb_runtime_t *sb_new(const sb_platform_t *pf, const sb_limits_t *lim) {
    sb_runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    rt->pf = *pf;
    rt->lim = *lim;
    return rt;
}

static void free_script(sb_runtime_t *rt, sb_script_t *s) {
    if (s->L) lua_close(s->L);
    rt->pf.mem_free(s->timers);
    rt->pf.mem_free(s->sleepers);
    rt->pf.mem_free(s);
}

void sb_free(sb_runtime_t *rt) {
    if (!rt) return;
    while (rt->scripts) {
        sb_script_t *s = rt->scripts;
        rt->scripts = s->next;
        free_script(rt, s);
    }
    free(rt);
}

void sb_set_stack_base(sb_runtime_t *rt, const void *base) { rt->stack_base = base; }
size_t sb_mem_total(sb_runtime_t *rt) { return rt->mem_total; }

static bool valid_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > NAME_MAX_LEN) return false;
    for (const char *p = name; *p; p++) {
        if (!(*p == '_' || *p == '-' || (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return false;
    }
    return true;
}

static void *zalloc(sb_runtime_t *rt, size_t n) {
    void *p = rt->pf.mem_realloc(NULL, n);
    if (p) memset(p, 0, n);
    return p;
}

bool sb_check_syntax(sb_runtime_t *rt, const char *src, size_t len, char *err, size_t errlen) {
    if (len > rt->lim.max_source) {
        snprintf(err, errlen, "script is larger than %zu bytes", rt->lim.max_source);
        return false;
    }
    sb_script_t *tmp = zalloc(rt, sizeof(*tmp)); /* not on the stack: it holds the log ring */
    if (!tmp) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    tmp->rt = rt;
    tmp->state = SB_RUNNING;
    snprintf(tmp->name, sizeof(tmp->name), "syntax");
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(sb_alloc, tmp, 0);
#else
    lua_State *L = lua_newstate(sb_alloc, tmp);
#endif
    int st = LUA_ERRMEM;
    if (L) {
        *(sb_script_t **)lua_getextraspace(L) = tmp;
        st = luaL_loadbufferx(L, src, len, "=script", "t");
        if (st != LUA_OK) snprintf(err, errlen, "%s", lua_tostring(L, -1));
        lua_close(L);
    } else {
        snprintf(err, errlen, "out of memory");
    }
    rt->pf.mem_free(tmp);
    return st == LUA_OK;
}

typedef struct {
    sb_script_t *s;
    const char *src;
    size_t len;
    char *err;
    size_t errlen;
    bool ok;
} start_args_t;

static int start_p(lua_State *L) {
    start_args_t *a = lua_touserdata(L, 1);
    char chunkname[NAME_MAX_LEN + 2];
    snprintf(chunkname, sizeof(chunkname), "=%s", a->s->name);
    if (luaL_loadbufferx(L, a->src, a->len, chunkname, "t") != LUA_OK) {
        snprintf(a->err, a->errlen, "%s", lua_tostring(L, -1));
        a->s->state = SB_FAILED;
        snprintf(a->s->error, sizeof(a->s->error), "%s", a->err);
        a->s->close_pending = true;
        return 0;
    }
    a->ok = true;
    spawn(a->s, 0, 0); /* the body runs like a callback: it may wait() */
    return 0;
}

bool sb_start(sb_runtime_t *rt, const char *name, const char *src, size_t len, char *err, size_t errlen) {
    if (!valid_name(name)) {
        snprintf(err, errlen, "names are 1-%d letters, digits, '_' or '-'", NAME_MAX_LEN);
        return false;
    }
    if (len > rt->lim.max_source) {
        snprintf(err, errlen, "script is larger than %zu bytes", rt->lim.max_source);
        return false;
    }
    sb_script_t *s = find(rt, name);
    if (s) {
        sb_stop(rt, name);
        sb_forget(rt, name);
    }
    s = zalloc(rt, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    s->rt = rt;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->timers = zalloc(rt, sizeof(sb_timer_t) * (size_t)rt->lim.max_timers);
    s->sleepers = zalloc(rt, sizeof(sb_sleeper_t) * (size_t)rt->lim.max_threads);
    if (!s->timers || !s->sleepers || !open_state(rt, s)) {
        snprintf(err, errlen, "out of memory");
        free_script(rt, s);
        return false;
    }
    s->state = SB_RUNNING;
    s->next = rt->scripts;
    rt->scripts = s;
    start_args_t a = {s, src, len, err, errlen, false};
    err[0] = 0;
    run_protected(s, start_p, &a);
    if (!a.ok) return false;
    if (s->state == SB_FAILED) snprintf(err, errlen, "%s", s->error);
    return s->state != SB_FAILED;
}

bool sb_stop(sb_runtime_t *rt, const char *name) {
    sb_script_t *s = find(rt, name);
    if (!s || s->state != SB_RUNNING) return false;
    s->state = SB_STOPPED;
    s->close_pending = true;
    snprintf(s->error, sizeof(s->error), "stopped");
    log_line(s, "stopped");
    if (s->depth == 0) {
        enter(s);
        leave(s);
    }
    return true;
}

bool sb_forget(sb_runtime_t *rt, const char *name) {
    for (sb_script_t **pp = &rt->scripts; *pp; pp = &(*pp)->next) {
        sb_script_t *s = *pp;
        if (strcmp(s->name, name) == 0 && s->state != SB_RUNNING && s->depth == 0) {
            *pp = s->next;
            free_script(rt, s);
            return true;
        }
    }
    return false;
}

typedef struct {
    sb_script_t *s;
    const char *event;
    const cJSON *data;
} event_args_t;

static int event_p(lua_State *L) {
    event_args_t *a = lua_touserdata(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &HANDLERS_KEY);
    if (lua_getfield(L, -1, a->event) != LUA_TTABLE) return 0;
    /* copy first: callbacks may add or cancel handlers */
    lua_newtable(L);
    int n = 0;
    lua_pushnil(L);
    while (lua_next(L, -3)) {
        lua_rawseti(L, -3, ++n);
    }
    int list = lua_gettop(L);
    push_json(L, a->data, 0);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    /* Its name as e.event, unless it has an event of its own (a "ui" event's
     * click, change, show...). */
    if (lua_getfield(L, -1, "event") == LUA_TNIL) {
        lua_pushstring(L, a->event);
        lua_setfield(L, -3, "event");
    }
    lua_pop(L, 1);
    int data = lua_gettop(L);
    for (int i = 1; i <= n && a->s->state == SB_RUNNING; i++) {
        lua_rawgeti(L, list, i);
        lua_pushvalue(L, data);
        spawn(a->s, 1, 0);
    }
    return 0;
}

void sb_event(sb_runtime_t *rt, const char *event, const cJSON *data) {
    for (sb_script_t *s = rt->scripts; s; s = s->next) {
        if (s->state != SB_RUNNING || s->handlers == 0) continue;
        event_args_t a = {s, event, data};
        run_protected(s, event_p, &a);
    }
}

int64_t sb_next_wake_us(sb_runtime_t *rt) {
    int64_t next = INT64_MAX;
    for (sb_script_t *s = rt->scripts; s; s = s->next) {
        if (s->state != SB_RUNNING) continue;
        for (int i = 0; i < s->ntimers; i++) {
            if (s->timers[i].next_us < next) next = s->timers[i].next_us;
        }
        for (int i = 0; i < s->nsleepers; i++) {
            if (s->sleepers[i].wake_us < next) next = s->sleepers[i].wake_us;
        }
    }
    return next;
}

typedef struct {
    sb_script_t *s;
    int ran;
} due_args_t;

static int due_p(lua_State *L) {
    due_args_t *a = lua_touserdata(L, 1);
    sb_script_t *s = a->s;
    for (int guard_n = 0; guard_n < 64 && s->state == SB_RUNNING; guard_n++) {
        int64_t now = s->rt->pf.now_us();
        int ti = -1, si = -1;
        int64_t best = now;
        for (int i = 0; i < s->ntimers; i++) {
            if (s->timers[i].next_us <= best) best = s->timers[i].next_us, ti = i;
        }
        for (int i = 0; i < s->nsleepers; i++) {
            if (s->sleepers[i].wake_us <= best) best = s->sleepers[i].wake_us, si = i, ti = -1;
        }
        if (si >= 0) {
            sb_sleeper_t sl = s->sleepers[si];
            s->sleepers[si] = s->sleepers[--s->nsleepers];
            int nargs = 0;
            if (sl.token) {   /* a device command that never answered */
                luaL_pushfail(L);
                lua_pushstring(L, "timed out");
                lua_pushstring(L, "timeout");
                lua_xmove(L, sl.co, 3);
                nargs = 3;
            }
            resume_thread(s, sl.co, sl.ref, nargs);
        } else if (ti >= 0) {
            sb_timer_t *t = &s->timers[ti];
            if (t->period_us) {
                t->next_us += t->period_us;
                if (t->next_us <= now) t->next_us = now + t->period_us; /* don't catch up */
                if (t->running) continue;                              /* last run still waiting */
                lua_rawgeti(L, LUA_REGISTRYINDEX, t->fn_ref);
                spawn(s, 0, t->id);
            } else {
                int ref = t->fn_ref;
                *t = s->timers[--s->ntimers];
                lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
                spawn(s, 0, 0);
            }
        } else {
            break;
        }
        a->ran++;
    }
    return 0;
}

typedef struct {
    sb_script_t *s;
    int64_t token;
    const cJSON *result;
    bool found;
} async_args_t;

static int async_p(lua_State *L) {
    async_args_t *a = lua_touserdata(L, 1);
    sb_script_t *s = a->s;
    for (int i = 0; i < s->nsleepers; i++) {
        if (s->sleepers[i].token != a->token) continue;
        sb_sleeper_t sl = s->sleepers[i];
        s->sleepers[i] = s->sleepers[--s->nsleepers];
        a->found = true;
        /* Converted on the main thread, which is protected; then handed over. */
        int n = push_result(L, a->result);
        lua_xmove(L, sl.co, n);
        resume_thread(s, sl.co, sl.ref, n);
        return 0;
    }
    return 0;
}

bool sb_async_result(sb_runtime_t *rt, int64_t token, const cJSON *result) {
    for (sb_script_t *s = rt->scripts; s; s = s->next) {
        async_args_t a = {s, token, result, false};
        run_protected(s, async_p, &a);
        if (a.found) return true;
    }
    return false;
}

int sb_run_due(sb_runtime_t *rt) {
    int ran = 0;
    for (sb_script_t *s = rt->scripts; s; s = s->next) {
        due_args_t a = {s, 0};
        run_protected(s, due_p, &a);
        ran += a.ran;
    }
    return ran;
}

static const char *state_name(sb_state_t st) {
    switch (st) {
    case SB_RUNNING: return "running";
    case SB_STOPPED: return "stopped";
    case SB_FINISHED: return "finished";
    default: return "failed";
    }
}

cJSON *sb_list(sb_runtime_t *rt) {
    cJSON *arr = cJSON_CreateArray();
    for (sb_script_t *s = rt->scripts; s; s = s->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddStringToObject(o, "state", state_name(s->state));
        if (s->state == SB_FAILED || s->state == SB_STOPPED) cJSON_AddStringToObject(o, "error", s->error);
        cJSON_AddNumberToObject(o, "mem", (double)s->mem_used);
        cJSON_AddNumberToObject(o, "mem_peak", (double)s->mem_peak);
        cJSON_AddNumberToObject(o, "mem_base", (double)s->mem_base);
        cJSON_AddNumberToObject(o, "mem_limit", (double)rt->lim.mem_per_script);
        cJSON_AddNumberToObject(o, "timers", s->ntimers);
        cJSON_AddNumberToObject(o, "handlers", s->handlers);
        cJSON_AddNumberToObject(o, "waiting", s->nsleepers);
        cJSON_AddNumberToObject(o, "errors", s->errors);
        cJSON_AddNumberToObject(o, "cpu_ms", (double)(s->cpu_us / 1000));
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *sb_logs(sb_runtime_t *rt, const char *name) {
    sb_script_t *s = find(rt, name);
    if (!s) return NULL;
    cJSON *arr = cJSON_CreateArray();
    int first = (s->log_next - s->log_count + LOG_LINES) % LOG_LINES;
    for (int i = 0; i < s->log_count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s->log[(first + i) % LOG_LINES]));
    }
    return arr;
}
