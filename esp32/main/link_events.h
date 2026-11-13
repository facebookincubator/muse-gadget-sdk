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

#ifdef __cplusplus
extern "C" {
#endif

// Connectivity history (Wi-Fi, Noise session, tunnel), kept apart from the
// text log ring so a burst of outage logging can't push out what caused it.
// Recorded only on transitions, so one outage costs a few entries. Lost on
// reboot; device.health and bug.report expose it so the agent can explain
// past outages ("123s ago tunnel_down: keepalive timeout ...").
// About two days of history at 50-60 events a day (each
// VM replacement costs three). bug.report carries all of it; device.health
// only the newest LINK_EVENTS_HEALTH_MAX (10-17 hours) to stay small.
#define LINK_EVENTS_CAPACITY 128
#define LINK_EVENTS_HEALTH_MAX 40
#define LINK_EVENTS_DETAIL_MAX 112

typedef enum {
    LINK_EVENT_WIFI_DOWN,
    LINK_EVENT_WIFI_UP,
    LINK_EVENT_SESSION_UP,
    LINK_EVENT_SESSION_DOWN,
    LINK_EVENT_TUNNEL_UP,
    LINK_EVENT_TUNNEL_DOWN,
    LINK_EVENT_TUNNEL_TX_BLOCKED,
    LINK_EVENT_STALL_PROBE,  // which hop failed when the session went silent
} link_event_kind_t;

// Allocates the PSRAM ring. Without PSRAM, recording is a no-op.
bool link_events_init(void);

// Records an event with the current DMA headroom. Callable from any task (not
// ISRs); never logs.
void link_events_record(link_event_kind_t kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Adds an array `name` of the newest `max` events to `parent`, oldest first:
// "<N>s ago <kind>: <detail> (dma_free=<B> dma_largest=<B>)".
struct cJSON;
bool link_events_add_json(struct cJSON *parent, const char *name, size_t max);

#ifdef __cplusplus
}
#endif
