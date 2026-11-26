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
 * The subtitles' tall band on the host:
 * muse_subs_core.c's placement and layout on each
 * board's panel, with the home's own geometry (muse_home_geom, extracted
 * from muse_home.c by test_muse_subs.py into home_geom_extract.h) for the
 * top row and the clock. Reads the subtitle vectors' words
 * (protocols/test-vectors/mg-subtitle-v1.json, written by the test as
 * "V name" then "W offset hex" lines; "L" starts the long reply).
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home_geom_extract.h"
#include "muse_subs_core.h"

static int failures;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            failures++;                                                \
        }                                                              \
    } while (0)

/* LVGL 9.5.0's Montserrat line height (lv_font_montserrat_28.c): the
 * band's font on these >= 360 px panels (muse_subs.c geometry()). */
#ifndef LINE_H_28
#define LINE_H_28 30
#endif

typedef struct {
    const char *name;
    int w, h;
    bool round;
    int corner;
    int line_h;
} board_t;

static const board_t BOARDS[] = {
    { "stopwatch", 466, 466, true, 0, LINE_H_28 },     /* M5Stack StopWatch */
    { "w175", 466, 466, true, 0, LINE_H_28 },          /* Waveshare S3 1.75C */
    { "watcher", 412, 412, true, 0, LINE_H_28 },       /* SenseCAP Watcher */
};
#define NBOARDS (int)(sizeof(BOARDS) / sizeof(BOARDS[0]))

static int ring_of(const board_t *b)   /* muse_mic_ring_width */
{
    int s = b->w < b->h ? b->w : b->h;
    int r = (int)lroundf(s * 0.022f);
    return r < 2 ? 2 : r;
}

/* muse_subs.c tall_top(): just below the top row, or below the clock. */
static int top_limit(const board_t *b, bool clock, int *ink_bottom)
{
    muse_home_geom_t g;
    muse_home_geom(b->w, b->h, b->round, b->corner, &g);
    int bottom = clock ? g.time_top + g.time_digits : g.batt_top + g.batt_h;
    if (ink_bottom) {
        *ink_bottom = bottom;
    }
    return bottom + MUSE_SUBS_GAP;
}

/* Inside the visible area: the circle within the ring, or clear of the ring and the rounded corners. */
static bool inside(const board_t *b, int ring, double x, double y)
{
    if (b->round) {
        double r = (b->w < b->h ? b->w : b->h) / 2.0 - ring, dx = x - b->w / 2.0, dy = y - b->h / 2.0;
        return dx * dx + dy * dy <= r * r;
    }
    if (x < ring || y < ring || x > b->w - ring || y > b->h - ring) {
        return false;
    }
    double c = b->corner, cx = x < c ? c : x > b->w - c ? b->w - c : x, cy = y < c ? c : y > b->h - c ? b->h - c : y;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= c * c || c == 0;
}

static bool box_inside(const board_t *b, int ring, const muse_subs_band_t *r)
{
    double x0 = r->x + MUSE_SUBS_PAD_X, x1 = r->x + r->w - MUSE_SUBS_PAD_X;
    double y0 = r->y + MUSE_SUBS_PAD_Y, y1 = r->y + r->h - MUSE_SUBS_PAD_Y;
    return inside(b, ring, x0, y0) && inside(b, ring, x1, y0) && inside(b, ring, x0, y1) && inside(b, ring, x1, y1);
}

/* A proportional font, about Montserrat's shape: wide letters wider. */
static int prop(void *ctx, const char *p, int n)
{
    int unit = ctx ? *(const int *)ctx : 10;
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        int k = c == 'm' || c == 'w' || c == 'W' || c == 'M' ? 16 : c == 'i' || c == 'l' || c == '.' || c == ',' ? 5
                : c == ' '                                                                                      ? 6
                : c >= 0x80                                                                                     ? 18
                : c >= 'A' && c <= 'Z'                                                                          ? 13
                                                                                                                : 10;
        w += k * unit / 10;
    }
    return w;
}

/* ---- the vectors' words ---- */

#define MAXW 1024
typedef struct {
    char name[64];
    int n;
    uint16_t off[MAXW];
    uint8_t len[MAXW];
    char *text[MAXW];
} vec_t;

static vec_t VECS[16];
static int NVECS;
static vec_t LONG;

static int unhex(const char *h, char *out)
{
    int n = 0;
    for (; h[0] && h[1] && h[0] != '\n'; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (char)v;
    }
    out[n] = 0;
    return n;
}

static void read_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: no vectors file %s\n", path);
        failures++;
        return;
    }
    char line[2048];
    vec_t *v = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'V') {
            v = &VECS[NVECS++];
            sscanf(line + 2, "%63s", v->name);
        } else if (line[0] == 'L') {
            v = &LONG;
            strcpy(v->name, "long");
        } else if (line[0] == 'W' && v && v->n < MAXW) {
            unsigned off;
            char hex[1024];
            sscanf(line + 2, "%u %1023s", &off, hex);
            char *t = malloc(strlen(hex) / 2 + 1);
            v->len[v->n] = (uint8_t)unhex(hex, t);
            v->off[v->n] = (uint16_t)off;
            v->text[v->n] = t;
            v->n++;
        }
    }
    fclose(f);
}

static muse_subs_t S;

/* Words k0..k1 of v into the model, all shown now (one message each, as a phone streams them). */
static void feed(const vec_t *v, int k0, int k1, uint16_t reply)
{
    for (int k = k0; k < k1; k++) {
        muse_subs_in_word_t w = { MUSE_SUBS_NOW, (const uint8_t *)v->text[k], v->len[k] };
        muse_subs_words(&S, reply, k + 1 == v->n ? 1 : 0, v->off[k], &w, 1, 0);
    }
    muse_subs_update(&S, 0);
}

/* Splits a joined layout into lines (in place). */
static int split(char *joined, char *lines[], int max)
{
    int n = 0;
    if (!joined[0]) {
        return 0;
    }
    for (char *p = joined; n < max;) {
        lines[n++] = p;
        char *e = strchr(p, '\n');
        if (!e) {
            break;
        }
        *e = 0;
        p = e + 1;
    }
    return n;
}

static bool utf8_whole(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int k = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (!k) {
            return false;
        }
        for (int i = 1; i < k; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return false;
            }
        }
        p += k;
    }
    return true;
}

static void squeeze(const char *in, char *out)   /* without any whitespace */
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t') {
            continue;
        }
        if (p[0] == 0xC2 && p[1] == 0xA0) {
            p++;
            continue;
        }
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] <= 0x8A || p[2] == 0xAF)) {
            p += 2;
            continue;
        }
        if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) {
            p += 2;
            continue;
        }
        out[n++] = (char)*p;
    }
    out[n] = 0;
}

/* ---- the tests ---- */

/* A long muted reply, word by word, on each board: the band grows a line at a
 * time, the newest line at the bottom; once full, older lines scroll up one
 * line at a time and the lines that move keep their text (no reflow), across
 * the pool's drops too; over video the last two of the same lines. */
static void test_growth_and_scroll(void)
{
    if (LONG.n == 0) {
        printf("FAIL: no long reply\n");
        failures++;
        return;
    }
    for (int i = 0; i < NBOARDS; i++) {
        const board_t *b = &BOARDS[i];
        int ring = ring_of(b);
        muse_subs_tall_t t;
        muse_subs_place_tall(b->w, b->h, b->round, b->corner, ring, b->line_h, top_limit(b, true, NULL), &t);
        int max_w = t.w - 2 * MUSE_SUBS_PAD_X;
        int unit = b->line_h >= 30 ? 14 : 10;   /* glyphs about the font's size */
        muse_subs_init(&S);
        muse_subs_state(&S, true, MUSE_SUBS_MS);
        static char prev_buf[8192], cur_buf[8192];
        char *prev[MUSE_SUBS_TALL_MAX], *cur[MUSE_SUBS_TALL_MAX];
        int np = 0, peak = 0, scrolls = 0, drops = 0, full_seen = 0;
        int last_words = 0;
        prev_buf[0] = 0;
        for (int k = 0; k < LONG.n; k++) {
            int before = S.nwords;
            feed(&LONG, k, k + 1, 7);
            if (S.nwords < before + 1) {
                drops++;
            }
            int first = 0, total = 0;
            int n = muse_subs_layout_joined(&S, prop, &unit, max_w, t.lines, cur_buf, sizeof(cur_buf), &first, &total);
            muse_subs_keep_from(&S, first);
            CHECK(n == (total < t.lines ? total : t.lines));
            int nc = split(cur_buf, cur, MUSE_SUBS_TALL_MAX);
            CHECK(nc == n);
            for (int j = 0; j < nc; j++) {
                CHECK(prop(&unit, cur[j], (int)strlen(cur[j])) <= max_w);
                CHECK(utf8_whole(cur[j]));
            }
            /* The newest word ends the bottom line. */
            {
                char body[300];
                const muse_subs_word_t *nw = &S.words[S.nwords - 1];   /* the newest, as the model keeps it */
                int bl = nw->len;
                memcpy(body, S.pool + nw->pos, (size_t)bl);
                while (bl > 0 && (body[bl - 1] == ' ' || body[bl - 1] == '\n' || body[bl - 1] == '\t')) {
                    bl--;
                }
                body[bl] = 0;
                const char *last = nc ? cur[nc - 1] : "";
                size_t ll = strlen(last);
                /* (A word wider than a line ends the bottom line with its last piece.) */
                size_t m = ll < (size_t)bl ? ll : (size_t)bl;
                if (bl > 0 && strspn(body, " \t") == 0) {
                    CHECK(m > 0 && !memcmp(last + ll - m, body + bl - m, m) ? 1
                          : (printf("  newest \"%s\" not at the end of \"%s\"\n", body, last), 0));
                }
            }
            /* Grows, never shrinks. */
            CHECK(nc >= peak || nc == t.lines);
            if (nc > peak) {
                CHECK(nc <= peak + 1 || LONG.len[k] > 40);   /* a line at a time */
                peak = nc;
            }
            /* The lines that were finished stay as they were: the same text, moved up
             * by however many lines scrolled off the top (one at a time). */
            if (np > 1) {
                int shift = -1;
                for (int s = 0; s < np && shift < 0; s++) {
                    bool same = true;
                    for (int j = 0; j + s < np - 1 && same; j++) {
                        same = j < nc && !strcmp(cur[j], prev[j + s]);
                    }
                    if (same) {
                        shift = s;
                    }
                }
                CHECK(shift >= 0 ? 1 : (printf("  %s: reflowed at word %d\n", b->name, k), 0));
                CHECK(shift <= 1 || LONG.len[k] > 40);
                if (shift == 1) {
                    scrolls++;
                    CHECK(np == t.lines);   /* scrolls only when full */
                }
            }
            if (nc == t.lines) {
                full_seen++;
            }
            /* Over video: the last two of these same lines (the wrap is one width). */
            char two[MUSE_SUBS_VIDEO_LINES][MUSE_SUBS_LINE_MAX];
            int f2 = 0;
            int n2 = muse_subs_layout_from(&S, prop, &unit, max_w, two, MUSE_SUBS_VIDEO_LINES, &f2);
            CHECK(n2 <= MUSE_SUBS_VIDEO_LINES && n2 == (nc < 2 ? nc : 2));
            for (int j = 0; j < n2; j++) {
                CHECK(!strcmp(two[j], cur[nc - n2 + j]));
            }
            memcpy(prev_buf, cur_buf, sizeof(cur_buf));
            for (int j = 0; j < nc; j++) {
                prev[j] = prev_buf + (cur[j] - cur_buf);
            }
            np = nc;
            last_words = S.nwords;
        }
        (void)last_words;
        printf("SCROLL %s lines=%d peak=%d scrolls=%d drops=%d words=%d\n", b->name, t.lines, peak, scrolls, drops,
               LONG.n);
        CHECK(peak == t.lines);
        CHECK(scrolls > 10);
        CHECK(drops > 0);   /* the pool filled: the scrolled-away lines went without a reflow */
        CHECK(full_seen > 0);
    }
}

/* The vectors' replies on each board's band: every word in full, in order,
 * code points whole, no hyphen added, no line wider than the band. */
static void test_vectors_wrap(void)
{
    CHECK(NVECS >= 8);
    for (int i = 0; i < NBOARDS; i++) {
        const board_t *b = &BOARDS[i];
        int ring = ring_of(b);
        muse_subs_tall_t t;
        muse_subs_place_tall(b->w, b->h, b->round, b->corner, ring, b->line_h, top_limit(b, false, NULL), &t);
        int max_w = t.w - 2 * MUSE_SUBS_PAD_X;
        int unit = b->line_h >= 30 ? 14 : 10;
        for (int v = 0; v < NVECS; v++) {
            muse_subs_init(&S);
            muse_subs_state(&S, true, MUSE_SUBS_MS);
            feed(&VECS[v], 0, VECS[v].n, 9);
            static char out[8192];
            int total = 0;
            int n = muse_subs_layout_joined(&S, prop, &unit, max_w, MUSE_SUBS_TALL_MAX, out, sizeof(out), NULL, &total);
            CHECK(n == total);   /* a vector's reply fits the tall band */
            char want[4096] = "", got[8192];
            muse_subs_revealed_text(&S, want, sizeof(want));   /* the words as the model keeps them (cleaned) */
            int hy_want = 0, hy_got = 0;
            for (char *p = want; *p; p++) hy_want += *p == '-';
            for (char *p = out; *p; p++) hy_got += *p == '-';
            CHECK(hy_got == hy_want);
            char *lines[MUSE_SUBS_TALL_MAX];
            int nl = split(out, lines, MUSE_SUBS_TALL_MAX);
            static char joined[8192];
            joined[0] = 0;
            for (int j = 0; j < nl; j++) {
                CHECK(prop(&unit, lines[j], (int)strlen(lines[j])) <= max_w);
                CHECK(utf8_whole(lines[j]));
                strcat(joined, lines[j]);
            }
            char sw[4096];
            squeeze(want, sw);
            squeeze(joined, got);
            if (strcmp(sw, got)) {
                printf("  %s on %s: got \"%s\" want \"%s\"\n", VECS[v].name, b->name, got, sw);
            }
            CHECK(!strcmp(sw, got));
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        read_vectors(argv[1]);
    }
    test_vectors_wrap();
    test_growth_and_scroll();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("muse_subs tall band: all passed\n");
    return 0;
}
