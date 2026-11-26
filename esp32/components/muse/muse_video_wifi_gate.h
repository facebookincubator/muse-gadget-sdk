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
 * The Wi-Fi video port's rules for who gets heard (muse_video_wifi.c;
 * protocols/VIDEO.md, Trust), kept apart from the sockets so the host tests
 * run them (tests/test_video_untrusted.py, WifiPortGate):
 *
 * - Starts refused in a row (the view closed, a size out of range, a codec
 *   the board doesn't take) make the port stop listening to starts for a
 *   while: after MUSE_VWG_FREE_REFUSALS of them, 1 s, then each refusal
 *   after the pause doubles it, up to 30 s. A start that's taken, or the
 *   view opening, ends it. Starts during the pause get the same "busy" any
 *   start gets when the board can't take one.
 * - The port is announced (mDNS) only while the Phone camera view is open,
 *   once the network is up; a bench build that takes starts any time
 *   announces it always.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_VWG_FREE_REFUSALS 3          /* refusals before the first pause */
#define MUSE_VWG_FIRST_PAUSE_MS 1000
#define MUSE_VWG_MAX_PAUSE_MS 30000

typedef struct {
    int refused;           /* starts refused in a row */
    int pause_ms;          /* the current pause (0: none yet) */
    int64_t until_us;      /* starts are refused until then */
} muse_vwg_backoff_t;

/* A start may be looked at now (not paused). */
bool muse_vwg_start_allowed(const muse_vwg_backoff_t *b, int64_t now_us);

/* A start was refused at now: returns the pause it began in ms (to log, once
 * a step), or 0 when it began none. */
int muse_vwg_refused(muse_vwg_backoff_t *b, int64_t now_us);

/* A start was taken, or the view opened: back to no pause. */
void muse_vwg_reset(muse_vwg_backoff_t *b);

/* The mDNS announcement: add and remove are the board's (mdns_service_add,
 * mdns_service_remove); 0 is success. */
typedef struct {
    int (*add)(void *ctx);
    int (*remove)(void *ctx);
    void *ctx;
} muse_vwg_announce_ops_t;

typedef struct {
    bool ready;            /* the network and mDNS are up */
    bool wanted;           /* the view is open (or the bench build takes starts any time) */
    bool on;               /* announced */
} muse_vwg_announce_t;

/* Sets whether the network is up and whether the view wants the port, and
 * adds or removes the announcement to match: at most one call each way per
 * change. Returns whether it's announced now. */
bool muse_vwg_announce(muse_vwg_announce_t *a, bool ready, bool wanted, const muse_vwg_announce_ops_t *ops);

#ifdef __cplusplus
}
#endif
