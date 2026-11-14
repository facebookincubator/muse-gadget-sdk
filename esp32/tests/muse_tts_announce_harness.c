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

// muse_tts_announce_check() (components/muse/muse_tts_text.c): the gate a
// device.speak command's text passes before the gadget agrees to say it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_tts.h"

static int failures;

static void expect(const char *name, const char *text, muse_tts_announce_t want)
{
    static char scratch[MUSE_TTS_ANNOUNCE_MAX + 8];
    muse_tts_announce_t got = muse_tts_announce_check(text, scratch, sizeof(scratch));
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", name, (int)got, (int)want);
        failures++;
    }
}

static char *repeat(const char *unit, size_t times)
{
    size_t len = strlen(unit);
    char *text = malloc(len * times + 1);
    if (!text) {
        printf("FAIL: out of memory building a case\n");
        exit(1);
    }
    for (size_t i = 0; i < times; i++) {
        memcpy(text + i * len, unit, len);
    }
    text[len * times] = '\0';
    return text;
}

int main(void)
{
    expect("plain Vietnamese", "Xin chào buổi sáng", MUSE_TTS_ANNOUNCE_OK);
    expect("plain English", "Good morning. Here is your briefing.", MUSE_TTS_ANNOUNCE_OK);
    expect("markdown is cleaned, not refused",
           "**Chào** bạn, hôm nay [trời đẹp](https://example.com) nhé", MUSE_TTS_ANNOUNCE_OK);
    expect("empty", "", MUSE_TTS_ANNOUNCE_EMPTY);
    expect("null text", NULL, MUSE_TTS_ANNOUNCE_EMPTY);
    expect("only emoji", "\xF0\x9F\x8E\x89\xF0\x9F\x8E\x89", MUSE_TTS_ANNOUNCE_EMPTY);
    expect("only markdown marks", "** ~~ ##", MUSE_TTS_ANNOUNCE_EMPTY);

    /* The cap counts bytes of cleaned text: exactly at it passes, one over fails. */
    char *at_cap = repeat("a", MUSE_TTS_ANNOUNCE_MAX);
    expect("exactly at the cap", at_cap, MUSE_TTS_ANNOUNCE_OK);
    free(at_cap);
    char *over_cap = repeat("a", MUSE_TTS_ANNOUNCE_MAX + 1);
    expect("one byte over the cap", over_cap, MUSE_TTS_ANNOUNCE_TOO_LONG);
    free(over_cap);

    /* Vietnamese letters are two bytes each once cleaned: 400 fit, 600 don't. */
    char *vi_fits = repeat("\xC3\xA1", 400);   /* "á" */
    expect("Vietnamese under the byte cap", vi_fits, MUSE_TTS_ANNOUNCE_OK);
    free(vi_fits);
    char *vi_over = repeat("\xC3\xA1", 600);
    expect("Vietnamese over the byte cap", vi_over, MUSE_TTS_ANNOUNCE_TOO_LONG);
    free(vi_over);

    /* A caller that hands over no scratch gets a refusal, not a crash. */
    if (muse_tts_announce_check("Chào bạn", NULL, 0) != MUSE_TTS_ANNOUNCE_EMPTY) {
        printf("FAIL no scratch: want EMPTY\n");
        failures++;
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
