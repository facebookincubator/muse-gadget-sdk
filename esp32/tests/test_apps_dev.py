# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Devices for apps (API 1.0): the registry (components/capabilities_registry), the dev_*
calls (components/muse_apps/muse_apps_dev.c), compiled on the host against
fakes; and every device a
board file or driver defines, checked against the well-known names in
components/muse_apps/sdk/api/api.json. Nothing here names a board."""

import json
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
DEV = ROOT / "components/capabilities_registry"
APPS = ROOT / "components/muse_apps"
API = json.loads((ROOT / "components/muse_apps/sdk/api/api.json").read_text())


def read(path):
    return (ROOT / path).read_text()


def compile_and_run(test, tmp, name, sources, includes, cases):
    """Builds sources (paths or (name, text) pairs) into one program and runs
    it once per case, with the case number as its argument."""
    paths = []
    for src in sources:
        if isinstance(src, tuple):
            p = Path(tmp) / src[0]
            p.write_text(src[1])
            paths.append(str(p))
        else:
            paths.append(str(src))
    exe = Path(tmp) / name
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    result = subprocess.run(cc + flags + ["-I" + str(i) for i in includes] + paths + ["-o", str(exe), "-lm"],
                            capture_output=True, text=True)
    test.assertEqual(result.returncode, 0, result.stderr)
    for case in range(cases):
        with test.subTest(case=case):
            run = subprocess.run([str(exe), str(case)], capture_output=True, text=True, timeout=60)
            test.assertEqual(run.returncode, 0, run.stdout + run.stderr)


REGISTRY_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capabilities_registry.h"

static const capability_field_t fields[] = {{"a", "", 0, 0}, {"b", "%", 0, 100}};
static const char *const events[] = {"tap", "flip"};
static capability_t dev(const char *name)
{
    capability_t d = {.name = name, .desc = "test", .fields = fields, .n_fields = 2, .rate = 10,
                    .events = events, .n_events = 2};
    return d;
}

static const capability_t *got_dev;
static int64_t got_t;
static float got_v[CAPABILITY_FIELDS + 2];
static int got_n, got_event = -1, data_calls, event_calls;
static void on_data(const capability_t *d, int64_t t_us, const float *v, int n)
{
    got_dev = d; got_t = t_us; got_n = n; data_calls++;
    memcpy(got_v, v, sizeof(float) * n);
}
static void on_event(const capability_t *d, int e) { got_dev = d; got_event = e; event_calls++; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    int test = atoi(argv[1]);
    static capability_t devs[CAPABILITY_MAX + 2];
    static char names[CAPABILITY_MAX + 2][8];
    if (test == 0) {   /* registering, finding */
        devs[0] = dev("battery");
        assert(capabilities_registry_count() == 0 && !capabilities_registry_find("battery") && !capabilities_registry_at(0));
        assert(capabilities_registry_register(&devs[0]));
        assert(capabilities_registry_count() == 1 && capabilities_registry_at(0) == &devs[0] && capabilities_registry_find("battery") == &devs[0]);
        assert(!capabilities_registry_at(1) && !capabilities_registry_at(-1) && !capabilities_registry_find("accel") && !capabilities_registry_find(NULL));
        devs[1] = dev("battery");
        assert(!capabilities_registry_register(&devs[1]) && capabilities_registry_count() == 1);   /* name taken */
    } else if (test == 1) {   /* descriptions breaking the rules */
        const char *bad[] = {"", "Accel", "soil-moisture", "a b", "0123456789012345678901234567890123"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            devs[i] = dev(bad[i]);
            assert(!capabilities_registry_register(&devs[i]));
        }
        assert(!capabilities_registry_register(NULL));
        capability_t d = dev("too_many");
        d.n_fields = CAPABILITY_FIELDS + 1;
        assert(!capabilities_registry_register(&d));
        d = dev("no_fields");
        d.fields = NULL;
        assert(!capabilities_registry_register(&d));
        d = dev("no_events");
        d.events = NULL;
        assert(!capabilities_registry_register(&d));
        static const capability_field_t p9[9];
        static const capability_action_t many[] = {{"go", "", p9, 9, false}};
        d = dev("many_params");
        d.actions = many;
        d.n_actions = 1;
        assert(!capabilities_registry_register(&d));
        static const capability_action_t badname[] = {{"Go", "", NULL, 0, false}};
        d.actions = badname;
        assert(!capabilities_registry_register(&d));
        assert(capabilities_registry_count() == 0);
    } else if (test == 2) {   /* full */
        for (int i = 0; i < CAPABILITY_MAX + 1; i++) {
            snprintf(names[i], sizeof(names[i]), "d%d", i);
            devs[i] = dev(names[i]);
            assert(capabilities_registry_register(&devs[i]) == (i < CAPABILITY_MAX));
        }
        assert(capabilities_registry_count() == CAPABILITY_MAX);
    } else if (test == 3) {   /* pushes and events reach the listener */
        devs[0] = dev("soil");
        assert(capabilities_registry_register(&devs[0]));
        float v[CAPABILITY_FIELDS + 2] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        capabilities_registry_push(&devs[0], v, 2);
        capabilities_registry_event(&devs[0], 0);
        assert(data_calls == 0 && event_calls == 0);   /* nobody listening */
        capabilities_registry_set_listener(on_data, on_event);
        capabilities_registry_push(&devs[0], v, 2);
        assert(data_calls == 1 && got_dev == &devs[0] && got_n == 2 && got_v[1] == 2 && got_t > 0);
        capabilities_registry_push(&devs[0], v, CAPABILITY_FIELDS + 2);
        assert(data_calls == 2 && got_n == CAPABILITY_FIELDS && got_v[7] == 8);
        capabilities_registry_push(&devs[0], v, 0);
        capabilities_registry_push(&devs[0], NULL, 2);
        assert(data_calls == 2);
        capabilities_registry_event(&devs[0], 1);
        assert(event_calls == 1 && got_event == 1);
        capabilities_registry_event(&devs[0], 2);
        capabilities_registry_event(&devs[0], -1);
        assert(event_calls == 1);
        capabilities_registry_set_listener(NULL, NULL);
        capabilities_registry_push(&devs[0], v, 2);
        assert(data_calls == 2);
    }
    return 0;
}
'''


# The runtime's side, faked: the app's manifest, its event queue, its budget,
# time and timers, and a registry the test fills.
APPS_FAKES = {
    "wasm_export.h": "#pragma once\ntypedef void *wasm_exec_env_t;\n",
    "cJSON.h": r'''#pragma once
typedef struct cJSON { const char *text; } cJSON;
char *cJSON_PrintUnformatted(const cJSON *item);
void cJSON_Delete(cJSON *item);
void cJSON_free(void *p);
''',
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_STATE 0x103\n",
    "esp_log.h": "#pragma once\n#include <stdio.h>\n#define ESP_LOGW(tag, ...) ((void)(tag), fprintf(stderr, __VA_ARGS__), fputc('\\n', stderr))\n"
                 "#define ESP_LOGI ESP_LOGW\n#define ESP_LOGE ESP_LOGW\n",
    "esp_timer.h": r'''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct fake_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; int dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
''',
    "freertos/FreeRTOS.h": "#pragma once\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n"
                           "#define taskENTER_CRITICAL(m) ((void)(m))\n#define taskEXIT_CRITICAL(m) ((void)(m))\n",
}

APPS_HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "muse_apps_priv.h"

/* ---- Fakes ---- */
static int64_t now_us = 1000000;
int64_t esp_timer_get_time(void) { return now_us; }
struct fake_timer { void (*cb)(void *); int armed; uint64_t at; };
static struct fake_timer timer;
static int timers_made;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{ timer.cb = a->callback; timers_made++; *out = &timer; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{ if (t->armed) return ESP_ERR_INVALID_STATE; t->armed = 1; t->at = now_us + us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { t->armed = 0; return ESP_OK; }
static void run_timer(void) { assert(timer.armed); now_us = (int64_t)timer.at; timer.armed = 0; timer.cb(NULL); }

static const char *allowed[8];
bool muse_apps_device_allowed(const char *name)
{ for (int i = 0; i < 8 && allowed[i]; i++) if (!strcmp(allowed[i], name)) return true; return false; }

static int posts, post_type[512], post_a[512], post_b[512];
bool muse_apps_post(int type, int a, int b)
{ assert(posts < 512); post_type[posts] = type; post_a[posts] = a; post_b[posts++] = b; return true; }

static long live_bytes;
static int allocs;
void *muse_apps_alloc(size_t n) { size_t *p = malloc(n + sizeof(size_t)); *p = n; live_bytes += n; allocs++; return p + 1; }
void *muse_apps_calloc(size_t n, size_t size) { void *p = muse_apps_alloc(n * size); memset(p, 0, n * size); return p; }
void muse_apps_free(void *p) { if (!p) return; size_t *b = (size_t *)p - 1; live_bytes -= *b; free(b); }

static cJSON described = {"{\"name\":\"accel\",\"desc\":\"a fake description\"}"};
cJSON *muse_apps_describe_device(const capability_t *d) { (void)d; return &described; }
char *cJSON_PrintUnformatted(const cJSON *item) { return strdup(item->text); }
void cJSON_Delete(cJSON *item) { assert(!item || item == &described); }
void cJSON_free(void *p) { free(p); }

/* The registry, as capabilities_registry.c keeps it. */
static const capability_t *reg[8];
static int n_reg;
static capability_data_fn data_fn;
static capability_event_fn event_fn;
int capabilities_registry_count(void) { return n_reg; }
const capability_t *capabilities_registry_at(int i) { return i >= 0 && i < n_reg ? reg[i] : NULL; }
const capability_t *capabilities_registry_find(const char *name)
{ for (int i = 0; i < n_reg; i++) if (!strcmp(reg[i]->name, name)) return reg[i]; return NULL; }
void capabilities_registry_set_listener(capability_data_fn d, capability_event_fn e) { data_fn = d; event_fn = e; }

/* ---- Drivers ---- */
static int starts[8], stops[8], start_rate[8], start_result;
static float reading[3] = {0.5f, -0.25f, 1.0f};
static int have_reading = 1;
static int calls, call_action, call_n, call_result;
static float call_p[8];
static const char *call_text;
static int idx(const capability_t *d) { for (int i = 0; i < n_reg; i++) if (reg[i] == d) return i; return -1; }
static int f_read(const capability_t *d, float *v) { (void)d; if (!have_reading) return -1; memcpy(v, reading, sizeof(reading)); return 3; }
static int f_start(const capability_t *d, int rate) { starts[idx(d)]++; start_rate[idx(d)] = rate; return start_result; }
static void f_stop(const capability_t *d) { stops[idx(d)]++; }
static int f_call(const capability_t *d, int action, const float *p, int n, const char *text)
{ (void)d; calls++; call_action = action; call_n = n; memcpy(call_p, p, sizeof(float) * n); call_text = text; return call_result; }

static const capability_field_t xyz[] = {{"x", "g", -8, 8}, {"y", "g", -8, 8}, {"z", "g", -8, 8}};
static const char *const accel_events[] = {"tap", "double_tap", "shake", "face_up", "face_down"};
static const capability_field_t buzz_ms[] = {{"ms", "ms", 1, 1000}};
static const capability_field_t free_param[] = {{"level", "", 0, 0}};
static const capability_action_t motor_actions[] = {{"buzz", "", buzz_ms, 1, false}, {"say", "", free_param, 1, true}};
static capability_t devs[6] = {
    {.name = "accel", .fields = xyz, .n_fields = 3, .rate = 100, .events = accel_events, .n_events = 5,
     .read = f_read, .start = f_start, .stop = f_stop},
    {.name = "vibration", .actions = motor_actions, .n_actions = 2, .call = f_call},
    {.name = "gyro", .fields = xyz, .n_fields = 3, .rate = 100, .read = f_read, .start = f_start, .stop = f_stop},
    {.name = "steps", .fields = xyz, .n_fields = 1, .rate = 1, .read = f_read, .start = f_start, .stop = f_stop},
    {.name = "battery", .fields = xyz, .n_fields = 3, .rate = 0, .read = f_read},
    {.name = "secret", .fields = xyz, .n_fields = 3, .rate = 10, .read = f_read, .start = f_start, .stop = f_stop},
};

typedef struct { int32_t t_ms, count; float v[8]; } reading_t;

static void push(int dev, float x)
{
    float v[3] = {x, 0, 0};
    data_fn(&devs[dev], now_us, v, 3);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    int test = atoi(argv[1]);
    for (int i = 0; i < 6; i++) reg[n_reg++] = &devs[i];
    allowed[0] = "accel"; allowed[1] = "vibration"; allowed[2] = "gyro"; allowed[3] = "steps"; allowed[4] = "battery";
    muse_apps_dev_start();
    char name[8];
    reading_t r[80];
    if (test == 0) {   /* listing and finding */
        assert(gadget_dev_count(NULL) == 6);
        assert(gadget_dev_name(NULL, 5, name, sizeof(name)) == 6 && !strcmp(name, "secret"));   /* listing is allowed */
        assert(gadget_dev_name(NULL, 1, name, 4) == 9 && !strcmp(name, "vib"));
        assert(gadget_dev_name(NULL, 6, name, sizeof(name)) == GADGET_ERR_ARG);
        assert(gadget_dev_name(NULL, -1, name, sizeof(name)) == GADGET_ERR_ARG);
        assert(gadget_dev_find(NULL, "accel") == 1 && gadget_dev_find(NULL, "battery") == 5);
        assert(gadget_dev_find(NULL, "secret") == GADGET_ERR_DENIED);       /* not in the manifest */
        assert(gadget_dev_find(NULL, "temperature") == GADGET_ERR_DENIED);
        allowed[5] = "temperature";
        assert(gadget_dev_find(NULL, "temperature") == GADGET_ERR_NOT_FOUND);
        /* A handle the manifest doesn't cover is no handle, guessed or not. */
        assert(gadget_dev_read(NULL, 6, r, sizeof(r[0])) == GADGET_ERR_HANDLE);
        assert(gadget_dev_open(NULL, 6, 1) == GADGET_ERR_HANDLE);
        assert(gadget_dev_describe(NULL, 6, name, sizeof(name)) == GADGET_ERR_HANDLE);
        assert(gadget_dev_field(NULL, 0, "x") == GADGET_ERR_HANDLE);
        assert(gadget_dev_close(NULL, 7) == GADGET_ERR_HANDLE);
        assert(starts[5] == 0);
    } else if (test == 1) {   /* descriptions and names */
        char buf[16];
        int n = gadget_dev_describe(NULL, 1, buf, sizeof(buf));
        assert(n == (int)strlen(described.text) && strlen(buf) == 15 && !strncmp(buf, described.text, 15));
        assert(gadget_dev_field(NULL, 1, "y") == 1 && gadget_dev_field(NULL, 1, "w") == GADGET_ERR_NOT_FOUND);
        assert(gadget_dev_event(NULL, 1, "shake") == 2 && gadget_dev_event(NULL, 1, "spin") == GADGET_ERR_NOT_FOUND);
        assert(gadget_dev_action(NULL, 2, "buzz") == 0 && gadget_dev_action(NULL, 2, "say") == 1);
        assert(gadget_dev_action(NULL, 1, "buzz") == GADGET_ERR_NOT_FOUND);
    } else if (test == 2) {   /* reading without opening */
        now_us = 5432100;
        assert(gadget_dev_read(NULL, 5, r, sizeof(r[0])) == 3);
        assert(r[0].t_ms == 5432 && r[0].count == 3 && r[0].v[0] == 0.5f && r[0].v[2] == 1.0f && r[0].v[3] == 0);
        assert(gadget_dev_read(NULL, 5, r, sizeof(r[0]) - 1) == GADGET_ERR_ARG);
        have_reading = 0;
        assert(gadget_dev_read(NULL, 5, r, sizeof(r[0])) == GADGET_ERR_NOT_FOUND);
        assert(gadget_dev_read(NULL, 2, r, sizeof(r[0])) == GADGET_ERR_NOT_FOUND);   /* the motor reads nothing */
    } else if (test == 3) {   /* opening: rates, the limit, a driver that won't start */
        assert(gadget_dev_open(NULL, 1, 101) == GADGET_ERR_ARG);
        assert(gadget_dev_open(NULL, 1, -1) == GADGET_ERR_ARG);
        assert(gadget_dev_open(NULL, 5, 1) == GADGET_ERR_ARG);   /* the battery is read on demand */
        assert(gadget_dev_open(NULL, 5, 0) == 0);
        assert(gadget_dev_open(NULL, 1, 20) == 0 && starts[0] == 1 && start_rate[0] == 20);
        assert(data_fn && event_fn && timers_made == 1);
        assert(gadget_dev_open(NULL, 1, 50) == 0 && starts[0] == 2 && start_rate[0] == 50);   /* a new rate */
        assert(gadget_dev_open(NULL, 2, 0) == 0);   /* events only, no fields: no queue */
        assert(gadget_dev_open(NULL, 3, 10) == 0);
        long before = live_bytes;
        assert(gadget_dev_open(NULL, 4, 1) == GADGET_ERR_LIMIT && live_bytes == before);
        assert(gadget_dev_close(NULL, 3) == 0 && stops[2] == 1);
        start_result = -1;
        assert(gadget_dev_open(NULL, 4, 1) == GADGET_ERR_BUSY && live_bytes == before - 64 * 40 && stops[3] == 0);
        start_result = 0;
        assert(gadget_dev_open(NULL, 4, 1) == 0);
        assert(live_bytes == 3 * 64 * 40);   /* accel, battery, steps; the motor has none */
        muse_apps_dev_stop();
        assert(live_bytes == 0 && stops[0] == 1 && stops[3] == 1 && !data_fn && !event_fn);
    } else if (test == 4) {   /* readings queue; EV_DEV_DATA at most once a frame */
        assert(gadget_dev_open(NULL, 1, 100) == 0);
        push(0, 1);
        assert(posts == 1 && post_type[0] == GADGET_EV_DEV_DATA && post_a[0] == 1 && post_b[0] == 1);
        now_us += 10000;
        push(0, 2);
        now_us += 10000;
        push(0, 3);
        assert(posts == 1 && timer.armed && timer.at == (uint64_t)(1000000 + 33000));
        run_timer();   /* the held-back readings are announced */
        assert(posts == 2 && post_a[1] == 1 && post_b[1] == 3 && !timer.armed);
        int n = gadget_dev_take(NULL, 1, r, sizeof(r[0]) * 2);   /* as many as fit */
        assert(n == 2 && r[0].v[0] == 1 && r[1].v[0] == 2 && r[0].t_ms == 1000 && r[1].t_ms == 1010 && r[0].count == 3);
        assert(gadget_dev_take(NULL, 1, r, sizeof(r)) == 1 && r[0].v[0] == 3);
        assert(gadget_dev_take(NULL, 1, r, sizeof(r)) == 0);
        /* 70 more, faster than frames: the oldest go, 64 stay. */
        for (int i = 0; i < 70; i++) {
            now_us += 1000;
            push(0, 100 + i);
        }
        assert(gadget_dev_take(NULL, 1, r, sizeof(r)) == 64 && r[0].v[0] == 106 && r[63].v[0] == 169);
        for (int i = 1; i < posts; i++) assert(post_type[i] != GADGET_EV_DEV_DATA || post_a[i] == 1);
        /* Not open: pushes go nowhere. */
        data_fn(&devs[2], now_us, reading, 3);
        assert(gadget_dev_take(NULL, 3, r, sizeof(r)) == 0);
    } else if (test == 5) {   /* events, only for an open device */
        assert(gadget_dev_open(NULL, 1, 0) == 0);
        event_fn(&devs[0], 2);
        assert(posts == 1 && post_type[0] == GADGET_EV_DEV_EVENT && post_a[0] == 1 && post_b[0] == 2);
        event_fn(&devs[2], 0);
        assert(posts == 1);
        assert(gadget_dev_close(NULL, 1) == 0 && stops[0] == 1);
        event_fn(&devs[0], 2);
        push(0, 1);
        assert(posts == 1);
        assert(gadget_dev_close(NULL, 1) == 0 && stops[0] == 1);   /* closing twice stops once */
    } else if (test == 6) {   /* actions */
        float p[2] = {250, 3};
        assert(gadget_dev_call(NULL, 2, 0, p, 4, "ignored") == 0);
        assert(calls == 1 && call_action == 0 && call_n == 1 && call_p[0] == 250 && call_text == NULL);
        assert(gadget_dev_call(NULL, 2, 2, p, 4, "") == GADGET_ERR_ARG);      /* no such action */
        assert(gadget_dev_call(NULL, 2, -1, p, 4, "") == GADGET_ERR_ARG);
        assert(gadget_dev_call(NULL, 2, 0, p, 3, "") == GADGET_ERR_ARG);      /* not whole floats */
        assert(gadget_dev_call(NULL, 2, 0, p, 8, "") == GADGET_ERR_ARG);      /* more than it takes */
        p[0] = 1001;
        assert(gadget_dev_call(NULL, 2, 0, p, 4, "") == GADGET_ERR_ARG);      /* out of its range */
        p[0] = NAN;
        assert(gadget_dev_call(NULL, 2, 0, p, 4, "") == GADGET_ERR_ARG);
        assert(calls == 1);
        p[0] = -5e9f;   /* unbounded: any finite number */
        assert(gadget_dev_call(NULL, 2, 1, p, 4, "hello") == 0 && call_n == 1 && !strcmp(call_text, "hello"));
        assert(gadget_dev_call(NULL, 2, 1, NULL, 0, "") == 0 && call_n == 0);
        call_result = -3;
        assert(gadget_dev_call(NULL, 2, 1, p, 4, "x") == GADGET_ERR_FAILED);
        call_result = 7;
        assert(gadget_dev_call(NULL, 2, 1, p, 4, "x") == 7);
        assert(gadget_dev_call(NULL, 1, 0, p, 4, "") == GADGET_ERR_ARG);      /* accel has no actions */
    } else if (test == 7) {   /* the app stops: everything it opened stops too */
        assert(gadget_dev_open(NULL, 1, 10) == 0 && gadget_dev_open(NULL, 3, 10) == 0 && gadget_dev_open(NULL, 4, 1) == 0);
        push(0, 1);
        now_us += 1000;
        push(0, 2);
        assert(timer.armed);
        muse_apps_dev_stop();
        assert(stops[0] == 1 && stops[2] == 1 && stops[3] == 1 && stops[5] == 0 && live_bytes == 0);
        int before = posts;
        run_timer();   /* a timer still due finds nothing to announce */
        assert(posts == before);
        muse_apps_dev_start();   /* the next app starts clean */
        assert(gadget_dev_take(NULL, 1, r, sizeof(r)) == 0);
        assert(gadget_dev_open(NULL, 1, 10) == 0 && timers_made == 1);   /* the timer is made once */
        /* Started by dev_read alone (no reading yet): dev_close stops it, once. */
        have_reading = 0;
        int started = starts[2], stopped = stops[2];
        assert(gadget_dev_read(NULL, 3, r, sizeof(r[0])) == GADGET_ERR_NOT_FOUND && starts[2] == started + 1);
        assert(gadget_dev_close(NULL, 3) == 0 && stops[2] == stopped + 1);
        assert(gadget_dev_close(NULL, 3) == 0 && stops[2] == stopped + 1);
        muse_apps_dev_stop();
    }
    return 0;
}
'''


def c_arrays(source, ctype):
    """name -> the entries of each `static const <ctype> name[] = {...};`."""
    out = {}
    for m in re.finditer(r"static const " + re.escape(ctype) + r"\s+(\w+)\[\w*\]\s*=\s*\{(.*?)\};", source, re.S):
        out[m.group(1)] = m.group(2)
    return out


def fields_of(body):
    return [(n, u, float(lo), float(hi)) for n, u, lo, hi in
            re.findall(r'\{\s*"(\w+)",\s*"([^"]*)",\s*(-?[\d.]+),\s*(-?[\d.]+)\s*\}', body)]


def devices_in(source):
    """Each capability_t initializer in source: its name, fields, events, actions."""
    fields = {k: fields_of(v) for k, v in c_arrays(source, "capability_field_t").items()}
    events = {k: re.findall(r'"(\w+)"', v) for k, v in c_arrays(source, "char *const").items()}
    actions = {}
    for k, v in c_arrays(source, "capability_action_t").items():
        actions[k] = [(n, d, fields[p], int(c), t == "true") for n, d, p, c, t in
                      re.findall(r'\{\s*"(\w+)",\s*"([^"]*)",\s*(\w+),\s*(\d+),\s*(true|false)\s*\}', v)]
    out = {}
    for m in re.finditer(r'\.name = "(\w+)",(.*?)\n\s*\}', source, re.S):
        body = m.group(2)
        ref = lambda key: (re.search(r"\." + key + r" = (\w+)", body) or [None, None])[1]
        count = lambda key: int((re.search(r"\.n_" + key + r" = (\d+)", body) or [None, 0])[1])
        dev = {
            "fields": fields.get(ref("fields"), []), "n_fields": count("fields"),
            "events": events.get(ref("events"), []), "n_events": count("events"),
            "actions": actions.get(ref("actions"), []), "n_actions": count("actions"),
        }
        out[m.group(1)] = dev
    return out


class Registry(unittest.TestCase):
    def test_registry(self):
        with tempfile.TemporaryDirectory() as tmp:
            compile_and_run(self, tmp, "registry", [DEV / "capabilities_registry.c", ("harness.c", REGISTRY_HARNESS)],
                            [DEV / "include"], 4)


class DevCalls(unittest.TestCase):
    def test_dev_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, text in APPS_FAKES.items():
                (Path(tmp) / "fakes" / name).parent.mkdir(parents=True, exist_ok=True)
                (Path(tmp) / "fakes" / name).write_text(text)
            compile_and_run(self, tmp, "dev_calls", [APPS / "muse_apps_dev.c", ("harness.c", APPS_HARNESS)],
                            [Path(tmp) / "fakes", APPS, DEV / "include"], 8)

    def test_every_dev_function_is_defined(self):
        """Each dev_* import has its definition, with the generated prototype
        (the host build above compiles them against muse_apps_api.h too)."""
        api = (APPS / "muse_apps_api.h").read_text()
        names = re.findall(r"GADGET_FN\((dev_\w+),", api)
        self.assertEqual(len(names), 12)
        self.assertEqual(set(names), {f["name"] for f in API["functions"] if f["group"] == "devices"})
        source = (APPS / "muse_apps_dev.c").read_text()
        flat = re.sub(r"\s+", " ", source)
        for name in names:
            proto = re.search(r"^(\w+ gadget_" + name + r"\([^)]*\));", api, re.M).group(1)
            self.assertTrue(re.sub(r"\s+", " ", proto) + " {" in flat, "%s isn't defined as %s" % (name, proto))
        for hook in ("void muse_apps_dev_start(void)", "void muse_apps_dev_stop(void)"):
            self.assertIn(hook + "\n{", source)
        self.assertIn("capabilities_registry_set_listener(on_data, on_event)", source)
        self.assertIn("capabilities_registry_set_listener(NULL, NULL)", source)
        # Queues in the app's budget, nothing large and static in internal RAM.
        self.assertIn("muse_apps_calloc(QUEUE_LEN, sizeof(reading_t))", source)
        self.assertNotRegex(source, r"static reading_t \w+\[")

    def test_limits_match_api_json(self):
        limits = {k: v for k, v, _ in API["limits"]}
        source = (APPS / "muse_apps_dev.c").read_text()
        self.assertRegex(source, r"#define OPEN_DEVS %d\b" % limits["devices_open"])
        self.assertRegex(source, r"#define QUEUE_LEN %d\b" % limits["device_queue"])
        self.assertEqual(limits["device_fields"], 8)
        self.assertEqual(limits["action_params"], 8)
        self.assertIn("#define CAPABILITY_FIELDS 8", (DEV / "include/capabilities_registry.h").read_text())


def preprocessor_frames(text, pos):
    """The #if conditions in force at pos, outermost first."""
    stack = []
    for m in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)", text[:pos], re.M):
        kind, cond = m.group(1), m.group(2).strip()
        if kind in ("if", "ifdef", "ifndef"):
            stack.append(cond)
        elif kind == "elif" and stack:
            stack[-1] = cond
        elif kind == "else" and stack:
            stack[-1] = "!(" + stack[-1] + ")"
        elif kind == "endif" and stack:
            stack.pop()
    return stack


def c_function(text, name):
    """The body of static function name in text, or None."""
    m = re.search(r"^static [^;{(]*\b" + re.escape(name) + r"\([^)]*\)\s*\{", text, re.M)
    if not m:
        return None
    body = text[m.start():]
    return body[:body.index("\n}\n")]


class BoardDevices(unittest.TestCase):
    """Every device a board file or driver defines has exactly what api.json's
    well-known name gives it; boards register theirs only with
    CONFIG_CAPABILITIES_REGISTRY and build the drivers they use; callbacks don't block."""

    @classmethod
    def setUpClass(cls):
        cls.sources = {path: path.read_text() for path in sorted((MUSE / "boards").glob("*.c"))}
        cls.sources[MUSE / "board_capabilities.c"] = (MUSE / "board_capabilities.c").read_text()
        cls.common = cls.sources[MUSE / "board_capabilities.c"]
        cls.known = {d[0]: d for d in API["devices"]["well_known"]}
        cls.devices = [(path, name, dev) for path, text in cls.sources.items()
                       for name, dev in devices_in(text).items() if name in cls.known]

    def test_well_known(self):
        names = {name for _, name, _ in self.devices}
        self.assertIn("battery", names)   # board_capabilities.c, on every board that reads power
        for path, name, dev in self.devices:
            with self.subTest(device=name, source=path.name):
                (_, _, fields, actions, events) = self.known[name]
                self.assertEqual([(f[0], f[1]) for f in dev["fields"]], [tuple(f) for f in fields])
                self.assertEqual(dev["n_fields"], len(fields))
                self.assertEqual(dev["events"], events)
                self.assertEqual(dev["n_events"], len(events))
                self.assertEqual(len(dev["actions"]), len(actions))
                self.assertEqual(dev["n_actions"], len(actions))
                for got, want in zip(dev["actions"], actions):
                    self.assertEqual(got[0], want[0])
                    self.assertEqual(got[1], want[1])
                    self.assertEqual([tuple(p) for p in got[2]], [(n, u, float(lo), float(hi)) for n, u, lo, hi in want[2]])
                    self.assertEqual(got[3], len(want[2]))

    def test_registered(self):
        self.assertIn("muse_board->read_power", self.common)
        self.assertIn("capabilities_registry_register(&s_battery)", self.common)
        glue = (ROOT / "main/muse_glue.c").read_text()   # once the board is up
        self.assertLess(glue.index("muse_app_run(muse_board_get());"), glue.index("board_capabilities_init();"))
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertIn('"board_capabilities.c"', cmake)
        self.assertIn("capabilities_registry", cmake[cmake.index("set(muse_priv_requires"):])
        # A board file registers its devices only in builds with the registry,
        # and builds the chip drivers it uses in its own CMake block.
        drivers = {}   # a chip driver's register call: the sources its board must build
        for path, text in self.sources.items():
            if path.parent != MUSE / "boards" or not path.name.startswith("board_"):
                continue
            for m in re.finditer(r"\b(capabilities_registry_register)\(", text):
                with self.subTest(board=path.name, at=m.start()):
                    self.assertTrue(any("CONFIG_CAPABILITIES_REGISTRY" in f for f in preprocessor_frames(text, m.start())),
                                    "%s registers a device outside CONFIG_CAPABILITIES_REGISTRY" % path.name)
            for call, srcs in drivers.items():
                if call not in text:
                    continue
                at = cmake.index('"boards/%s"' % path.name)
                block = cmake[cmake.rfind("if(", 0, at):]
                block = block[:block.index("elseif(", 10)] if "elseif(" in block[10:] else block
                for src in srcs:
                    with self.subTest(board=path.name, driver=src):
                        self.assertIn(src, block)

    def test_callbacks_dont_block(self):
        # A device's callbacks run on the app's task: no sleeping, no bus
        # traffic of their own (hand slow work to a task or timer).
        for path, text in self.sources.items():
            for m in re.finditer(r"\.(read|start|stop|call) = (\w+),", text):
                body = c_function(text, m.group(2))
                if body is None:
                    continue
                with self.subTest(source=path.name, callback=m.group(2)):
                    self.assertNotIn("vTaskDelay", body)
                    self.assertNotRegex(body, r"\bi2c_master_(transmit|receive|transmit_receive)\(")


if __name__ == "__main__":
    unittest.main()
