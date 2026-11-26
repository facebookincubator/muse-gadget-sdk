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

/* Live call subtitles' model (muse_subs_core.h). */

#include "muse_subs_core.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

uint16_t muse_subs_blend565(uint16_t px, unsigned bg, unsigned fg)
{
    unsigned r = (px >> 11) << 3, g = ((px >> 5) & 63) << 2, b = (px & 31) << 3;
    unsigned k = 255 - (bg > 255 ? 255 : bg);
    r = r * k / 255;
    g = g * k / 255;
    b = b * k / 255;
    if (fg) {
        r += (255 - r) * fg / 255;
        g += (255 - g) * fg / 255;
        b += (255 - b) * fg / 255;
    }
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

void muse_subs_init(muse_subs_t *s)
{
    memset(s, 0, sizeof(*s));
    s->hidden = true;
}

static void reset_reply(muse_subs_t *s)
{
    s->nwords = s->revealed = 0;
    s->pool_len = 0;
    s->text_end = 0;
    s->ended = false;
    s->end_at = 0;
    s->end_seen_ms = 0;
    s->have_reply = false;
    s->keep_from = 0;
    s->drops = 0;
    s->have_clock = false;   /* a reply's clock is its own: the next one waits for its first clock */
    if (!s->hidden) {
        s->hidden = true;
        s->gen++;
    }
}

void muse_subs_state(muse_subs_t *s, bool active, muse_subs_timing_t timing)
{
    s->active = active;
    s->timing = timing == MUSE_SUBS_MS ? MUSE_SUBS_MS : MUSE_SUBS_FRAME;
    if (!active) {
        reset_reply(s);
        s->have_dropped = false;   /* the mode's over: a later call's replies count their ids again */
    }
}

int muse_subs_utf8_clean(char *dst, int cap, const uint8_t *src, int len)
{
    int o = 0;
    for (int i = 0; i < len;) {
        uint8_t c = src[i];
        int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n > 0 && i + n <= len && !(n == 2 && c < 0xC2);
        for (int k = 1; ok && k < n; k++) {
            ok = (src[i + k] & 0xC0) == 0x80;
        }
        if (ok && c < 0x20 && c != ' ') {
            ok = false;   /* control characters: not drawn */
            n = 1;
        }
        if (!ok) {
            if (o + 1 >= cap) {
                break;
            }
            dst[o++] = c == '\n' || c == '\t' || c == '\r' ? ' ' : '?';   /* (a CR LF: two spaces, one gap) */
            i += 1;
            continue;
        }
        if (o + n >= cap) {
            break;
        }
        memcpy(dst + o, src + i, n);
        o += n;
        i += n;
    }
    if (cap > 0) {
        dst[o] = 0;
    }
    return o;
}

/* Drops the oldest words (they've scrolled away) to make room. */
static void drop_oldest(muse_subs_t *s, int n)
{
    if (n <= 0) {
        return;
    }
    if (n > s->nwords) {
        n = s->nwords;
    }
    int cut = n < s->nwords ? s->words[n].pos : s->pool_len;
    memmove(s->pool, s->pool + cut, (size_t)(s->pool_len - cut));
    s->pool_len -= cut;
    memmove(s->words, s->words + n, sizeof(s->words[0]) * (size_t)(s->nwords - n));
    s->nwords -= n;
    for (int i = 0; i < s->nwords; i++) {
        s->words[i].pos = (uint16_t)(s->words[i].pos - cut);
    }
    s->revealed = s->revealed > n ? s->revealed - n : 0;
    s->keep_from = s->keep_from > n ? s->keep_from - n : 0;
    s->drops++;
}

void muse_subs_words(muse_subs_t *s, uint16_t reply_id, uint8_t flags, uint16_t char_offset,
                     const muse_subs_in_word_t *w, int count, int64_t now_ms)
{
    if (!s->active) {
        s->ignored++;   /* only state on turns subtitles on */
        return;
    }
    if (s->have_dropped && reply_id == s->dropped_id) {
        return;   /* the reply the user talked over */
    }
    if (!s->have_reply || reply_id != s->reply_id) {
        reset_reply(s);   /* a new reply supersedes the old one */
        s->have_dropped = false;   /* ...and the one the user talked over is behind it: its id is free again */
        s->have_reply = true;
        s->reply_id = reply_id;
    }
    uint32_t off = char_offset;
    if (flags & 0x02) {
        /* replace: the tail from char_offset is rewritten. */
        int keep = 0;
        while (keep < s->nwords && s->words[keep].off < off) {
            keep++;
        }
        if (keep < s->nwords) {
            s->pool_len = s->words[keep].pos;
            s->nwords = keep;
            if (s->keep_from > keep) {
                s->keep_from = 0;   /* its line was rewritten: no known line start */
            }
            if (s->revealed > keep) {
                s->revealed = keep;
                s->gen++;
            }
        }
        s->text_end = off;
        s->ended = false;
        s->end_seen_ms = 0;
    }
    for (int i = 0; i < count; i++) {
        uint32_t woff = off;
        off += w[i].len;
        if (woff < s->text_end) {
            continue;   /* already have it (a resend) */
        }
        char clean[256];
        int n = muse_subs_utf8_clean(clean, sizeof(clean), w[i].utf8, w[i].len);
        if (n == 0) {
            s->text_end = off;
            continue;
        }
        if ((s->nwords == MUSE_SUBS_WORDS_MAX || s->pool_len + n > MUSE_SUBS_POOL) && s->keep_from > 0) {
            /* The lines that have scrolled away go first, whole: the wrap
             * restarts at a line's first word, so nothing on screen reflows. */
            drop_oldest(s, s->keep_from);
        }
        if (s->nwords == MUSE_SUBS_WORDS_MAX || s->pool_len + n > MUSE_SUBS_POOL) {
            drop_oldest(s, s->nwords / 2 > 0 ? s->nwords / 2 : 1);   /* the band's own text fills the pool */
        }
        if (s->pool_len + n > MUSE_SUBS_POOL) {
            continue;
        }
        muse_subs_word_t *d = &s->words[s->nwords++];
        d->show_at = w[i].show_at;
        d->pos = (uint16_t)s->pool_len;
        d->len = (uint8_t)n;
        d->off = woff;
        d->came_ms = now_ms;
        memcpy(s->pool + s->pool_len, clean, (size_t)n);
        s->pool_len += n;
        s->text_end = off;
    }
    s->last_ms = now_ms;
}

void muse_subs_reply_end(muse_subs_t *s, uint16_t reply_id, uint32_t show_at, int64_t now_ms)
{
    if (!s->active) {
        s->ignored++;
        return;
    }
    if (!s->have_reply || reply_id != s->reply_id) {
        return;
    }
    s->ended = true;
    s->end_at = show_at;
    s->last_ms = now_ms;
}

void muse_subs_clear(muse_subs_t *s, uint16_t reply_id)
{
    if (reply_id == 0 || (s->have_reply && reply_id == s->reply_id)) {
        reset_reply(s);
    }
}

void muse_subs_clock(muse_subs_t *s, uint16_t reply_id, uint32_t playout_ms, int64_t now_ms)
{
    if (!s->active) {
        s->ignored++;
        return;
    }
    if (!s->have_reply || reply_id != s->reply_id) {
        return;   /* a reply this device doesn't hold */
    }
    s->have_clock = true;
    s->clock_ms = playout_ms;
    s->clock_at_ms = now_ms;
}

void muse_subs_frame(muse_subs_t *s, uint32_t frame)
{
    s->have_frame = true;
    s->frame = frame;
}

void muse_subs_no_frame(muse_subs_t *s)
{
    s->have_frame = false;
}

/* Whether a time in the reply's domain has come. */
static bool due(const muse_subs_t *s, uint32_t at, int64_t now_ms)
{
    if (at == MUSE_SUBS_NOW) {
        return true;
    }
    if (s->timing == MUSE_SUBS_FRAME) {
        return s->have_frame && (int32_t)(s->frame - at) >= 0;   /* serial comparison */
    }
    if (!s->have_clock) {
        return false;
    }
    int64_t playout = (int64_t)s->clock_ms + (now_ms - s->clock_at_ms);
    return playout >= (int64_t)at;
}

void muse_subs_lock(muse_subs_t *s, bool locked)
{
    s->locked = locked;
}

bool muse_subs_update(muse_subs_t *s, int64_t now_ms)
{
    uint32_t gen = s->gen;
    if (s->paused && now_ms - s->paused_ms >= MUSE_SUBS_PAUSE_MAX_MS) {
        s->paused = false;   /* a press nobody decided: the reply plays on, and so does its band */
        s->gen++;
    }
    if (s->locked && s->have_reply && s->ended && due(s, s->end_at, now_ms)) {
        reset_reply(s);   /* it ended behind the lock: dropped */
        return s->gen != gen;
    }
    /* The late fallback is for a word whose moment can't be judged: no frame on the panel in frame timing, no clock
     * yet for this reply in ms timing. While the domain has a time, a word waits for its moment, however early
     * it came. */
    bool timeless = s->timing == MUSE_SUBS_FRAME ? !s->have_frame : !s->have_clock;
    while (s->revealed < s->nwords) {
        const muse_subs_word_t *w = &s->words[s->revealed];
        if (!due(s, w->show_at, now_ms) && (!timeless || now_ms - w->came_ms < MUSE_SUBS_LATE_MS)) {
            break;
        }
        s->revealed++;
        s->last_ms = now_ms;
        s->gen++;
    }
    if (s->revealed > 0 && s->hidden) {
        s->hidden = false;
        s->gen++;
    }
    if (!s->hidden) {
        if (s->ended && !s->end_seen_ms && s->revealed == s->nwords &&
            (due(s, s->end_at, now_ms) || now_ms - s->last_ms >= MUSE_SUBS_LATE_MS)) {
            s->end_seen_ms = now_ms;
        }
        bool done = s->end_seen_ms && now_ms - s->end_seen_ms >= MUSE_SUBS_LINGER_MS;
        /* An app that never sends reply_end (one missed after a barge-in):
         * every word shown and nothing new for a while is its end. A word
         * still waiting for its moment keeps the band; a real reply_end's
         * linger wins. */
        bool idle = s->revealed == s->nwords &&
                    now_ms - s->last_ms >= (s->ended ? MUSE_SUBS_IDLE_MS : MUSE_SUBS_NO_END_MS);
        if (done || idle) {
            reset_reply(s);
        }
    }
    return s->gen != gen;
}

bool muse_subs_band_shows(int front, bool video, bool locked)
{
    return front >= 0 && !video && !locked;
}

bool muse_subs_visible(const muse_subs_t *s)
{
    return !s->hidden && s->revealed > 0 && !s->paused;
}

void muse_subs_pause(muse_subs_t *s, bool on, int64_t now_ms)
{
    if (on != s->paused) {
        s->paused = on;
        s->gen++;
    }
    if (on) {
        s->paused_ms = now_ms;
    }
}

static int ws_at_end(const uint8_t *u, int n);
static int ws_at_start(const uint8_t *u, int n);

/* Leading whitespace's length (the kinds trim_len knows). */
static int lead_len(const char *p, int n)
{
    int k = 0;
    for (int w; k < n && (w = ws_at_start((const uint8_t *)p + k, n - k)) > 0;) {
        k += w;
    }
    return k;
}

/* Length without trailing whitespace: spaces, tabs and line breaks, and the
 * Unicode spaces a reply's text can carry between words (no-break U+00A0,
 * thin U+2009, narrow no-break U+202F, ideographic U+3000 and the rest of
 * U+2000..U+200A). They all end a word here; a line break may follow it. */
static int trim_len(const char *p, int n)
{
    for (int w; n > 0 && (w = ws_at_end((const uint8_t *)p, n)) > 0;) {
        n -= w;
    }
    return n;
}

/* The whitespace that ends u[0..n) / starts it: its length in bytes (0: none). */
static int ws_len(const uint8_t *u, int n)
{
    if (n >= 1 && (u[0] == ' ' || u[0] == '\t' || u[0] == '\n' || u[0] == '\r')) {
        return 1;
    }
    if (n >= 2 && u[0] == 0xC2 && u[1] == 0xA0) {
        return 2;
    }
    if (n >= 3 && u[0] == 0xE2 && u[1] == 0x80 && (u[2] <= 0x8A || u[2] == 0xAF)) {
        return 3;
    }
    if (n >= 3 && u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80) {
        return 3;
    }
    return 0;
}

static int ws_at_start(const uint8_t *u, int n)
{
    return ws_len(u, n);
}

static int ws_at_end(const uint8_t *u, int n)
{
    for (int k = 1; k <= 3 && k <= n; k++) {
        if (ws_len(u + n - k, k) == k) {
            return k;
        }
    }
    return 0;
}

/* The longest prefix of p (whole code points, at least one) that fits max_w
 * with a hyphen after it (or bare, when "hyphen" is false), and fits cap
 * bytes with its hyphen and terminator. Returns its length in bytes. */
static int break_prefix(const char *p, int n, muse_subs_measure_t measure, void *ctx, int max_w, bool hyphen, int cap)
{
    int keep = 0;
    char tmp[MUSE_SUBS_LINE_MAX + 2];
    for (int i = 0; i < n;) {
        int k = 1;
        while (i + k < n && ((uint8_t)p[i + k] & 0xC0) == 0x80) {
            k++;
        }
        if (i + k + 2 > cap) {
            break;
        }
        memcpy(tmp, p, (size_t)(i + k));
        int len = i + k;
        if (hyphen) {
            tmp[len++] = '-';
        }
        if (keep > 0 && measure(ctx, tmp, len) > max_w) {
            break;
        }
        i += k;
        keep = i;
    }
    return keep;
}

/* A finished line: idx counts lines from the first word kept (0 up), word is
 * the word whose text starts it, line[0..len) its text (trailing spaces
 * trimmed, not terminated). */
typedef void (*line_fn)(void *u, int idx, const char *line, int len, int word);

/* The greedy wrap from the first word kept: appending never re-wraps an
 * earlier line, and a line's first word starts the same line again when the
 * words before it are gone. Hands each line to emit as it's finished;
 * returns how many there are. One line's buffer, whatever the count. */
static int wrap(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w, line_fn emit, void *u)
{
    char line[MUSE_SUBS_LINE_MAX];
    int total = 0;          /* lines started; the open one is total - 1 */
    int cur_len = 0;
    int line_word = 0;      /* the word that started the open line */
    bool open = false;
    bool space = false;     /* the next word on this line goes after a space */
    line[0] = 0;
    for (int i = 0; i < s->revealed; i++) {
        const char *p = s->pool + s->words[i].pos;
        int n = s->words[i].len;
        /* Whitespace before it (a word that starts with a separator, or one
         * that is nothing but one: phones stream "today", " ", "count.")
         * separates it from the word before, as whitespace after that one
         * does. */
        int lead = lead_len(p, n);
        if (lead && cur_len) {
            space = true;
        }
        p += lead;
        n -= lead;
        int body = trim_len(p, n);
        if (body == 0) {
            continue;
        }
        int sep = space && cur_len ? 1 : 0;
        bool fits = false;
        if (open && cur_len + sep + body < MUSE_SUBS_LINE_MAX) {
            if (sep) {
                line[cur_len] = ' ';
            }
            memcpy(line + cur_len + sep, p, (size_t)body);
            fits = measure(ctx, line, cur_len + sep + body) <= max_w;
            line[cur_len] = 0;
        }
        if (!fits) {
            if (open) {
                emit(u, total - 1, line, trim_len(line, cur_len), line_word);
            }
            open = true;
            total++;
            cur_len = 0;
            sep = 0;
            line_word = i;
            line[0] = 0;
            /* A word wider than a whole line: never cut or ellipsized, it
             * breaks across lines at code-point boundaries, with no hyphen.
             * The last piece goes on as a word. */
            while (body >= MUSE_SUBS_LINE_MAX - 1 || measure(ctx, p, body) > max_w) {
                int k = break_prefix(p, body, measure, ctx, max_w, false, MUSE_SUBS_LINE_MAX);
                emit(u, total - 1, p, k, line_word);
                p += k;
                n -= k;
                body -= k;
                total++;
            }
        }
        if (sep) {
            line[cur_len++] = ' ';
        }
        memcpy(line + cur_len, p, (size_t)body);
        cur_len += body;
        line[cur_len] = 0;
        /* A space before the next word if this one came with whitespace after
         * it, or the reply's text has a gap before the next word (a phone
         * that drops the separators). */
        space = n > body || (i + 1 < s->nwords && s->words[i + 1].off > s->words[i].off + s->words[i].len);
    }
    if (open) {
        emit(u, total - 1, line, trim_len(line, cur_len), line_word);
    }
    return total;
}

/* The ring of the last max lines, in the caller's lines, and their first words. */
typedef struct {
    char (*lines)[MUSE_SUBS_LINE_MAX];
    int max;
    int words[MUSE_SUBS_TALL_MAX];
} ring_t;

static void ring_line(void *u, int idx, const char *line, int len, int word)
{
    ring_t *r = u;
    char *d = r->lines[idx % r->max];
    memcpy(d, line, (size_t)len);
    d[len] = 0;
    if (r->max <= MUSE_SUBS_TALL_MAX) {
        r->words[idx % r->max] = word;
    }
}

static void swap_lines(char *a, char *b)
{
    for (int i = 0; i < MUSE_SUBS_LINE_MAX; i++) {
        char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

static void reverse_lines(char (*l)[MUSE_SUBS_LINE_MAX], int a, int b)   /* [a, b) */
{
    for (b--; a < b; a++, b--) {
        swap_lines(l[a], l[b]);
    }
}

int muse_subs_layout_from(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w,
                          char lines[][MUSE_SUBS_LINE_MAX], int max_lines, int *first_word)
{
    if (first_word) {
        *first_word = 0;
    }
    if (max_lines <= 0 || !muse_subs_visible(s)) {
        return 0;
    }
    ring_t r = { .lines = lines, .max = max_lines };
    int total = wrap(s, measure, ctx, max_w, ring_line, &r);
    int shown = total < max_lines ? total : max_lines;
    int first = total - shown;
    if (first_word && shown > 0 && max_lines <= MUSE_SUBS_TALL_MAX) {
        *first_word = r.words[first % max_lines];
    }
    /* Rotate the ring in place so lines come out oldest first (no copy of
     * the lines: the video task's stack is small). */
    int k = first % max_lines;
    if (total > max_lines && k) {
        reverse_lines(lines, 0, k);
        reverse_lines(lines, k, max_lines);
        reverse_lines(lines, 0, max_lines);
    }
    return shown;
}

int muse_subs_layout(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w,
                     char lines[][MUSE_SUBS_LINE_MAX], int max_lines)
{
    return muse_subs_layout_from(s, measure, ctx, max_w, lines, max_lines, NULL);
}

static void count_line(void *u, int idx, const char *line, int len, int word)
{
    (void)u;
    (void)idx;
    (void)line;
    (void)len;
    (void)word;
}

typedef struct {
    int skip;          /* lines before this one aren't kept */
    int end;           /* nor this one or after */
    char *out;
    int cap, len, written;
    bool full;
    int first_word;
} join_t;

static void join_line(void *u, int idx, const char *line, int len, int word)
{
    join_t *j = u;
    if (idx < j->skip || idx >= j->end || j->full) {
        return;
    }
    int need = (j->written ? 1 : 0) + len + 1;
    if (j->len + need > j->cap) {
        j->full = true;   /* (a cap muse_subs.c sizes from the pool never gets here) */
        return;
    }
    if (j->written == 0) {
        j->first_word = word;
    } else {
        j->out[j->len++] = '\n';
    }
    memcpy(j->out + j->len, line, (size_t)len);
    j->len += len;
    j->out[j->len] = 0;
    j->written++;
}

int muse_subs_layout_window(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w, int max_lines,
                            int end, char *out, int cap, int *first_word, int *total)
{
    if (cap > 0) {
        out[0] = 0;
    }
    if (first_word) {
        *first_word = 0;
    }
    if (total) {
        *total = 0;
    }
    if (max_lines <= 0 || cap <= 0 || !muse_subs_visible(s)) {
        return 0;
    }
    int all = wrap(s, measure, ctx, max_w, count_line, NULL);
    if (end < 0 || end > all) {
        end = all;
    }
    join_t j = { .skip = end > max_lines ? end - max_lines : 0, .end = end, .out = out, .cap = cap };
    wrap(s, measure, ctx, max_w, join_line, &j);
    if (first_word) {
        *first_word = j.first_word;
    }
    if (total) {
        *total = all;
    }
    return j.written;
}

int muse_subs_layout_joined(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w, int max_lines,
                            char *out, int cap, int *first_word, int *total)
{
    return muse_subs_layout_window(s, measure, ctx, max_w, max_lines, -1, out, cap, first_word, total);
}

int muse_subs_line_count(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w)
{
    return muse_subs_visible(s) ? wrap(s, measure, ctx, max_w, count_line, NULL) : 0;
}

int muse_subs_scroll_end(muse_subs_scroll_t *sc, const muse_subs_t *s, int total, int64_t now_ms)
{
    if (sc->end == 0 && sc->at_ms == 0) {
        sc->drops = s->drops;   /* a new band: its lines come in one by one from the first */
    }
    if (sc->drops != s->drops || total < sc->end) {
        sc->drops = s->drops;
        sc->end = total;   /* the lines count from elsewhere now: all at once */
        sc->at_ms = now_ms;
    } else if (total > sc->end && (sc->at_ms == 0 || now_ms - sc->at_ms >= MUSE_SUBS_SCROLL_MS)) {
        sc->end++;   /* one more line: the band grows, or scrolls up one line */
        sc->at_ms = now_ms;
    }
    return sc->end;
}

void muse_subs_keep_from(muse_subs_t *s, int word)
{
    s->keep_from = word < 0 ? 0 : word > s->nwords ? s->nwords : word;
}

int muse_subs_revealed_text(const muse_subs_t *s, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < s->revealed && cap > 0; i++) {
        int n = s->words[i].len;
        if (o + n >= cap) {
            break;
        }
        memcpy(out + o, s->pool + s->words[i].pos, (size_t)n);
        o += n;
    }
    if (cap > 0) {
        out[o] = 0;
    }
    return o;
}

uint32_t muse_subs_extend_frame(uint32_t *ext, bool *reset, uint16_t number)
{
    if (*reset) {
        *reset = false;
        *ext = number;
    } else {
        *ext += (uint32_t)(int32_t)(int16_t)(uint16_t)(number - (uint16_t)*ext);
    }
    return *ext;
}

void muse_subs_barge_in(muse_subs_t *s)
{
    s->paused = false;   /* the press's pause ends with the turn */
    if (s->have_reply) {
        s->have_dropped = true;
        s->dropped_id = s->reply_id;
    }
    reset_reply(s);
}

void muse_subs_place(int w, int h, bool round, int corner, int ring, int line_h, int lines, muse_subs_band_t *out)
{
    int s = w < h ? w : h;
    int bh = lines * line_h + 2 * MUSE_SUBS_PAD_Y;
    int bottom = h - ring - MUSE_SUBS_GAP;
    int by = bottom - bh;
    int half;
    if (round) {
        /* Low on a circle the band would be a sliver: its lowest text row
         * sits where the circle (inside the ring) is still 72% wide. The
         * text box is that wide; its top row, nearer the middle, is wider. */
        double r = s / 2.0 - ring - MUSE_SUBS_GAP;
        int hx = (int)(0.72 * r);
        double low = h / 2.0 + sqrt(r * r - (double)hx * hx);   /* the lowest text row */
        if ((int)low + MUSE_SUBS_PAD_Y < bottom) {
            by = (int)low + MUSE_SUBS_PAD_Y - bh;
        }
        /* Re-measured at the text box's actual lowest row (rounding). */
        double dy = (by + bh - MUSE_SUBS_PAD_Y) - h / 2.0;
        double cx = r * r - dy * dy > 0 ? sqrt(r * r - dy * dy) : 0;
        half = (int)cx + MUSE_SUBS_PAD_X;
    } else {
        /* Clear of the rounded corners at the band's lowest row. */
        int c = corner > 0 ? corner : 0;
        int inset = 0;
        int from_bottom = h - (by + bh);
        if (c > from_bottom) {
            double dy = c - from_bottom;
            inset = (int)ceil(c - sqrt((double)c * c - dy * dy));
        }
        half = w / 2 - ring - MUSE_SUBS_GAP - inset;
    }
    out->w = 2 * half & ~3;
    out->x = (w - out->w) / 2 & ~1;
    out->y = by;
    out->h = bh;
}

/* Half the band's width the visible area allows at row y: a round panel's
 * chord within the ring (as muse_subs_place measures it, plus the band's
 * side padding: the text box is the chord), a rectangle's width less the
 * ring and its rounded corners' inset at that row. */
static double band_half_at(int w, int h, bool round, int corner, int ring, double y)
{
    if (round) {
        int s = w < h ? w : h;
        double r = s / 2.0 - ring - MUSE_SUBS_GAP;
        double dy = y - h / 2.0;
        return r * r - dy * dy > 0 ? (int)sqrt(r * r - dy * dy) + MUSE_SUBS_PAD_X : -1;
    }
    if (y < ring + MUSE_SUBS_GAP || y > h - ring - MUSE_SUBS_GAP) {
        return -1;
    }
    int c = corner > 0 ? corner : 0;
    double from_edge = y < h / 2.0 ? y : h - y;
    int inset = 0;
    if (c > from_edge) {
        double dy = c - from_edge;
        inset = (int)ceil(c - sqrt((double)c * c - dy * dy));
    }
    return w / 2 - ring - MUSE_SUBS_GAP - inset;
}

void muse_subs_place_tall(int w, int h, bool round, int corner, int ring, int line_h, int top_y,
                          muse_subs_tall_t *out)
{
    muse_subs_band_t two;
    muse_subs_place(w, h, round, corner, ring, line_h, MUSE_SUBS_VIDEO_LINES, &two);
    out->x = two.x;
    out->w = two.w;
    out->bottom = two.y + two.h;
    out->line_h = line_h > 0 ? line_h : 1;
    int text_bottom = out->bottom - MUSE_SUBS_PAD_Y;
    /* The highest top line: its band no higher than top_y, and the band at
     * least as wide there as at the bottom (round: the text box's top row
     * on the circle; rectangle: the band's top row clear of the corners). */
    int top = top_y + MUSE_SUBS_PAD_Y;
    int min_top = ring + MUSE_SUBS_GAP + MUSE_SUBS_PAD_Y;
    if (top < min_top) {
        top = min_top;
    }
    while (top < text_bottom &&
           band_half_at(w, h, round, corner, ring, round ? top : top - MUSE_SUBS_PAD_Y) < two.w / 2) {
        top++;
    }
    int lines = (text_bottom - top) / out->line_h;
    if (lines < MUSE_SUBS_VIDEO_LINES) {
        lines = MUSE_SUBS_VIDEO_LINES;
    }
    if (lines > MUSE_SUBS_TALL_MAX) {
        lines = MUSE_SUBS_TALL_MAX;
    }
    out->lines = lines;
    out->text_top = text_bottom - lines * out->line_h;
}

void muse_subs_tall_rect(const muse_subs_tall_t *t, int n, muse_subs_band_t *out)
{
    n = n < 1 ? 1 : n > t->lines ? t->lines : n;
    out->x = t->x;
    out->w = t->w;
    out->h = n * t->line_h + 2 * MUSE_SUBS_PAD_Y;
    out->y = t->bottom - out->h;
}
