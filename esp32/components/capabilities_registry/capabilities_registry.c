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

/* The device registry (capabilities_registry.h). */

#include "capabilities_registry.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "capabilities_registry";
#define now_us() esp_timer_get_time()
#else
#include <stdio.h>
#include <time.h>
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

/* Registered once at start-up and never removed, so readers need no lock:
 * a slot is filled before the count that covers it is published. */
static const capability_t *s_devs[CAPABILITY_MAX];
static atomic_int s_count;
static capability_data_fn _Atomic s_data;
static capability_event_fn _Atomic s_event;

static bool valid_name(const char *s)
{
    if (!s || !*s || strlen(s) > 31) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) {
            return false;
        }
    }
    return true;
}

bool capabilities_registry_register(const capability_t *dev)
{
    if (!dev || !valid_name(dev->name) || dev->n_fields > CAPABILITY_FIELDS ||
        (dev->n_fields && !dev->fields) || (dev->n_actions && !dev->actions) || (dev->n_events && !dev->events)) {
        ESP_LOGW(TAG, "device %s: bad description", dev && dev->name ? dev->name : "?");
        return false;
    }
    for (int i = 0; i < dev->n_actions; i++) {
        if (!valid_name(dev->actions[i].name) || dev->actions[i].n_params > CAPABILITY_PARAMS) {
            ESP_LOGW(TAG, "device %s: bad action %d", dev->name, i);
            return false;
        }
    }
#ifdef ESP_PLATFORM
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
#endif
    int n = atomic_load(&s_count);
    bool ok = n < CAPABILITY_MAX;
    for (int i = 0; ok && i < n; i++) {
        ok = strcmp(s_devs[i]->name, dev->name) != 0;
    }
    if (ok) {
        s_devs[n] = dev;
        atomic_store(&s_count, n + 1);
    }
#ifdef ESP_PLATFORM
    portEXIT_CRITICAL(&mux);
#endif
    if (ok) {
        ESP_LOGI(TAG, "device %s: %d fields, %d actions, %d events", dev->name, dev->n_fields, dev->n_actions,
                 dev->n_events);
    } else {
        ESP_LOGW(TAG, "device %s not registered: name taken or registry full", dev->name);
    }
    return ok;
}

int capabilities_registry_count(void)
{
    return atomic_load(&s_count);
}

const capability_t *capabilities_registry_at(int index)
{
    return index >= 0 && index < atomic_load(&s_count) ? s_devs[index] : NULL;
}

const capability_t *capabilities_registry_find(const char *name)
{
    int n = atomic_load(&s_count);
    for (int i = 0; name && i < n; i++) {
        if (!strcmp(s_devs[i]->name, name)) {
            return s_devs[i];
        }
    }
    return NULL;
}

void capabilities_registry_set_listener(capability_data_fn data, capability_event_fn event)
{
    atomic_store(&s_data, data);
    atomic_store(&s_event, event);
}

void capabilities_registry_push(const capability_t *dev, const float *values, int n)
{
    capability_data_fn fn = atomic_load(&s_data);
    if (fn && dev && values && n > 0) {
        fn(dev, now_us(), values, n > CAPABILITY_FIELDS ? CAPABILITY_FIELDS : n);
    }
}

void capabilities_registry_event(const capability_t *dev, int event)
{
    capability_event_fn fn = atomic_load(&s_event);
    if (fn && dev && event >= 0 && event < dev->n_events) {
        fn(dev, event);
    }
}
