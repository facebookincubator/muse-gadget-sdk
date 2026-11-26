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
 * The gadget's own deadlines for a call that doesn't get going (muse_call.c;
 * no IDF here, so tests/test_muse_call_backstop.py runs them on a fake clock):
 *
 * - No answer: a start the client never answers (still asked, nothing back)
 *   is dropped after 30 s ("No answer").
 * - The connecting backstop (mgcommands.h "Live call"): a gadget that
 *   has shown connecting
 *   for 60 s with no newer state from the client ends the call itself: it
 *   sends [live_call, end] as for the user's end, shows "Couldn't start the
 *   call" (3 s) and goes home. A backstop for a silent client: the Muse apps
 *   fail their own join at 45 s, with a reason. Every connecting the client
 *   sends (a re-sent one too: a connect that legitimately takes longer)
 *   restarts the 60 s; live, ended, failed or idle end connecting.
 */

#include <stdbool.h>
#include <stdint.h>

#define MUSE_CALL_ANSWER_MAX_US (30 * 1000000LL)
#define MUSE_CALL_CONNECTING_MAX_US (60 * 1000000LL)

typedef enum {
    MUSE_CALL_DUE_NONE = 0,
    MUSE_CALL_DUE_NO_ANSWER,   /* asked 30 s ago, never answered */
    MUSE_CALL_DUE_BACKSTOP,    /* connecting for 60 s with nothing newer */
} muse_call_due_t;

typedef struct {
    int64_t asked_us;        /* the start request */
    int64_t connecting_us;   /* the client's last connecting */
} muse_call_timers_t;

/* The gadget asked the client for a call. */
static inline void muse_call_timers_asked(muse_call_timers_t *t, int64_t now_us)
{
    t->asked_us = now_us;
}

/* The client sent connecting (the first, or again). */
static inline void muse_call_timers_connecting(muse_call_timers_t *t, int64_t now_us)
{
    t->connecting_us = now_us;
}

/* What's due now, in the call's phase: asked (waiting for an answer) or
 * connecting (the client's last state); any other phase owes nothing. */
static inline muse_call_due_t muse_call_timers_due(const muse_call_timers_t *t, bool asked, bool connecting,
                                                   int64_t now_us)
{
    if (asked && now_us - t->asked_us > MUSE_CALL_ANSWER_MAX_US) {
        return MUSE_CALL_DUE_NO_ANSWER;
    }
    if (connecting && now_us - t->connecting_us > MUSE_CALL_CONNECTING_MAX_US) {
        return MUSE_CALL_DUE_BACKSTOP;
    }
    return MUSE_CALL_DUE_NONE;
}
