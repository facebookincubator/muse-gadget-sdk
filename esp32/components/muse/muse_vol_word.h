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
 * The bench volume's console word (bench and
 * test mutes are RAM-only, never written to the saved
 * volume), header-only for the host tests. On the USB console (>) and the
 * Nordic UART (!IN):
 *
 *   vol=N       N 0-100: the speaker at N in RAM only (0: off, the level
 *               kept), never written to NVS; a reboot comes back at the
 *               saved volume (automation)
 *   vol=saved   back to the saved volume (automation)
 *   vol?        the volume now and the saved one (a diagnostic)
 *
 * answered "@vol N (ram; saved M)" after vol=N, "@vol N saved M" after vol?
 * and vol=saved, N and M as speaker_volume reads them (0 while the speaker
 * is off). A user's or the app's change (a key, a slider, set_settings)
 * ends the RAM volume: the saved values come back and that change applies
 * and is saved (muse_settings.c).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    MUSE_VOL_WORD_NONE,    /* not this word */
    MUSE_VOL_WORD_BAD,     /* this word, malformed or out of range: the usage */
    MUSE_VOL_WORD_SET,     /* vol=N (0-100, *pct) */
    MUSE_VOL_WORD_SAVED,   /* vol=saved */
    MUSE_VOL_WORD_SHOW,    /* vol? */
} muse_vol_word_t;

static inline muse_vol_word_t muse_vol_word(const char *line, int *pct)
{
    if (strncmp(line, "vol", 3)) {
        return MUSE_VOL_WORD_NONE;
    }
    const char *a = line + 3;
    if (!strcmp(a, "?")) {
        return MUSE_VOL_WORD_SHOW;
    }
    if (a[0] != '=') {
        /* "volume=", "vols": another word, not a malformed vol */
        return (a[0] >= 'a' && a[0] <= 'z') || a[0] == '.' || a[0] == '_' ? MUSE_VOL_WORD_NONE
                                                                            : MUSE_VOL_WORD_BAD;
    }
    a++;
    if (!strcmp(a, "saved")) {
        return MUSE_VOL_WORD_SAVED;
    }
    int v = 0;
    size_t n = 0;
    for (; a[n] >= '0' && a[n] <= '9' && n < 3; n++) {
        v = v * 10 + (a[n] - '0');
    }
    if (!n || a[n] || v > 100) {
        return MUSE_VOL_WORD_BAD;
    }
    *pct = v;
    return MUSE_VOL_WORD_SET;
}

/* The answer without the console's "@": "vol 0 (ram; saved 40)" after a
 * set, "vol 40 saved 40" otherwise. */
static inline int muse_vol_text(char *out, size_t cap, bool after_set, int now, int saved)
{
    return after_set ? snprintf(out, cap, "vol %d (ram; saved %d)", now, saved)
                     : snprintf(out, cap, "vol %d saved %d", now, saved);
}

#define MUSE_VOL_USAGE "vol=N (0-100, RAM only), vol=saved or vol?"
