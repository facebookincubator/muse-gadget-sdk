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
 * The user's Settings mic gain, the parts with no IDF
 * in them, for the host tests.
 *
 * - The console words (muse_input.c's console; !IN over the Nordic UART):
 *   mic.gain? (a diagnostic) and mic.gain=N (automation: the user's Settings
 *   gain, as the slider sets it: 0 to MUSE_MIC_GAIN_MAX in its 3 dB steps).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_settings.h"

typedef enum {
    MUSE_MIC_WORD_NONE,     /* not a mic.gain word */
    MUSE_MIC_WORD_QUERY,    /* mic.gain? */
    MUSE_MIC_WORD_SET,      /* mic.gain=N: *db, clamped and on the slider's step */
    MUSE_MIC_WORD_BAD,      /* mic.gain= without a number */
} muse_mic_word_t;

#define MUSE_MIC_GAIN_STEP_DB 3   /* the Settings slider's step */

static inline muse_mic_word_t muse_mic_gain_word(const char *line, int *db)
{
    if (!strcmp(line, "mic.gain?")) {
        return MUSE_MIC_WORD_QUERY;
    }
    if (strncmp(line, "mic.gain=", 9)) {
        return MUSE_MIC_WORD_NONE;
    }
    const char *v = line + 9;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || strlen(v) > 6) {
        return MUSE_MIC_WORD_BAD;
    }
    n = n < 0 ? 0 : n > MUSE_MIC_GAIN_MAX ? MUSE_MIC_GAIN_MAX : n;
    n = (n + MUSE_MIC_GAIN_STEP_DB / 2) / MUSE_MIC_GAIN_STEP_DB * MUSE_MIC_GAIN_STEP_DB;   /* the nearest step */
    *db = (int)(n > MUSE_MIC_GAIN_MAX ? MUSE_MIC_GAIN_MAX : n);
    return MUSE_MIC_WORD_SET;
}

/* The answer on mg.bench: the user's stored gain, the connection's override
 * (-1: none) and the gain captures use. */
static inline int muse_mic_gain_status(char *out, size_t cap, int user, int override, int effective)
{
    char conn[24];
    if (override >= 0) {
        snprintf(conn, sizeof(conn), "%d dB", override);
    } else {
        snprintf(conn, sizeof(conn), "none");
    }
    return snprintf(out, cap, "mic gain: settings %d dB, connection %s, effective %d dB", user, conn, effective);
}
