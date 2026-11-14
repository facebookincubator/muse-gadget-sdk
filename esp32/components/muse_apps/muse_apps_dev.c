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
 * The API's dev_* group: apps reach every device the drivers registered
 * (components/capabilities_registry) through these few calls. A handle is the device's
 * place in the registry plus one, and it's only good for a device the
 * manifest names: listing every device is allowed, finding or using one
 * isn't. An open device's readings queue here (up to 64 each, oldest dropped,
 * in the app's budget) from the drivers' own tasks; the app is told with
 * EV_DEV_DATA at most once a frame per device, and gets its events as
 * EV_DEV_EVENT. Nothing here blocks: the drivers' calls mustn't either.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "muse_apps_priv.h"

static const char *TAG = "muse_apps_dev";

#define OPEN_DEVS 4          /* limits.devices_open */
#define QUEUE_LEN 64        /* limits.device_queue */
#define POST_US 33000       /* EV_DEV_DATA at most once a frame (30 a second) per device */
#define TAKE_CHUNK 8        /* readings copied out per turn of the lock */
#define TEXT_MAX 4096       /* limits.text_bytes */
#define WARM_DEVS 4          /* streaming devices dev_read started without dev_open */
#define WARM_HZ 25           /* and the rate they run at */

/* gadget_reading_t, as components/muse_apps/sdk/api/gadget.h has it. */
typedef struct {
    int32_t t_ms;
    int32_t count;
    float v[CAPABILITY_FIELDS];
} reading_t;

_Static_assert(sizeof(reading_t) == 40 && CAPABILITY_FIELDS == 8, "gadget_reading_t is 40 bytes, 8 values");
_Static_assert(CAPABILITY_PARAMS == 8, "limits.action_params");

typedef struct {
    const capability_t *dev;   /* NULL: free */
    int32_t handle;
    reading_t *queue;        /* QUEUE_LEN, from the app's budget; NULL for a device without fields */
    uint8_t head, count;
    int64_t posted_us;       /* the last EV_DEV_DATA */
    bool owed;               /* readings since then that no event announced yet */
} slot_t;

/* The app task changes the slots, under the lock; the listeners (drivers'
 * tasks) and the timer read and fill them under it. */
static slot_t s_open[OPEN_DEVS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Announces readings the once-a-frame limit held back. Made once and kept:
 * its callback may still be running when an app stops. */
static esp_timer_handle_t s_timer;
static bool s_timer_armed;   /* under the lock */

/* ---- Handles ---------------------------------------------------------------------------- */

static int index_of(const capability_t *d)
{
    for (int i = 0; i < capabilities_registry_count(); i++) {
        if (capabilities_registry_at(i) == d) {
            return i;
        }
    }
    return -1;
}

/* The device behind a handle, if the app may use it: dev_find would have
 * given it this handle. */
static const capability_t *dev_of(int32_t handle)
{
    const capability_t *d = capabilities_registry_at(handle - 1);
    return d && muse_apps_device_allowed(d->name) ? d : NULL;
}

/* App task, or under the lock. */
/* Streaming devices the app reads with dev_read but never opened: started so
 * the driver keeps a latest reading, with no queue and no events. The app task
 * only. */
static const capability_t *s_warm[WARM_DEVS];

static bool warm_has(const capability_t *d)
{
    for (int i = 0; i < WARM_DEVS; i++) {
        if (s_warm[i] == d) {
            return true;
        }
    }
    return false;
}

static void warm_drop(const capability_t *d)
{
    for (int i = 0; i < WARM_DEVS; i++) {
        if (s_warm[i] == d) {
            s_warm[i] = NULL;
        }
    }
}

static slot_t *slot_of(const capability_t *d)
{
    for (int i = 0; i < OPEN_DEVS; i++) {
        if (s_open[i].dev == d) {
            return &s_open[i];
        }
    }
    return NULL;
}

/* A string into the app's buffer, cut to fit and NUL-terminated; its full length. */
static int32_t put_text(void *buf, uint32_t cap, const char *text)
{
    size_t n = strlen(text);
    if (buf && cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(buf, text, k);
        ((char *)buf)[k] = '\0';
    }
    return (int32_t)n;
}

/* ---- What the drivers push (their tasks) ------------------------------------------------- */

static void on_timer(void *arg)
{
    (void)arg;
    struct {
        int32_t handle, waiting;
    } due[OPEN_DEVS];
    int n = 0;
    int64_t next = 0;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_timer_armed = false;
    for (int i = 0; i < OPEN_DEVS; i++) {
        slot_t *s = &s_open[i];
        if (!s->dev || !s->owed) {
            continue;
        }
        int64_t wait = POST_US - (now - s->posted_us);
        if (wait <= 0) {
            s->owed = false;
            s->posted_us = now;
            if (s->count) {
                due[n].handle = s->handle;
                due[n++].waiting = s->count;
            }
        } else if (!next || wait < next) {
            next = wait;
        }
    }
    s_timer_armed = next > 0;
    taskEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < n; i++) {
        muse_apps_post(GADGET_EV_DEV_DATA, due[i].handle, due[i].waiting);
    }
    if (next > 0) {
        esp_timer_start_once(s_timer, (uint64_t)next);
    }
}

static void on_data(const capability_t *dev, int64_t t_us, const float *values, int n)
{
    int32_t handle = 0, waiting = 0;
    int64_t arm_us = 0;
    taskENTER_CRITICAL(&s_lock);
    slot_t *s = slot_of(dev);
    if (s && s->queue) {
        reading_t *r = &s->queue[(s->head + s->count) % QUEUE_LEN];
        if (s->count == QUEUE_LEN) {
            s->head = (s->head + 1) % QUEUE_LEN;   /* full: that was the oldest */
        } else {
            s->count++;
        }
        r->t_ms = (int32_t)(t_us / 1000);   /* sys_millis()'s clock */
        r->count = n;
        for (int i = 0; i < CAPABILITY_FIELDS; i++) {
            r->v[i] = i < n ? values[i] : 0.0f;
        }
        int64_t since = t_us - s->posted_us;
        if (since >= POST_US) {
            s->posted_us = t_us;
            s->owed = false;
            handle = s->handle;
            waiting = s->count;
        } else {
            s->owed = true;
            if (!s_timer_armed && s_timer) {
                s_timer_armed = true;
                arm_us = POST_US - since;
            }
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (handle) {
        muse_apps_post(GADGET_EV_DEV_DATA, handle, waiting);
    }
    if (arm_us > 0) {
        esp_timer_start_once(s_timer, (uint64_t)arm_us);
    }
}

static void on_event(const capability_t *dev, int event)
{
    int32_t handle = 0;
    taskENTER_CRITICAL(&s_lock);
    slot_t *s = slot_of(dev);
    if (s) {
        handle = s->handle;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (handle) {
        muse_apps_post(GADGET_EV_DEV_EVENT, handle, event);
    }
}

/* App task: the slot goes first, so a reading pushed meanwhile finds nothing,
 * then the driver stops and the queue is freed. */
static void close_slot(slot_t *s)
{
    const capability_t *d = s->dev;
    reading_t *q = s->queue;
    taskENTER_CRITICAL(&s_lock);
    memset(s, 0, sizeof(*s));
    taskEXIT_CRITICAL(&s_lock);
    if (d && d->stop) {
        d->stop(d);
    }
    warm_drop(d);   /* stopped: a later dev_read starts it again */
    muse_apps_free(q);
}

/* ---- Hooks ---------------------------------------------------------------------------------- */

void muse_apps_dev_start(void)
{
    for (int i = 0; i < OPEN_DEVS; i++) {
        if (s_open[i].dev) {
            close_slot(&s_open[i]);   /* never: the last app's stop closed them */
        }
    }
}

void muse_apps_dev_stop(void)
{
    for (int i = 0; i < OPEN_DEVS; i++) {
        if (s_open[i].dev) {
            close_slot(&s_open[i]);
        }
    }
    for (int i = 0; i < WARM_DEVS; i++) {
        if (s_warm[i]) {
            if (s_warm[i]->stop) {
                s_warm[i]->stop(s_warm[i]);
            }
            s_warm[i] = NULL;
        }
    }
    capabilities_registry_set_listener(NULL, NULL);
}

/* ---- The imports ---------------------------------------------------------------------------- */

int32_t gadget_dev_count(wasm_exec_env_t env)
{
    (void)env;
    return capabilities_registry_count();
}

int32_t gadget_dev_name(wasm_exec_env_t env, int32_t index, void *buf, uint32_t buf_size)
{
    (void)env;
    const capability_t *d = capabilities_registry_at(index);
    return d ? put_text(buf, buf_size, d->name) : GADGET_ERR_ARG;
}

int32_t gadget_dev_find(wasm_exec_env_t env, const char *name)
{
    (void)env;
    if (!name || !muse_apps_device_allowed(name)) {
        return GADGET_ERR_DENIED;
    }
    int i = index_of(capabilities_registry_find(name));
    return i < 0 ? GADGET_ERR_NOT_FOUND : i + 1;
}

int32_t gadget_dev_describe(wasm_exec_env_t env, int32_t device, void *buf, uint32_t buf_size)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    cJSON *o = muse_apps_describe_device(d);
    char *text = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!text) {
        return GADGET_ERR_NO_MEMORY;
    }
    int32_t n = put_text(buf, buf_size, text);
    cJSON_free(text);
    return n;
}

int32_t gadget_dev_field(wasm_exec_env_t env, int32_t device, const char *name)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    for (int i = 0; name && i < d->n_fields; i++) {
        if (!strcmp(d->fields[i].name, name)) {
            return i;
        }
    }
    return GADGET_ERR_NOT_FOUND;
}

int32_t gadget_dev_action(wasm_exec_env_t env, int32_t device, const char *name)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    for (int i = 0; name && i < d->n_actions; i++) {
        if (!strcmp(d->actions[i].name, name)) {
            return i;
        }
    }
    return GADGET_ERR_NOT_FOUND;
}

int32_t gadget_dev_event(wasm_exec_env_t env, int32_t device, const char *name)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    for (int i = 0; name && i < d->n_events; i++) {
        if (!strcmp(d->events[i], name)) {
            return i;
        }
    }
    return GADGET_ERR_NOT_FOUND;
}

int32_t gadget_dev_read(wasm_exec_env_t env, int32_t device, void *reading, uint32_t reading_size)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    if (!reading || reading_size < sizeof(reading_t)) {
        return GADGET_ERR_ARG;
    }
    float v[CAPABILITY_FIELDS] = {0};
    int n = d->read ? d->read(d, v) : -1;
    if (n < 0) {
        /* A streaming device has a reading only while it runs: dev_read
         * without dev_open starts it, and later reads get its latest. */
        if (d->rate > 0 && d->start && !slot_of(d) && !warm_has(d)) {
            for (int i = 0; i < WARM_DEVS; i++) {
                if (!s_warm[i]) {
                    if (d->start(d, d->rate < WARM_HZ ? d->rate : WARM_HZ) == 0) {
                        s_warm[i] = d;
                    }
                    break;
                }
            }
        }
        return GADGET_ERR_NOT_FOUND;
    }
    reading_t r = {
        .t_ms = (int32_t)(esp_timer_get_time() / 1000),   /* the driver keeps it current */
        .count = n < CAPABILITY_FIELDS ? n : CAPABILITY_FIELDS,
    };
    memcpy(r.v, v, sizeof(float) * (size_t)r.count);
    memcpy(reading, &r, sizeof(r));   /* the app's memory needn't be aligned */
    return r.count;
}

int32_t gadget_dev_open(wasm_exec_env_t env, int32_t device, int32_t rate_hz)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    if (rate_hz < 0 || rate_hz > d->rate) {
        return GADGET_ERR_ARG;
    }
    slot_t *s = slot_of(d);
    bool fresh = !s;
    if (fresh) {
        s = slot_of(NULL);
        if (!s) {
            return GADGET_ERR_LIMIT;
        }
        if (!s_timer) {
            const esp_timer_create_args_t args = {.callback = on_timer, .name = "app_dev"};
            if (esp_timer_create(&args, &s_timer) != ESP_OK) {
                s_timer = NULL;
                return GADGET_ERR_NO_MEMORY;
            }
        }
        reading_t *q = NULL;
        if (d->n_fields) {
            q = muse_apps_calloc(QUEUE_LEN, sizeof(reading_t));
            if (!q) {
                return GADGET_ERR_NO_MEMORY;
            }
        }
        capabilities_registry_set_listener(on_data, on_event);
        taskENTER_CRITICAL(&s_lock);
        *s = (slot_t){.dev = d, .handle = device, .queue = q};
        taskEXIT_CRITICAL(&s_lock);
    }
    /* Again on an open device: its new rate. */
    if (d->start && d->start(d, rate_hz) < 0) {
        if (fresh) {
            reading_t *q = s->queue;
            taskENTER_CRITICAL(&s_lock);
            memset(s, 0, sizeof(*s));
            taskEXIT_CRITICAL(&s_lock);
            muse_apps_free(q);
        }
        ESP_LOGW(TAG, "%s didn't start", d->name);
        return GADGET_ERR_BUSY;
    }
    return 0;
}

int32_t gadget_dev_take(wasm_exec_env_t env, int32_t device, void *readings, uint32_t readings_size)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    uint32_t want = readings ? readings_size / sizeof(reading_t) : 0;
    uint32_t got = 0;
    reading_t chunk[TAKE_CHUNK];
    while (got < want) {
        int n = 0;
        taskENTER_CRITICAL(&s_lock);
        slot_t *s = slot_of(d);
        while (s && s->queue && s->count && n < TAKE_CHUNK && got + n < want) {
            chunk[n++] = s->queue[s->head];
            s->head = (s->head + 1) % QUEUE_LEN;
            s->count--;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (!n) {
            break;
        }
        memcpy((reading_t *)readings + got, chunk, sizeof(reading_t) * (size_t)n);
        got += n;
    }
    return (int32_t)got;
}

int32_t gadget_dev_close(wasm_exec_env_t env, int32_t device)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    slot_t *s = slot_of(d);
    if (s) {
        close_slot(s);
    } else if (warm_has(d)) {   /* started by dev_read alone: dev_close stops it too */
        if (d->stop) {
            d->stop(d);
        }
        warm_drop(d);
    }
    return 0;
}

int32_t gadget_dev_call(wasm_exec_env_t env, int32_t device, int32_t action, const void *params, uint32_t params_len,
                        const char *text)
{
    (void)env;
    const capability_t *d = dev_of(device);
    if (!d) {
        return GADGET_ERR_HANDLE;
    }
    if (action < 0 || action >= d->n_actions) {
        return GADGET_ERR_ARG;
    }
    const capability_action_t *a = &d->actions[action];
    if (params_len % sizeof(float) || params_len / sizeof(float) > a->n_params || (params_len && !params)) {
        return GADGET_ERR_ARG;
    }
    int n = (int)(params_len / sizeof(float));
    float p[CAPABILITY_PARAMS] = {0};
    if (params_len) {
        memcpy(p, params, params_len);   /* the app's memory needn't be aligned */
    }
    for (int i = 0; i < n; i++) {
        const capability_field_t *f = &a->params[i];
        if (!isfinite(p[i]) || (f->max > f->min && (p[i] < f->min || p[i] > f->max))) {
            return GADGET_ERR_ARG;
        }
    }
    if (a->text) {
        text = text ? text : "";
        if (strnlen(text, TEXT_MAX + 1) > TEXT_MAX) {
            return GADGET_ERR_ARG;
        }
    } else {
        text = NULL;
    }
    if (!d->call) {
        return GADGET_ERR_FAILED;
    }
    int r = d->call(d, action, p, n, text);
    return r < 0 ? GADGET_ERR_FAILED : r;
}
