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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where one push-to-talk utterance goes. A pure function of the moment's
 * state, decided once per press, so the host tests pin it down.
 */

typedef enum {
    MG_POLICY_AUTO,        /* BLE when an mg client takes push-to-talk, else Wi-Fi */
    MG_POLICY_BLE_ONLY,    /* never Wi-Fi: BLE, else the offline queue, else nothing */
    MG_POLICY_WIFI_ONLY,   /* always the existing Wi-Fi path, as without BLE audio */
} mg_policy_t;

typedef enum {
    MG_ROUTE_WIFI,    /* the existing path to Muse (it keeps its own offline notes) */
    MG_ROUTE_BLE,     /* live to the mg client */
    MG_ROUTE_QUEUE,   /* recorded to the offline clip queue for an mg client */
    MG_ROUTE_NONE,    /* nowhere can take it: the press is refused */
    /* The held live turn: a client's link is up but it isn't proven or ready
     * yet (the connect burst): captured and held, live once it is, else the clip queue after 3 s. */
    MG_ROUTE_HELD,
} mg_route_t;

typedef struct {
    bool client_connected;   /* an mg client is connected at all */
    bool client_ready;       /* ...and subscribed to Control and Data */
    bool ptt_enabled;        /* ...and set push_to_talk_enabled */
    bool wifi_ready;         /* the Wi-Fi path can start a turn now */
    bool queue_available;    /* the queue partition exists and has room */
    bool queue_enabled;      /* the audio_queue_enabled setting */
    bool unproven;           /* the client hasn't passed this connection's token proof (no bench switch) */
    bool held_ok;            /* a press may become a held live turn (set up: a proof key) */
} mg_route_inputs_t;

mg_route_t mg_route_decide(mg_policy_t policy, const mg_route_inputs_t *in);

const char *mg_route_name(mg_route_t route);

#ifdef __cplusplus
}
#endif
