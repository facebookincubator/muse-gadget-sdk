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
 * The bench guard's console word, so a turn whose capture never rose above
 * the room's floor isn't sent; header-only for the host tests. On the USB console (>) and the
 * Nordic UART (!IN):
 *
 *   ptt.minrise=N   N 1-40: a push-to-talk turn is held on the gadget until
 *                   its capture rises N dB over its own floor; one that
 *                   never does is never sent (automation)
 *   ptt.minrise=0   off (automation)
 *   ptt.minrise?    the setting (a diagnostic)
 *
 * answered "@ptt.minrise N" or "@ptt.minrise off". RAM only: never stored,
 * off at every boot, so a user's quiet speech is never dropped; the test
 * tools set it before spoken rows. The hold itself: mg_proto_turn_hold.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MG_MINRISE_MAX_DB 40

typedef enum {
    MG_MINRISE_WORD_NONE,   /* not this word */
    MG_MINRISE_WORD_BAD,    /* this word, malformed or out of range: the usage */
    MG_MINRISE_WORD_SET,    /* ptt.minrise=N (0-40, *db) */
    MG_MINRISE_WORD_SHOW,   /* ptt.minrise? */
} mg_minrise_word_t;

static inline mg_minrise_word_t mg_minrise_word(const char *line, int *db)
{
    static const char w[] = "ptt.minrise";
    if (strncmp(line, w, sizeof(w) - 1)) {
        return MG_MINRISE_WORD_NONE;
    }
    const char *a = line + sizeof(w) - 1;
    if (!strcmp(a, "?")) {
        return MG_MINRISE_WORD_SHOW;
    }
    if (a[0] != '=') {
        return MG_MINRISE_WORD_BAD;
    }
    int v = 0;
    size_t n = 0;
    for (a++; a[n] >= '0' && a[n] <= '9' && n < 3; n++) {
        v = v * 10 + (a[n] - '0');
    }
    if (!n || a[n] || v > MG_MINRISE_MAX_DB) {
        return MG_MINRISE_WORD_BAD;
    }
    *db = v;
    return MG_MINRISE_WORD_SET;
}

/* "ptt.minrise 15" or "ptt.minrise off" (the console adds its "@"). */
static inline int mg_minrise_text(char *out, size_t cap, int db)
{
    return db ? snprintf(out, cap, "ptt.minrise %d", db) : snprintf(out, cap, "ptt.minrise off");
}

#define MG_MINRISE_USAGE "ptt.minrise=N (1-40 dB), ptt.minrise=0 (off) or ptt.minrise?"
