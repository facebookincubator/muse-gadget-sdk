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

/* Live call subtitles' model (muse_subs_core.c) on the host. */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "muse_subs_core.h"
#include <math.h>

static int failures;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            failures++;                                                \
        }                                                              \
    } while (0)

/* 10 px per code point. */
static int mono(void *ctx, const char *p, int n)
{
    (void)ctx;
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (((unsigned char)p[i] & 0xC0) != 0x80) {
            w += 10;
        }
    }
    return w;
}

static muse_subs_t S;

static void words(uint16_t id, uint8_t flags, uint16_t off, const char *const *texts, const uint32_t *at, int n, int64_t now)
{
    muse_subs_in_word_t w[32];
    for (int i = 0; i < n; i++) {
        w[i].show_at = at[i];
        w[i].utf8 = (const uint8_t *)texts[i];
        w[i].len = (uint8_t)strlen(texts[i]);
    }
    muse_subs_words(&S, id, flags, off, w, n, now);
}

static int layout(int max_w, char out[][MUSE_SUBS_LINE_MAX], int max_lines)
{
    return muse_subs_layout(&S, mono, NULL, max_w, out, max_lines);
}

static void test_frame_reveal(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "Hello ", "there, ", "friend." };
    uint32_t at[] = { 10, 20, 30 };
    words(1, 0, 0, t, at, 3, 1000);
    CHECK(!muse_subs_update(&S, 1000) && !muse_subs_visible(&S));   /* no frame yet */
    muse_subs_frame(&S, 9);
    CHECK(!muse_subs_update(&S, 1010));
    muse_subs_frame(&S, 10);
    CHECK(muse_subs_update(&S, 1020) && muse_subs_visible(&S));
    char l[2][MUSE_SUBS_LINE_MAX];
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "Hello"));
    muse_subs_frame(&S, 25);
    muse_subs_update(&S, 1030);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "Hello there,"));
    muse_subs_frame(&S, 31);
    muse_subs_update(&S, 1040);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "Hello there, friend."));
    /* The end: linger, then gone. */
    muse_subs_reply_end(&S, 1, 40, 1050);
    muse_subs_frame(&S, 41);
    muse_subs_update(&S, 1100);
    CHECK(muse_subs_visible(&S));
    muse_subs_update(&S, 1100 + MUSE_SUBS_LINGER_MS - 1);
    CHECK(muse_subs_visible(&S));
    CHECK(muse_subs_update(&S, 1100 + MUSE_SUBS_LINGER_MS) && !muse_subs_visible(&S));
}

static void test_now_and_late(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "Now ", "later" };
    uint32_t at[] = { MUSE_SUBS_NOW, 5000 };
    words(2, 0, 0, t, at, 2, 0);
    muse_subs_update(&S, 1);
    char l[2][MUSE_SUBS_LINE_MAX];
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "Now"));
    /* Frame 5000 never comes: shown MUSE_SUBS_LATE_MS after it came. */
    muse_subs_update(&S, MUSE_SUBS_LATE_MS - 1);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "Now"));
    muse_subs_update(&S, MUSE_SUBS_LATE_MS);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "Now later"));
}

static void test_ms_clock(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_MS);
    const char *t[] = { "one ", "two ", "three" };
    uint32_t at[] = { 0, 500, 1000 };
    words(3, 0, 0, t, at, 3, 100);
    muse_subs_update(&S, 100);
    CHECK(!muse_subs_visible(&S));   /* no clock yet */
    muse_subs_clock(&S, 3, 0, 200);
    muse_subs_update(&S, 200);
    char l[2][MUSE_SUBS_LINE_MAX];
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "one"));
    muse_subs_update(&S, 699);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "one"));
    muse_subs_update(&S, 700);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "one two"));
    muse_subs_clock(&S, 3, 1000, 750);   /* the phone's clock ran ahead: catch up */
    muse_subs_update(&S, 750);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "one two three"));
}

/* The band without video shows a text in front, never with video (it draws its own) and never while locked, even
 * with a band left in front (a lock that came while the model's mutex was busy). */
static void test_band_hides_locked(void)
{
    CHECK(muse_subs_band_shows(0, false, false));
    CHECK(!muse_subs_band_shows(0, false, true));
    CHECK(!muse_subs_band_shows(0, true, false) && !muse_subs_band_shows(-1, false, false));
    if (!failures) {
        printf("band: never while locked, whatever is in front\n");
    }
}

static void test_wrap_scroll_ellipsis(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "The ", "quick ", "brown ", "fox ", "jumps ", "over ", "the ", "lazy ", "dog." };
    uint32_t at[9];
    for (int i = 0; i < 9; i++) {
        at[i] = MUSE_SUBS_NOW;
    }
    words(4, 0, 0, t, at, 4, 0);
    muse_subs_update(&S, 0);
    char l[2][MUSE_SUBS_LINE_MAX];
    /* 100 px = 10 characters a line. */
    CHECK(layout(100, l, 2) == 2 && !strcmp(l[0], "The quick") && !strcmp(l[1], "brown fox"));
    words(4, 0, 20, t + 4, at + 4, 5, 0);
    muse_subs_update(&S, 0);
    int n = layout(100, l, 2);
    /* Lines: "The quick" / "brown fox" / "jumps over" / "the lazy" / "dog.": the last two. */
    CHECK(n == 2 && !strcmp(l[0], "the lazy") && !strcmp(l[1], "dog."));
    /* Appends don't re-wrap: the same lines with one more word only add at the end. */
    char l3[3][MUSE_SUBS_LINE_MAX];
    CHECK(layout(100, l3, 3) == 3 && !strcmp(l3[0], "jumps over"));
    /* A word longer than a line: never cut, never "...": it breaks at
     * characters with a hyphen, and the next word follows its last piece. */
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *u[] = { "Supercalifragilistic ", "ok" };
    uint32_t now2[] = { MUSE_SUBS_NOW, MUSE_SUBS_NOW };
    words(5, 0, 0, u, now2, 2, 0);
    muse_subs_update(&S, 0);
    /* No hyphen at a forced break: whole code points only. */
    CHECK(layout(100, l3, 3) == 3 && !strcmp(l3[0], "Supercalif") && !strcmp(l3[1], "ragilistic") &&
          !strcmp(l3[2], "ok"));
}

/* A proportional font: wide letters wider. */
static int prop(void *ctx, const char *p, int n)
{
    (void)ctx;
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        w += c == 'm' || c == 'w' || c == 'W' || c == 'M' ? 16 : c == 'i' || c == 'l' || c == '.' || c == '/' ? 5
             : c >= 0x80                                                                              ? 18
                                                                                                      : 10;
    }
    return w;
}

/* Every character of a text shows, in order, no line wider than the box,
 * no "..." (none in the text): hyphens only at the ends of broken pieces. */
static void check_whole(const char *const *t, int n, int max_w, const char *joined)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    uint32_t at[16];
    for (int i = 0; i < n; i++) {
        at[i] = MUSE_SUBS_NOW;
    }
    words(30, 0, 0, t, at, n, 0);
    muse_subs_update(&S, 0);
    char l[4][MUSE_SUBS_LINE_MAX];
    int k = muse_subs_layout(&S, prop, NULL, max_w, l, 4);
    char out[512] = "";
    for (int i = 0; i < k; i++) {
        CHECK(prop(NULL, l[i], (int)strlen(l[i])) <= max_w);
        CHECK(!strstr(l[i], "...") && !strstr(l[i], "\xe2\x80\xa6"));
        strcat(out, l[i]);   /* a forced break has no hyphen: compare without spaces */
    }
    char a[512], b[512];
    size_t na = 0, nb = 0;
    for (const char *c = out; *c; c++) if (*c != ' ') a[na++] = *c;
    for (const char *c = joined; *c; c++) if (*c != ' ') b[nb++] = *c;
    a[na] = b[nb] = 0;
    if (strcmp(a, b)) {
        printf("  got \"%s\"\n want \"%s\"\n", a, b);
    }
    CHECK(!strcmp(a, b));
}

/* A phone that sends words without their separators: the offsets' gaps
 * say where the spaces were (one each). Words that touch (no gap) join. */
static void test_words_without_spaces(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "What's", "the", "weather", "in", "Livermore?" };
    muse_subs_in_word_t w[5];
    uint32_t off[] = { 0, 7, 11, 19, 22 };
    for (int i = 0; i < 5; i++) {
        w[i].show_at = MUSE_SUBS_NOW;
        w[i].utf8 = (const uint8_t *)t[i];
        w[i].len = (uint8_t)strlen(t[i]);
    }
    /* One word a message, as a phone sends while the text streams in. */
    for (int i = 0; i < 5; i++) {
        muse_subs_words(&S, 40, 0, (uint16_t)off[i], &w[i], 1, 0);
    }
    muse_subs_update(&S, 0);
    char l[4][MUSE_SUBS_LINE_MAX];
    CHECK(layout(160, l, 4) == 3 && !strcmp(l[0], "What's the") && !strcmp(l[1], "weather in") && !strcmp(l[2], "Livermore?"));
    CHECK(layout(400, l, 4) == 1 && !strcmp(l[0], "What's the weather in Livermore?"));
}

/* Words that end in another kind of space (no-break, thin, a tab, a line
 * break): still separate words, wrapped at them, drawn as one space. */
static void test_unicode_separators(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "What's\xc2\xa0", "the\t", "weather\xe2\x80\xaf", "in\n", "Livermore?" };
    uint32_t at[5];
    for (int i = 0; i < 5; i++) {
        at[i] = MUSE_SUBS_NOW;
    }
    words(41, 0, 0, t, at, 5, 0);
    muse_subs_update(&S, 0);
    char l[4][MUSE_SUBS_LINE_MAX];
    CHECK(layout(400, l, 4) == 1 && !strcmp(l[0], "What's the weather in Livermore?"));
    CHECK(layout(160, l, 4) == 3 && !strcmp(l[0], "What's the") && !strcmp(l[1], "weather in") &&
          !strcmp(l[2], "Livermore?"));
}

/* What a phone streams mid-reply: a word, then its separator alone, then
 * the next word; and a word with the space in front. */
static void test_split_separators(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "Let's ", "make ", "today", " ", "count. ", "In", " Livermore?" };
    uint32_t at[7];
    for (int i = 0; i < 7; i++) {
        at[i] = MUSE_SUBS_NOW;
    }
    for (int i = 0, off = 0; i < 7; off += (int)strlen(t[i]), i++) {
        words(43, 0, (uint16_t)off, t + i, at + i, 1, 0);
    }
    muse_subs_update(&S, 0);
    char l[4][MUSE_SUBS_LINE_MAX];
    CHECK(layout(400, l, 4) == 1 && !strcmp(l[0], "Let's make today count. In Livermore?"));
    CHECK(layout(200, l, 4) == 2 && !strcmp(l[0], "Let's make today") && !strcmp(l[1], "count. In Livermore?"));
}

static void test_long_words(void)
{
    const char *a[] = { "An ", "electroencephalography ", "reads ", "it." };
    check_whole(a, 4, 120, "An electroencephalography reads it.");
    const char *b[] = { "See ", "https://example.com/very/long/path/index.html" };
    check_whole(b, 2, 150, "See https://example.com/very/long/path/index.html");
    const char *c[] = { "Wwwwwwwwwwwwwwwwwwww" };   /* every piece one wide letter at a time if need be */
    check_whole(c, 1, 120, "Wwwwwwwwwwwwwwwwwwww");
    const char *d[] = { "Ma\xc3\xb1" "ana ", "caf\xc3\xa9" };   /* breaks fall between code points */
    check_whole(d, 2, 60, "Ma\xc3\xb1" "ana caf\xc3\xa9");
}

static void test_utf8(void)
{
    char out[16];
    const uint8_t bad[] = { 'a', 0xC3, 'b', 0xE2, 0x80, 0x99, 0xFF, 'c' };
    int n = muse_subs_utf8_clean(out, sizeof(out), bad, sizeof(bad));
    CHECK(n == 8 && !memcmp(out, "a?b\xE2\x80\x99?c", 8));
    /* Never a split code point at the cap. */
    const uint8_t e[] = { 'x', 0xC3, 0xA9, 0xC3, 0xA9 };
    n = muse_subs_utf8_clean(out, 5, e, sizeof(e));
    CHECK(n == 3 && !memcmp(out, "x\xC3\xA9", 3));
    /* Wrapping counts code points, and a break keeps them whole. */
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "caf\xC3\xA9 ", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9" };
    uint32_t at[] = { MUSE_SUBS_NOW, MUSE_SUBS_NOW };
    words(6, 0, 0, t, at, 2, 0);
    muse_subs_update(&S, 0);
    char l[3][MUSE_SUBS_LINE_MAX];
    CHECK(layout(100, l, 3) == 3 && !strcmp(l[0], "caf\xC3\xA9"));
    CHECK(!strcmp(l[1], "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"));
    CHECK(!strcmp(l[2], "\xC3\xA9\xC3\xA9"));
}

static void test_lifecycle(void)
{
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    const char *t[] = { "first ", "reply" };
    uint32_t at[] = { MUSE_SUBS_NOW, MUSE_SUBS_NOW };
    words(7, 0, 0, t, at, 2, 0);
    muse_subs_update(&S, 0);
    CHECK(muse_subs_visible(&S));
    muse_subs_clear(&S, 99);   /* another reply's clear: nothing */
    CHECK(muse_subs_visible(&S));
    /* A resend of the same words isn't doubled. */
    words(7, 0, 0, t, at, 2, 0);
    muse_subs_update(&S, 0);
    char l[2][MUSE_SUBS_LINE_MAX];
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "first reply"));
    /* replace: the tail from byte 6 rewritten. */
    const char *r[] = { "answer!" };
    words(7, 0x02, 6, r, at, 1, 0);
    muse_subs_update(&S, 0);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "first answer!"));
    /* A new reply supersedes the old one. */
    const char *n2[] = { "next" };
    words(8, 0, 0, n2, at, 1, 0);
    muse_subs_update(&S, 0);
    CHECK(layout(300, l, 2) == 1 && !strcmp(l[0], "next"));
    muse_subs_clear(&S, 0);
    CHECK(!muse_subs_visible(&S));
    /* Mode off clears too. */
    words(9, 0, 0, n2, at, 1, 0);
    muse_subs_update(&S, 0);
    CHECK(muse_subs_visible(&S));
    muse_subs_state(&S, false, MUSE_SUBS_FRAME);
    CHECK(!muse_subs_visible(&S));
    /* No reply_end ever comes: gone 3 s after the last word showed. */
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    words(10, 0, 0, n2, at, 1, 0);
    muse_subs_update(&S, 0);
    muse_subs_update(&S, MUSE_SUBS_NO_END_MS - 1);
    CHECK(muse_subs_visible(&S));
    muse_subs_update(&S, MUSE_SUBS_NO_END_MS);
    CHECK(!muse_subs_visible(&S));
    /* ... but a word still waiting for its frame keeps it up: with frames on
     * the panel it waits for its own, never shown early by the late fallback
     * (then 3 s more). */
    muse_subs_frame(&S, 100);
    const char *two[] = { "now ", "later" };
    uint32_t at2[] = { MUSE_SUBS_NOW, 500 };
    words(11, 0, 0, two, at2, 2, 10000);
    muse_subs_update(&S, 10000);
    muse_subs_update(&S, 10000 + 4 * MUSE_SUBS_LATE_MS);
    CHECK(muse_subs_visible(&S) && S.revealed == 1);
    muse_subs_frame(&S, 500);
    int64_t t500 = 10000 + 4 * MUSE_SUBS_LATE_MS + 1;
    muse_subs_update(&S, t500);
    CHECK(S.revealed == 2);
    muse_subs_update(&S, t500 + MUSE_SUBS_NO_END_MS - 1);
    CHECK(muse_subs_visible(&S));
    muse_subs_update(&S, t500 + MUSE_SUBS_NO_END_MS);
    CHECK(!muse_subs_visible(&S));
    /* With a reply_end, its 1.5 s linger decides (not the 3 s). */
    words(12, 0, 0, n2, at, 1, 20000);
    muse_subs_reply_end(&S, 12, MUSE_SUBS_NOW, 20000);
    muse_subs_update(&S, 20000);
    muse_subs_update(&S, 20000 + MUSE_SUBS_LINGER_MS - 1);
    CHECK(muse_subs_visible(&S));
    muse_subs_update(&S, 20000 + MUSE_SUBS_LINGER_MS);
    CHECK(!muse_subs_visible(&S));
    /* The gadget's own talk press: cleared at once, that reply's later words
     * ignored, the next reply shown. */
    words(13, 0, 0, t, at, 1, 30000);
    muse_subs_update(&S, 30000);
    CHECK(muse_subs_visible(&S));
    muse_subs_barge_in(&S);
    CHECK(!muse_subs_visible(&S));
    words(13, 0, 6, t + 1, at, 1, 30010);
    muse_subs_update(&S, 30010);
    CHECK(!muse_subs_visible(&S) && S.nwords == 0);
    words(14, 0, 0, n2, at, 1, 30020);
    muse_subs_update(&S, 30020);
    CHECK(muse_subs_visible(&S) && layout(300, l, 2) == 1 && !strcmp(l[0], "next"));
    /* The drop lasts only while its reply does: the next reply's first words, or the mode going off (a call's end),
     * free its id, so a later reply with that id shows (each call's replies count from the start again). */
    CHECK(!S.have_dropped);
    words(15, 0, 0, t, at, 1, 30030);
    muse_subs_barge_in(&S);
    words(16, 0, 0, n2, at, 1, 30040);
    words(15, 0, 0, n2, at, 1, 30050);
    muse_subs_update(&S, 30050);
    CHECK(muse_subs_visible(&S) && S.reply_id == 15 && layout(300, l, 2) == 1 && !strcmp(l[0], "next"));
    muse_subs_barge_in(&S);
    muse_subs_state(&S, false, MUSE_SUBS_FRAME);
    CHECK(!S.have_dropped);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    words(15, 0, 0, n2, at, 1, 30060);
    muse_subs_update(&S, 30060);
    CHECK(muse_subs_visible(&S) && S.reply_id == 15 && layout(300, l, 2) == 1 && !strcmp(l[0], "next"));
    printf("barge-in: a dropped reply's id is free again after the next reply or the mode's end\n");
}

/* A reply's clock is its own, an early word waits for its moment, and a talk press's pause hides the band without
 * dropping the reply. */
static void test_clock_and_pause(void)
{
    char l[2][MUSE_SUBS_LINE_MAX];
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_MS);
    /* Reply 1 plays to 9 s. */
    const char *a[] = { "first " };
    uint32_t at0[] = { 0 };
    words(1, 0, 0, a, at0, 1, 0);
    muse_subs_clock(&S, 1, 9000, 0);
    muse_subs_update(&S, 0);
    CHECK(muse_subs_visible(&S));
    /* Reply 2's words at 5 s: never on reply 1's clock (9 s, already past 5 s), and with no clock of its own for
     * the late fallback's 2 s. */
    const char *b[] = { "second " };
    uint32_t at5[] = { 5000 };
    words(2, 0, 0, b, at5, 1, 100);
    muse_subs_update(&S, 100);
    CHECK(!muse_subs_visible(&S) && S.revealed == 0);
    /* Its clock at 0: the word waits for its 5 s, however long ago it came (no early late fallback). */
    muse_subs_clock(&S, 2, 0, 200);
    muse_subs_update(&S, 200 + MUSE_SUBS_LATE_MS + 1000);
    CHECK(S.revealed == 0);
    muse_subs_update(&S, 200 + 4999);
    CHECK(S.revealed == 0);
    muse_subs_update(&S, 200 + 5000);
    CHECK(S.revealed == 1 && layout(300, l, 2) == 1 && !strcmp(l[0], "second"));
    /* A talk press's touch-down hides the band; the reply's words keep coming due; a tap brings it back. */
    const char *c[] = { "third " };
    uint32_t at6[] = { 6000 };
    words(2, 0, 7, c, at6, 1, 5300);
    muse_subs_pause(&S, true, 5300);
    CHECK(!muse_subs_visible(&S));
    muse_subs_update(&S, 200 + 6000);
    CHECK(!muse_subs_visible(&S) && S.revealed == 2);
    muse_subs_pause(&S, false, 6300);
    muse_subs_update(&S, 6300);
    CHECK(muse_subs_visible(&S) && layout(300, l, 2) == 1 && !strcmp(l[0], "second third"));
    /* A pause nothing ends lapses after MUSE_SUBS_PAUSE_MAX_MS, as the reply audio's does. */
    muse_subs_pause(&S, true, 6400);
    muse_subs_update(&S, 6400 + MUSE_SUBS_PAUSE_MAX_MS - 1);
    CHECK(!muse_subs_visible(&S));
    muse_subs_update(&S, 6400 + MUSE_SUBS_PAUSE_MAX_MS);
    CHECK(muse_subs_visible(&S));
    /* A press that claims the turn drops the reply (barge-in). */
    muse_subs_pause(&S, true, 8000);
    muse_subs_barge_in(&S);
    muse_subs_update(&S, 8001);
    CHECK(!muse_subs_visible(&S) && S.nwords == 0 && !S.paused);
    printf("timing: a reply's own clock, no early words; a talk tap keeps the reply's subtitles\n");
}

static void test_long_reply(void)
{
    /* Far more words than kept: the oldest go, nothing overflows, the end is right. */
    muse_subs_init(&S);
    muse_subs_state(&S, true, MUSE_SUBS_FRAME);
    char buf[16];
    uint32_t off = 0;
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "w%d ", i);
        const char *t[] = { buf };
        uint32_t at[] = { (uint32_t)i };
        words(11, 0, (uint16_t)off, t, at, 1, i);
        off += (uint32_t)strlen(buf);
        muse_subs_frame(&S, (uint32_t)i);
        muse_subs_update(&S, i);
    }
    char l[2][MUSE_SUBS_LINE_MAX];
    int n = layout(100, l, 2);
    CHECK(n == 2 && strstr(l[1], "w999") != NULL);
    (void)off;
}

int main(void)
{
    test_frame_reveal();
    test_now_and_late();
    test_ms_clock();
    test_band_hides_locked();
    test_wrap_scroll_ellipsis();
    test_long_words();
    test_words_without_spaces();
    test_unicode_separators();
    test_split_separators();
    test_utf8();
    test_lifecycle();
    test_clock_and_pause();
    test_long_reply();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("muse_subs: all passed\n");
    return 0;
}
