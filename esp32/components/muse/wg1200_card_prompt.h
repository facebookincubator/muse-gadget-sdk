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
 * WeatherXM WG1200 swipe-down card prompt: a template kept in muse_nvs that
 * USB (">card.prompt=…") or the Companion BLE service ("card.prompt=…") can
 * change without reflashing. Its placeholders are filled at swipe time:
 *   {weather}   the station's live readings
 *   {location}  the station's GPS fix, or the build's fallback place
 *   {style}     a randomly picked card style
 * The inline helpers below are free of ESP-IDF so host tests can compile them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Longest template accepted; a BLE write carries ~400 characters, so longer
 * ones arrive as card.prompt= followed by card.prompt+= chunks. */
#define WG1200_CARD_PROMPT_MAX 1023

#define WG1200_CARD_PROMPT_DEFAULT                                                         \
    "Weather: {weather} Station location: {location}. Draw my swipe-down forecast card. "  \
    "My saved card preferences (forecast providers or models, time range, location, "      \
    "style, layout or anything else I've told you) override these defaults; use a "        \
    "default only where I haven't said otherwise. Defaults: treat these readings as "      \
    "ground truth, fetch today's hourly forecast for this location from multiple "         \
    "models/providers, score each against the current conditions, and draw a 480x480 "     \
    "HOURLY forecast card for today from the best-fitting models, with exact values and "  \
    "the model sources noted on the card, in the \"{style}\" style. Always send the card " \
    "via display.draw_file to this station."

/* Fills {weather}, {location} and {style}; any other text, braces included,
 * is copied as it is. Always NUL-terminates; returns the length written. */
static inline size_t wg1200_card_prompt_expand(const char *tpl, const char *weather,
                                               const char *location, const char *style,
                                               char *out, size_t cap)
{
    static const char *const names[] = {"{weather}", "{location}", "{style}"};
    const char *values[] = {weather ? weather : "", location ? location : "", style ? style : ""};
    size_t n = 0;
    if (!out || cap == 0) {
        return 0;
    }
    while (tpl && *tpl && n + 1 < cap) {
        const char *value = NULL;
        size_t name_len = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            size_t len = strlen(names[i]);
            if (strncmp(tpl, names[i], len) == 0) {
                value = values[i];
                name_len = len;
                break;
            }
        }
        if (value) {
            size_t len = strlen(value);
            if (len > cap - 1 - n) {
                len = cap - 1 - n;
            }
            memcpy(out + n, value, len);
            n += len;
            tpl += name_len;
        } else {
            out[n++] = *tpl++;
        }
    }
    out[n] = '\0';
    return n;
}

/* Applies one command to the stored template `cur` (cap bytes): "card.prompt"
 * replaces it (empty restores the default), "card.prompt+" appends a chunk.
 * Returns false, leaving `cur` as it was, when the result would be too long or
 * an append arrives with nothing to append to. */
static inline bool wg1200_card_prompt_apply(char *cur, size_t cap, bool append, const char *chunk)
{
    size_t have = append ? strlen(cur) : 0;
    size_t add = strlen(chunk ? chunk : "");
    if ((append && have == 0) || have + add > WG1200_CARD_PROMPT_MAX || have + add + 1 > cap) {
        return false;
    }
    memcpy(cur + have, chunk ? chunk : "", add);
    cur[have + add] = '\0';
    return true;
}

/* Storage and commands, in wg1200_card_prompt.c. */

/* The template a swipe uses: the stored one, else WG1200_CARD_PROMPT_DEFAULT. */
void wg1200_card_prompt_get(char *out, size_t cap);

/* Handles card.prompt=…, card.prompt+=… and card.prompt? from the Companion
 * BLE service or the USB console. True when `cmd` was one of them; *result is
 * then the short answer the caller reports ("ok: 612 chars" or an error). */
bool wg1200_card_prompt_command(const char *cmd, const char *value, const char **result);
