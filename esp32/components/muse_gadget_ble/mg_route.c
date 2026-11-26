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

#include "mg_route.h"

#include <stddef.h>

mg_route_t mg_route_decide(mg_policy_t policy, const mg_route_inputs_t *in)
{
    if (!in || policy == MG_POLICY_WIFI_ONLY) {
        return MG_ROUTE_WIFI;
    }
    bool ble = in->client_connected && in->client_ready && in->ptt_enabled && !in->unproven;
    /* A set-up gadget's press during the connect burst (the link up, the client not proven or
     * ready yet) is a held live turn, ahead of Wi-Fi only where Wi-Fi can't take it. */
    bool held = in->client_connected && in->held_ok && !ble;
    /* The queue is for clips recorded while no client is connected. */
    bool queue = in->queue_available && in->queue_enabled && !in->client_connected;
    if (ble) {
        return MG_ROUTE_BLE;
    }
    if (policy == MG_POLICY_BLE_ONLY) {
        return held ? MG_ROUTE_HELD : queue ? MG_ROUTE_QUEUE : MG_ROUTE_NONE;
    }
    if (in->wifi_ready) {
        return MG_ROUTE_WIFI;
    }
    if (held) {
        return MG_ROUTE_HELD;
    }
    /*
     * Neither is up. The user turned the queue on from their mg client, so
     * that's where they expect the clip; otherwise the Wi-Fi path decides
     * (it saves notes for later, or says why it can't).
     */
    return queue ? MG_ROUTE_QUEUE : MG_ROUTE_WIFI;
}

const char *mg_route_name(mg_route_t route)
{
    switch (route) {
    case MG_ROUTE_WIFI: return "wifi";
    case MG_ROUTE_BLE: return "ble";
    case MG_ROUTE_QUEUE: return "queue";
    case MG_ROUTE_HELD: return "held";
    default: return "none";
    }
}
