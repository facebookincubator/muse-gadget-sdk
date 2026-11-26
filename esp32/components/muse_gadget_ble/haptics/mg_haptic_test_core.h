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
 * The vibration tests' logic (mg_haptic_test.h) over the haptics core: pure
 * C, no SDK, no clock of its own; the caller holds the core's lock and passes
 * the time and the gates, then drives the motor from mgh_motor() and forwards
 * the statuses a cut client pattern gets. A test is built as the bytes of a
 * haptic_play with the bench flag and checked by mgh_parse_play, so it meets
 * exactly the caps a client's request meets, then played as
 * MGH_SRC_TEST (the gates, the duty windows and the arbitration in mgh_play).
 * This module (mg_haptic_test.c) is this repo's, not part of the haptics core;
 * the core it calls is the vendored one, unchanged.
 */

#include "mg_haptic_test.h"
#include "mg_haptics.h"

typedef struct {
    mgh_t *h;
    uint8_t default_strength;  /* ptt_buzz_volume_percent, for the strength flag */
    /* The last test asked for, and what came of it. */
    uint8_t state;             /* MGHT_ST_* */
    uint8_t op, preset, reason;
    uint16_t ms;               /* a pulse: as asked */
    uint8_t id;                /* the core's id for the playing test */
    uint32_t start, end;       /* when it started; when it ends if it plays out */
} mght_t;

enum { MGHT_ST_NONE, MGHT_ST_PLAYING, MGHT_ST_DONE, MGHT_ST_STOPPED, MGHT_ST_REFUSED, MGHT_ST_CUT };

void mght_init(mght_t *t, mgh_t *h, uint8_t default_strength);
void mght_caps(const mght_t *t, mght_caps_t *c);
/* out: the statuses for a client's pattern the test cut (at most 2), *n their count. */
mght_result_t mght_run(mght_t *t, const mght_cmd_t *c, const mgh_env_t *e, uint32_t now, mgh_status_t out[2],
                       unsigned *n);
bool mght_stop(mght_t *t, uint32_t now);
/* Call after mgh_tick at now. */
void mght_status(mght_t *t, uint32_t now, mght_status_t *s);
/* A console line: 0 parsed into c (or a status question: *status set), -1 not a vib word, -2 malformed (out says
 * why). */
int mght_parse(const mght_t *t, const char *line, mght_cmd_t *c, bool *status, char *out, size_t n);
/* The whole console word: parse, run, and the answer line. */
void mght_console(mght_t *t, const char *line, const mgh_env_t *e, uint32_t now, mgh_status_t out[2], unsigned *n,
                  char *answer, size_t an);
