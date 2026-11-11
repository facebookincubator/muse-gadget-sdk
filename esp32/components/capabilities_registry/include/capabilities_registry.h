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
 * Devices: the sensors, motors and other hardware the firmware has drivers
 * for, each described by its driver. A board file or driver registers its
 * device once at start-up; WebAssembly apps (dev_* in components/muse_apps/sdk/api) and device.guide
 * read the registry, so new hardware needs no change to the apps API.
 *
 * A device that is one of the well-known kinds in components/muse_apps/sdk/api/api.json (devices,
 * well_known) uses that name and exactly its fields, actions and events, in
 * that order; any other device picks its own name and describes itself.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPABILITY_MAX 16        /* devices on one gadget */
#define CAPABILITY_FIELDS 8      /* values in one reading */
#define CAPABILITY_PARAMS 8      /* numbers one action takes */

typedef struct {
    const char *name;   /* a-z, 0-9, '_' */
    const char *unit;   /* "%", "hPa", "g"; "" when it has none */
    float min, max;     /* the range it can take; equal when unbounded */
} capability_field_t;

typedef struct {
    const char *name;
    const char *desc;               /* for Muse: what it does */
    const capability_field_t *params;
    uint8_t n_params;
    bool text;                      /* takes a string as well */
} capability_action_t;

typedef struct capability capability_t;

struct capability {
    const char *name;               /* unique on the gadget: a-z, 0-9, '_' */
    const char *desc;               /* for Muse: what it is and where it sits */
    const capability_field_t *fields; /* a reading's values, in order */
    uint8_t n_fields;
    uint16_t rate;                  /* highest streaming rate in Hz; 0: read on demand only */
    const capability_action_t *actions;
    uint8_t n_actions;
    const char *const *events;      /* what it reports by itself, in order */
    uint8_t n_events;

    /* Any of these may be NULL. None may block: a slow driver keeps its
     * latest reading from a task of its own, and starts a slow action and
     * returns. They're called from the caller's task (an app's, or Link's). */

    /* The latest reading into values[n_fields]: n_fields, or negative if
     * there's none yet. */
    int (*read)(const capability_t *dev, float *values);
    /* Start pushing readings (capabilities_registry_push) at about rate_hz (0: events
     * only) and events (capabilities_registry_event). 0, or negative if it can't. */
    int (*start)(const capability_t *dev, int rate_hz);
    void (*stop)(const capability_t *dev);
    /* Run an action: 0 or more, or negative on failure. */
    int (*call)(const capability_t *dev, int action, const float *params, int n_params, const char *text);

    void *ctx;                      /* the driver's own */
};

/* At start-up, from any task. False if the registry is full, the name is
 * taken, or the description breaks the limits above. dev must stay valid. */
bool capabilities_registry_register(const capability_t *dev);

int capabilities_registry_count(void);
const capability_t *capabilities_registry_at(int index);
const capability_t *capabilities_registry_find(const char *name);

/* Where pushed readings and events go: one consumer (the apps runtime).
 * Called on the driver's task: copy and return. */
typedef void (*capability_data_fn)(const capability_t *dev, int64_t t_us, const float *values, int n);
typedef void (*capability_event_fn)(const capability_t *dev, int event);
void capabilities_registry_set_listener(capability_data_fn data, capability_event_fn event);

/* For drivers, while started: a reading, or an event (its index in events). */
void capabilities_registry_push(const capability_t *dev, const float *values, int n);
void capabilities_registry_event(const capability_t *dev, int event);

#ifdef __cplusplus
}
#endif
