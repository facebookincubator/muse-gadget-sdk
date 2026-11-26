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

#ifndef MG_HAPTICS_H
#define MG_HAPTICS_H
/*
 * The haptics core (mgcommands.h, Haptics): pure C11, no SDK, no allocation.
 * The board gives it time in ms, its actuator's capability (setting 15) and the states it already tracks (update,
 * battery, a talk turn, haptics_enabled); the core parses and checks requests, plays patterns step by step, keeps both
 * duty windows, arbitrates the sources and builds the client's statuses. The board drives the actuator from
 * mgh_motor() after every mgh_play / mgh_tick / mgh_stop / mgh_interrupt and calls mgh_tick again after *next_ms.
 *
 *   haptic_play  [device_action, 15, motor, 0, id, flags, strength, repeat, kind, preset | count, count x u16]
 *   haptic_stop  [device_action, 17, motor]
 *   status       [device_action, 15, id, status, reason]
 *
 * Choices the protocol leaves open (a reference implementation plays them the same way):
 * - Validation order: n < 7 invalid_length; a non-empty mb effect list unsupported; motor invalid_value; an unknown
 *   kind, or a timing list without the timing flag, unsupported; the kind's bytes not exactly there (a preset 8 bytes,
 *   a timing list 8 + 2 x count) invalid_length; strength > 100, repeat 0, count 0 or > 16, a step of 0, an on step
 *   over the cap, a pass over MG_HAPTIC_MAX_PATTERN_MS, a request over MG_HAPTIC_MAX_REQUEST_MS invalid_value; an
 *   unknown preset unsupported. Unknown flag bits are ignored.
 * - Refusal order: updating, low_battery, asleep, disabled, in_use, duty_limit. A refused request leaves what plays
 *   playing (a replacement's "stopped, replaced" comes only when the new one plays).
 * - A pattern is done after its last pass's last step (an even list's trailing off step included), as the request's
 *   length counts it (ring x5 = 10,000 ms). Between passes of a list that ends on an on step: MG_HAPTIC_REPEAT_GAP_MS.
 * - Sources, highest first: the talk-threshold buzz, the setup confirm, a test (Developer page, console), a client's
 *   pattern. A higher one cuts a lower one; an equal one replaces it; a lower one is refused (in_use). In a talk turn
 *   only the talk buzz plays. The talk buzz and the setup confirm need haptics_enabled; a test never does; a client's
 *   pattern does unless it carries the bench flag. Only a client's pattern gets statuses.
 * - On steps shorter than the actuator's min_on_ms play min_on_ms long; checks (caps, duty) use what plays.
 * - Duty: the on time in MGH_DUTY_BUCKET_MS buckets; a window's use is the sum of the buckets it touches (at most one
 *   bucket more than the window: never less than the true use). A request that does not fit both budgets is refused;
 *   the gadget's own cues skip.
 * RAM: one mgh_t (88 B on a 32-bit target), the board's; the caps it points at stay the board's (const).
 * Host tests: haptics/tests/test_mg_haptics.py (the C core against haptics_ref.py and
 * protocol/test-vectors/mg-haptic-v1.json).
 */
#include <stdbool.h>
#include <stdint.h>
#ifndef MGH_MGCOMMANDS_H
#define MGH_MGCOMMANDS_H "../protocol/mgcommands.h"
#endif
#include MGH_MGCOMMANDS_H

#define MGH_SRC_CLIENT          0
#define MGH_SRC_TEST            1
#define MGH_SRC_SETUP           2
#define MGH_SRC_TALK            3
#define MGH_DUTY_BUCKET_MS      5000u
#define MGH_DUTY_BUCKETS        13u     /* 65 s: a minute's window touches at most 13 buckets */
#define MGH_PLAY_MIN_LEN        7u      /* motor, 0, id, flags, strength, repeat, kind */

/* The actuator: setting 15's four bytes, plus the board's own on-step cap (<= MG_HAPTIC_MAX_ON_MS). */
typedef struct {
    uint8_t kind, flags, min_on_ms, strength_steps;
    uint16_t max_on_ms;
} mgh_caps_t;

typedef struct {
    uint8_t id, flags, strength, repeat, count;
    uint16_t steps[MG_HAPTIC_MAX_STEPS];        /* ms, on first; as asked (lengthened at play) */
} mgh_req_t;

/* The states the board tracks, read at every mgh_play. */
typedef struct {
    uint8_t enabled;        /* haptics_enabled on this connection */
    uint8_t updating, low_battery, asleep, in_turn;
} mgh_env_t;

typedef struct { uint8_t id, status, reason; } mgh_status_t;

typedef struct {
    const mgh_caps_t *caps;
    uint32_t duty_slot;                         /* the newest bucket's number (now / MGH_DUTY_BUCKET_MS) */
    uint32_t t;                                 /* the current step's (or gap's) start */
    uint16_t steps[MG_HAPTIC_MAX_STEPS];        /* the playing pattern, lengthened */
    uint16_t duty[MGH_DUTY_BUCKETS];            /* on ms per bucket */
    uint16_t last_on_ms, played;                /* the last start's on time; starts since mgh_init */
    uint8_t active, src, id, count, step, passes_left, gap, on;
    uint8_t last_src, last_status, last_reason, last_count;     /* the last result, for a board's UI and console */
} mgh_t;

void mgh_init(mgh_t *h, const mgh_caps_t *caps);     /* caps: kept by pointer */
/* Request parsing; p, n: the bytes after the action byte. 0 or an mg_error_code. */
uint16_t mgh_parse_play(const mgh_caps_t *caps, const uint8_t *p, unsigned n, mgh_req_t *r);
uint16_t mgh_parse_stop(const uint8_t *p, unsigned n);
/* A preset's steps into r (count, steps; the rest untouched); false for an unknown preset. */
bool mgh_preset(uint8_t preset, mgh_req_t *r);
/* A request's on time as it would play (lengthened, every pass). */
uint32_t mgh_on_ms(const mgh_t *h, const mgh_req_t *r);

/* Start r from src. Statuses for the client into out (at most 2: the replaced pattern's, then this one's); returns
 * their count. */
unsigned mgh_play(mgh_t *h, const mgh_req_t *r, uint8_t src, const mgh_env_t *e, uint32_t now, mgh_status_t out[2]);
/* haptic_stop: the client's pattern stops (stopped, none); nothing of the client's playing: no status (0). */
unsigned mgh_stop(mgh_t *h, uint32_t now, mgh_status_t out[1]);
/* Stop whatever plays for a test (the Developer page's Stop, a key): never a gadget cue; a client's pattern gets
 * nothing (only the client stops it): 1 if a test stopped. */
bool mgh_stop_test(mgh_t *h, uint32_t now);
/* The device cuts what plays: reason updating / low_battery (everything, cues too) or in_use (a turn's button_down:
 * everything but the talk buzz). A client's pattern gets (interrupted, reason). */
unsigned mgh_interrupt(mgh_t *h, uint8_t reason, uint32_t now, mgh_status_t out[1]);
/* The link dropped: the client's pattern stops, no status. */
void mgh_link_drop(mgh_t *h, uint32_t now);
/* Advance to now: a client's pattern that ended gives (done, none). *next_ms: ms until the next step (0: idle). */
unsigned mgh_tick(mgh_t *h, uint32_t now, mgh_status_t out[1], uint32_t *next_ms);
static inline bool mgh_motor(const mgh_t *h) { return h->active && h->on; }
/* Duty: the on time in the buckets a window of window_ms back from now touches (the step now on included), and on
 * time spent outside the core (a console pulse) added at now. */
uint32_t mgh_duty_used(mgh_t *h, uint32_t now, uint32_t window_ms);
void mgh_duty_add(mgh_t *h, uint32_t now, uint32_t ms);
#endif
