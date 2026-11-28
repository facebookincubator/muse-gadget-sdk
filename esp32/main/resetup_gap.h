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
#include <stdint.h>

/*
 * The re-setup prompt's rate (protocols/README.md, Gadget setup v2: The re-setup prompt's rate), per connection: on
 * a set-up gadget a pairing_client_hello on the prompt's own connection takes the prompt over (bound to the newest
 * hello, the one press confirms that hello); one from another connection while the prompt is up gets
 * error_operation_in_progress, in plaintext, and shows nothing. With no prompt up, only a hello on the connection of
 * a dismiss (the back gesture, or a talk press on a re-setup prompt) within RESETUP_PROMPT_GAP_US of it is refused;
 * test and bench builds (exempt) are exempt from that rule only. A gadget that isn't set up has no rate. conn: the
 * BLE connection's sequence number (a reconnect is a new one; 0: none). Pure: ble_server.c keeps the state, the host
 * tests run it.
 */
#define RESETUP_PROMPT_GAP_US (10LL * 1000 * 1000)

typedef enum {
    RESETUP_SHOW = 0,   /* the prompt may show, bound to this hello */
    RESETUP_TAKEOVER,   /* the prompt up on this connection is now this hello's */
    RESETUP_BUSY,       /* error_operation_in_progress, nothing shows */
} resetup_hello_t;

typedef struct {
    uint32_t prompt_conn;      /* the connection the prompt is up on; 0: none */
    uint32_t dismissed_conn;   /* the connection of the last dismiss; 0: none */
    int64_t dismissed_us;
} resetup_gap_t;

static inline resetup_hello_t resetup_hello(const resetup_gap_t *g, bool exempt, bool set_up, uint32_t conn,
                                            int64_t now_us)
{
    if (!set_up) {
        return RESETUP_SHOW;
    }
    if (g->prompt_conn != 0) {
        return g->prompt_conn == conn ? RESETUP_TAKEOVER : RESETUP_BUSY;
    }
    if (!exempt && g->dismissed_conn != 0 && g->dismissed_conn == conn && now_us - g->dismissed_us < RESETUP_PROMPT_GAP_US) {
        return RESETUP_BUSY;
    }
    return RESETUP_SHOW;
}

/* A prompt showed on conn. */
static inline void resetup_gap_shown(resetup_gap_t *g, uint32_t conn)
{
    g->prompt_conn = conn;
}

/* The prompt ended: dismissed (the back gesture, a talk press on a re-setup prompt) starts the 10 s rule on its
 * connection; a confirm, the phone giving up or a link drop doesn't. */
static inline void resetup_gap_ended(resetup_gap_t *g, bool dismissed, int64_t now_us)
{
    if (dismissed && g->prompt_conn != 0) {
        g->dismissed_conn = g->prompt_conn;
        g->dismissed_us = now_us;
    }
    g->prompt_conn = 0;
}

/*
 * Setup mode (protocols/README.md, The press): a set-up gadget whose only key is its talk key takes a re-setup only
 * in it. It is the key pressed at power-on (within MUSE_BOOTGUARD_ENTRY_WINDOW_MS of boot) and let go before the SAFE
 * hold (muse_bootguard_setup_press), on a normal boot, and lasts until the confirmed re-setup (ended) or
 * SETUP_MODE_US after that boot. SAFE of either kind is never setup mode. Test and bench builds (exempt) are always in
 * it. now_us: the time since boot.
 */
#define SETUP_MODE_US (5LL * 60 * 1000 * 1000)

static inline bool setup_mode_now(bool exempt, bool safe, bool pressed, bool ended, int64_t now_us)
{
    return exempt || (!safe && pressed && !ended && now_us < SETUP_MODE_US);
}
