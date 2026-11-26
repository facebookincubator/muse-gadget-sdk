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
 * A live call and the passcode lock (muse_call.c; no IDF here, so
 * tests/test_muse_call_lock.py runs them on the host): a call opens only on an
 * unlocked gadget, whoever starts it.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MUSE_CALL_START_GO = 0,
    MUSE_CALL_START_IN_CALL,   /* a call already asked or on: nothing */
    MUSE_CALL_START_LOCKED,    /* the keypad first ("Unlock to call"), no call asked */
    MUSE_CALL_START_UI,        /* a UI stream is on */
} muse_call_start_t;

/* A start the gadget makes (a press, the menu, a console word): may it ask the phone now? */
static inline muse_call_start_t muse_call_may_start(bool idle, bool locked, bool ui_stream)
{
    return !idle ? MUSE_CALL_START_IN_CALL : locked ? MUSE_CALL_START_LOCKED
                   : ui_stream ? MUSE_CALL_START_UI : MUSE_CALL_START_GO;
}

/* The phone's connecting or live (starts) while idle or asked: held behind the keypad while locked. */
static inline bool muse_call_keypad_holds(bool idle_or_asked, bool starts, bool locked)
{
    return idle_or_asked && starts && locked;
}

/* A call state from a central without this connection's token proof (not the answer to the
 * gadget's own pending start) shows only on an unlocked, awake gadget, and never touches the owner's call (one this
 * gadget started or accepted): true, take it (marked unproven); false, nothing at all (no wake, no keypad, no
 * change to the call on screen). */
static inline bool muse_call_unproven_shows(bool owner_call, bool locked, bool asleep)
{
    return !owner_call && !locked && !asleep;
}

/* An unproven call on screen goes, locally, once the gadget locks or its screen sleeps: it never draws over the
 * lock or keeps the screen up. */
static inline bool muse_call_unproven_closes(bool locked, bool asleep)
{
    return locked || asleep;
}

/* The call's state as one word (muse_call.c's s_call): its phase (muse_call_phase_t, 0 for none) and whether the call
 * came from an unproven central. One word, so the pair changes in one store and every reader takes both from one
 * load, from any task: a call that's ending is never seen as on and the owner's on its way out (that would skip the
 * passcode's screen-off lock). The unproven bit may be set while no call is on (client_state marks it just before the
 * phase leaves none); a reader counts it only with a call on, and the phase going to none clears it in the same
 * store. */
#define MUSE_CALL_WORD_PHASE 0x7fu
#define MUSE_CALL_WORD_UNPROVEN 0x80u

static inline unsigned muse_call_word_phase(uint32_t w)
{
    return w & MUSE_CALL_WORD_PHASE;
}

/* A call is on, and it's an unproven central's. */
static inline bool muse_call_word_unproven(uint32_t w)
{
    return muse_call_word_phase(w) != 0 && (w & MUSE_CALL_WORD_UNPROVEN);
}

/* A call is on, and it's the owner's (one this gadget started or accepted, or a proven central's). */
static inline bool muse_call_word_owner(uint32_t w)
{
    return muse_call_word_phase(w) != 0 && !(w & MUSE_CALL_WORD_UNPROVEN);
}

/* The word with a new phase: none clears the unproven bit with it. */
static inline uint32_t muse_call_word_with_phase(uint32_t w, unsigned phase)
{
    phase &= MUSE_CALL_WORD_PHASE;
    return phase ? (w & MUSE_CALL_WORD_UNPROVEN) | phase : 0;
}

static inline uint32_t muse_call_word_with_unproven(uint32_t w, bool unproven)
{
    return unproven ? w | MUSE_CALL_WORD_UNPROVEN : w & ~MUSE_CALL_WORD_UNPROVEN;
}
