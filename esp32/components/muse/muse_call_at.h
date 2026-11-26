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
 * The bench's timed call (console call.at=DELAY_S,LEN_S, automation, RAM only).
 * A phone that holds the BLE link leaves no way to start a call from the Nordic UART, so the call is armed
 * before it connects: it starts DELAY_S after the next time a Live call client becomes ready (a rising
 * edge of muse_call_available), lasts LEN_S, ends itself, and fires once. call.at=0 disarms; call.at? reports.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool armed;
    bool strict;       /* once only: a client that goes before the start gives it up (MUSE_CALL_AT_GIVE_UP) */
    bool was_available;
    uint32_t delay_s, len_s;
    int64_t fire_us;   /* when the call starts (0: waiting for a client to become ready) */
    int64_t end_us;    /* when it ends (0: not started) */
} muse_call_at_t;

typedef enum {
    MUSE_CALL_AT_NONE,
    MUSE_CALL_AT_START,
    MUSE_CALL_AT_END,
    MUSE_CALL_AT_GIVE_UP,   /* strict: the client went before the start; disarmed */
} muse_call_at_action_t;

/* Arms (len_s > 0) or disarms (len_s 0). available: whether a client is ready now (only a later one counts). */
void muse_call_at_arm(muse_call_at_t *a, uint32_t delay_s, uint32_t len_s, bool available);
/* What's due now. A client that goes away before the start puts it back to waiting for the next one (strict:
 * gives it up). */
muse_call_at_action_t muse_call_at_tick(muse_call_at_t *a, bool available, int64_t now_us);
