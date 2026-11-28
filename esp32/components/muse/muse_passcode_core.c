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

#include "muse_passcode_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- the record and the lock -------------------------------------------- */

static const int WAITS_S[MUSE_PASS_WAIT_LEVELS] = { 30, 60, 120, 240, 300 };

int muse_pass_wait_s(int level)
{
    if (level < 0) level = 0;
    if (level >= MUSE_PASS_WAIT_LEVELS) level = MUSE_PASS_WAIT_LEVELS - 1;
    return WAITS_S[level];
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

static void rec_default(muse_pass_rec_t *r)
{
    wipe(r, sizeof(*r));
    r->format = MUSE_PASS_FORMAT;
    r->lock = MUSE_PASS_LOCK_SCREEN_OFF;
}

void muse_pass_init(muse_pass_t *p, const muse_pass_rec_t *rec, int64_t now_ms)
{
    memset(p, 0, sizeof(*p));
    if (rec && (rec->format == MUSE_PASS_FORMAT || rec->format == 1)) {
        p->rec = *rec;
        if (rec->format == 1) {
            p->rec.format = MUSE_PASS_FORMAT;   /* the earlier layout, one byte shorter */
            p->dirty = true;
        }
        p->rec.reserved = 0;   /* unused: written as 0 from the next store on */
        if (p->rec.lock > MUSE_PASS_LOCK_AFTER_5_MIN) p->rec.lock = MUSE_PASS_LOCK_SCREEN_OFF;
        if (p->rec.wait_level >= MUSE_PASS_WAIT_LEVELS) p->rec.wait_level = MUSE_PASS_WAIT_LEVELS - 1;
        if (!p->rec.on) {
            p->rec.tries = p->rec.wait_level = 0;
        }
    } else {
        rec_default(&p->rec);
    }
    p->locked = p->rec.on;   /* a reboot starts locked */
    if (p->rec.on && p->rec.tries >= MUSE_PASS_FREE_TRIES) {
        /* After a reboot the current level's wait starts again. */
        p->wait_until_ms = now_ms + (int64_t)muse_pass_wait_s(p->rec.wait_level) * 1000;
    }
}

void muse_pass_init_unreadable(muse_pass_t *p)
{
    memset(p, 0, sizeof(*p));
    rec_default(&p->rec);
    p->rec.on = 1;   /* no salt or hash: muse_pass_check never takes a code (unreadable) */
    p->locked = true;
    p->unreadable = true;
}

bool muse_pass_screen_off(muse_pass_t *p, int64_t now_ms, bool busy)
{
    p->dark = true;
    p->after_due = false;
    if (!p->rec.on || p->locked || busy) {
        return false;   /* busy: the first screen-off after the call or turn locks */
    }
    if (p->rec.lock == MUSE_PASS_LOCK_SCREEN_OFF) {
        p->locked = true;
        return true;
    }
    p->after_due = true;
    p->dark_since_ms = now_ms;
    return false;
}

static bool after_lapsed(muse_pass_t *p, int64_t now_ms)
{
    if (p->after_due && p->rec.on && now_ms - p->dark_since_ms >= MUSE_PASS_AFTER_MS) {
        p->after_due = false;
        if (!p->locked) {
            p->locked = true;
            return true;
        }
    }
    return false;
}

bool muse_pass_screen_on(muse_pass_t *p, int64_t now_ms)
{
    bool locked = after_lapsed(p, now_ms);
    p->dark = false;
    p->after_due = false;   /* woken within the 5 minutes: no code, and the next screen-off starts again */
    return locked;
}

bool muse_pass_tick(muse_pass_t *p, int64_t now_ms, uint32_t idle_ms, bool stays_on, bool busy)
{
    if (!p->rec.on || p->locked) {
        return false;
    }
    if (p->dark) {
        return after_lapsed(p, now_ms);
    }
    if (stays_on && !busy && idle_ms >= MUSE_PASS_IDLE_MS) {
        p->locked = true;
        return true;
    }
    return false;
}

bool muse_pass_press(muse_pass_t *p, int64_t now_ms)
{
    return p->dark && after_lapsed(p, now_ms);
}

void muse_pass_lock_now(muse_pass_t *p)
{
    if (p->rec.on) {
        p->locked = true;
    }
}

static bool hash_code(const uint8_t salt[MUSE_PASS_SALT_LEN], const char digits[MUSE_PASS_DIGITS],
                      muse_pass_sha256_t sha, uint8_t out[MUSE_PASS_HASH_LEN])
{
    uint8_t buf[MUSE_PASS_SALT_LEN + sizeof(MUSE_PASS_TAG) - 1 + MUSE_PASS_DIGITS];
    memcpy(buf, salt, MUSE_PASS_SALT_LEN);
    memcpy(buf + MUSE_PASS_SALT_LEN, MUSE_PASS_TAG, sizeof(MUSE_PASS_TAG) - 1);
    memcpy(buf + MUSE_PASS_SALT_LEN + sizeof(MUSE_PASS_TAG) - 1, digits, MUSE_PASS_DIGITS);
    bool ok = sha(buf, sizeof(buf), out);
    wipe(buf, sizeof(buf));
    return ok;
}

static bool digits_ok(const char d[MUSE_PASS_DIGITS])
{
    for (int i = 0; i < MUSE_PASS_DIGITS; i++) {
        if (d[i] < '0' || d[i] > '9') return false;
    }
    return true;
}

int64_t muse_pass_wait_left_ms(const muse_pass_t *p, int64_t now_ms)
{
    return p->wait_until_ms && now_ms < p->wait_until_ms ? p->wait_until_ms - now_ms : 0;
}

muse_pass_result_t muse_pass_check(muse_pass_t *p, const char digits[MUSE_PASS_DIGITS], int64_t now_ms,
                                   muse_pass_sha256_t sha)
{
    if (muse_pass_wait_left_ms(p, now_ms) > 0) {
        return MUSE_PASS_WAITING;
    }
    p->wait_until_ms = 0;
    bool right = false;
    if (p->rec.on && !p->unreadable && digits_ok(digits)) {
        uint8_t h[MUSE_PASS_HASH_LEN];
        if (!hash_code(p->rec.salt, digits, sha, h)) {
            wipe(h, sizeof(h));
            return MUSE_PASS_WRONG;   /* the hash failed: not right, and not counted */
        }
        uint8_t diff = 0;
        for (int i = 0; i < MUSE_PASS_HASH_LEN; i++) diff |= h[i] ^ p->rec.hash[i];   /* constant time */
        wipe(h, sizeof(h));
        right = diff == 0;
    }
    if (right) {
        if (p->rec.tries || p->rec.wait_level) {
            p->rec.tries = p->rec.wait_level = 0;
            p->dirty = true;
        }
        p->locked = false;
        return MUSE_PASS_RIGHT;
    }
    if (p->rec.tries < 255) p->rec.tries++;
    p->dirty = !p->unreadable;   /* an unreadable record is never written over: the count is RAM's */
    if (p->rec.tries < MUSE_PASS_FREE_TRIES) {
        return MUSE_PASS_WRONG;
    }
    /* The 5th wrong code waits 30 s; each further one doubles it to 5 min. */
    int level = p->rec.tries - MUSE_PASS_FREE_TRIES;
    p->rec.wait_level = (uint8_t)(level < MUSE_PASS_WAIT_LEVELS ? level : MUSE_PASS_WAIT_LEVELS - 1);
    p->wait_until_ms = now_ms + (int64_t)muse_pass_wait_s(p->rec.wait_level) * 1000;
    return MUSE_PASS_WAIT;
}

bool muse_pass_set(muse_pass_t *p, const char digits[MUSE_PASS_DIGITS], const uint8_t salt[MUSE_PASS_SALT_LEN],
                   muse_pass_sha256_t sha)
{
    uint8_t h[MUSE_PASS_HASH_LEN];
    if (!digits_ok(digits) || !hash_code(salt, digits, sha, h)) {
        wipe(h, sizeof(h));
        return false;
    }
    p->rec.format = MUSE_PASS_FORMAT;
    p->rec.on = 1;
    p->rec.tries = p->rec.wait_level = 0;
    memcpy(p->rec.salt, salt, MUSE_PASS_SALT_LEN);
    memcpy(p->rec.hash, h, MUSE_PASS_HASH_LEN);
    wipe(h, sizeof(h));
    p->wait_until_ms = 0;
    p->locked = false;
    p->after_due = false;
    p->unreadable = false;   /* a new code replaces whatever was stored */
    p->dirty = true;
    return true;
}

void muse_pass_turn_off(muse_pass_t *p)
{
    uint8_t lock = p->rec.lock;
    rec_default(&p->rec);
    p->rec.lock = lock;   /* the Lock choice stays for the next time it's turned on */
    p->locked = p->after_due = false;
    p->wait_until_ms = 0;
    p->unreadable = false;   /* `pass clear`: the stored record is erased */
    p->dirty = true;
}

void muse_pass_set_lock(muse_pass_t *p, muse_pass_lock_t lock)
{
    if (lock > MUSE_PASS_LOCK_AFTER_5_MIN || p->rec.lock == (uint8_t)lock) return;
    p->rec.lock = (uint8_t)lock;
    p->dirty = !p->unreadable;
}

void muse_pass_forget(muse_pass_t *p)
{
    rec_default(&p->rec);
    p->locked = p->after_due = false;
    p->wait_until_ms = 0;
    p->unreadable = false;   /* the pairing's erase: the stored record, readable or not, goes */
    p->dirty = true;
}

void muse_pass_status(const muse_pass_t *p, int64_t now_ms, char *out, size_t n)
{
    if (!p->rec.on) {
        snprintf(out, n, "off");
        return;
    }
    int64_t left = muse_pass_wait_left_ms(p, now_ms);
    snprintf(out, n, "on %s tries=%u wait=%d", p->locked ? "locked" : "unlocked", (unsigned)p->rec.tries,
             (int)((left + 999) / 1000));
}

/* ---- geometry: the keypad's ring and grid ----------------------------------- */

/* Python's round(): half to even, as nearbyint in the default rounding mode. */
static int pround(double v)
{
    return (int)nearbyint(v);
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

void muse_pass_geom_base(muse_pass_geom_t *g, int w, int h, bool round, double ppm)
{
    memset(g, 0, sizeof(*g));
    g->layout = round ? MUSE_PASS_RING : MUSE_PASS_GRID;
    g->w = w;
    g->h = h;
    g->s = w < h ? w : h;
    g->ppm = ppm;
    g->cx = w / 2.0;
    g->cy = h / 2.0;
    int S = g->s;
    g->ring = imax(2, pround(0.022 * S));
    g->edge = imax(2, pround(0.01 * S));
    g->m = imax(4, pround(0.02 * S));
    g->prompt_caps = imax(imax(9, pround(0.036 * S)), (int)ceil(1.4 * ppm));
    g->small_caps = imax(imax(9, pround(0.03 * S)), (int)ceil(1.15 * ppm));
    g->dot_d = imax(imax(6, pround(0.032 * S)), (int)ceil(1.2 * ppm));
    g->dot_pitch = pround(2.4 * g->dot_d);
    g->dot_stroke = imax(1, pround(0.006 * S));
    g->gap = imax(4, pround(0.6 * g->prompt_caps));
    g->slop_px = imax(8, pround(1.5 * ppm));
}

void muse_pass_geom_ring(muse_pass_geom_t *g)
{
    double R = g->s / 2.0 - g->ring;
    double s18 = sin(18.0 * M_PI / 180.0);
    g->rc = (R - g->edge) / (1 + 0.84 * s18);   /* the keys' outer edge on R - edge */
    g->pitch = 2 * g->rc * s18;
    g->key_d = 0.84 * g->pitch;
    for (int d = 0; d < 10; d++) {   /* 0 at 12 o'clock, then 1-9 clockwise, 36 degrees apart */
        double a = 36.0 * d * M_PI / 180.0;
        g->key_x[d] = g->cx + g->rc * sin(a);
        g->key_y[d] = g->cy - g->rc * cos(a);
    }
    g->center_hit_r = g->rc - g->pitch / 2.0;
    g->center_vis_r = g->rc - g->key_d / 2.0 - g->edge;
    g->inner_r = 0.35 * g->pitch;
    double cell_w = g->pitch, cell_h = g->s / 2.0 - g->center_hit_r;
    g->cell_w = cell_w;
    g->cell_h = cell_h;
    g->key_mm = (cell_w < cell_h ? cell_w : cell_h) / g->ppm;
    g->digit_caps = imin(pround(0.5 * g->key_d), imax(pround(0.36 * g->key_d), (int)ceil(2.0 * g->ppm)));
    g->dots_cy = g->cy;
    muse_pass_geom_ring_prompt(g, g->prompt_caps);
}

void muse_pass_geom_ring_prompt(muse_pass_geom_t *g, int prompt_caps_used)
{
    g->del_box = imax(14, pround(1.6 * prompt_caps_used));
    g->del_cy = g->cy + g->dot_d / 2.0 + g->gap + g->del_box / 2.0;
}

int muse_pass_prompt_pitch(int caps)
{
    return pround(caps * 1.65);
}

void muse_pass_geom_grid(muse_pass_geom_t *g, int prompt_lines, int prompt_caps_used)
{
    int inset = g->ring + g->m;
    int top_line = inset + g->gap + (prompt_lines > 1 ? (prompt_lines - 1) * muse_pass_prompt_pitch(prompt_caps_used) : 0);
    g->dots_cy = top_line + prompt_caps_used + g->gap + g->dot_d / 2.0;
    double top = g->dots_cy + g->dot_d / 2.0 + g->gap;
    double bot = g->h - inset;
    double px = (g->w - 2.0 * inset) / 3.0, py = (bot - top) / 4.0;
    double mn = px < py ? px : py;
    g->key_gap = imax(3, pround(0.07 * mn));
    g->kp_left = inset;
    g->kp_top = top;
    g->kp_bot = bot;
    g->cell_w = px;
    g->cell_h = py;
    g->key_w = px - 2 * g->key_gap;
    g->key_h = py - 2 * g->key_gap;
    g->key_r = pround(0.28 * (g->key_w < g->key_h ? g->key_w : g->key_h));
    g->inner_rx = 0.35 * px;
    g->inner_ry = 0.35 * py;
    g->pitch = mn;
    g->key_mm = mn / g->ppm;
    /* 1 2 3 / 4 5 6 / 7 8 9 / blank 0 del */
    static const int ROW[11] = { 3, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3 };
    static const int COL[11] = { 1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 2 };
    for (int k = 0; k < 11; k++) {
        g->key_x[k] = inset + (COL[k] + 0.5) * px;
        g->key_y[k] = top + (ROW[k] + 0.5) * py;
    }
    double kmin = mn - 2 * g->key_gap;
    g->digit_caps = imin(pround(0.5 * (py - 2 * g->key_gap)), imax(pround(0.36 * kmin), (int)ceil(2.0 * g->ppm)));
    g->del_box = imax(14, pround(0.42 * kmin));
}

/* ---- hit rules ------------------------------------------------------------ */

int muse_pass_cell(const muse_pass_geom_t *g, double x, double y)
{
    if (g->layout == MUSE_PASS_RING) {
        double dx = x - g->cx, dy = y - g->cy;
        if (hypot(dx, dy) < g->center_hit_r) {
            return MUSE_PASS_KEY_CENTER;
        }
        /* The angle clockwise from 12 o'clock; each key's cell the 36 degree
         * sector around it, out to the glass. */
        double a = atan2(dx, -dy) * 180.0 / M_PI;
        if (a < 0) a += 360.0;
        int k = (int)floor((a + 18.0) / 36.0);
        return k % 10;
    }
    if (y < g->kp_top) {
        return MUSE_PASS_KEY_NONE;
    }
    /* The edge cells reach the glass under the mic ring band. */
    int col = (int)floor((x - g->kp_left) / g->cell_w);
    int row = (int)floor((y - g->kp_top) / g->cell_h);
    col = col < 0 ? 0 : col > 2 ? 2 : col;
    row = row > 3 ? 3 : row;
    if (row < 3) {
        return 1 + row * 3 + col;
    }
    return col == 0 ? MUSE_PASS_KEY_CENTER : col == 1 ? 0 : MUSE_PASS_KEY_DEL;
}

bool muse_pass_inner(const muse_pass_geom_t *g, int key, double x, double y)
{
    if (key < 0 || key > 9) {
        return false;
    }
    double dx = x - g->key_x[key], dy = y - g->key_y[key];
    if (g->layout == MUSE_PASS_RING) {
        return hypot(dx, dy) <= g->inner_r;
    }
    double ex = dx / g->inner_rx, ey = dy / g->inner_ry;
    return ex * ex + ey * ey <= 1.0;
}

static int push(int *out, int n, int ev)
{
    if (n < MUSE_PASS_EV_MAX) out[n++] = ev;
    return n;
}

static int registered(muse_pass_stroke_t *st, int key, int *out, int n)
{
    st->blocked = key;
    st->entered = true;
    st->stroke_keys |= (uint16_t)(1u << key);
    return push(out, n, key);
}

int muse_pass_bench_path(const muse_pass_geom_t *g, const char *keys, int *xs, int *ys, int max)
{
    int n = 0, k = 0;
    for (const char *p = keys; *p; p++) {
        if (*p == ',') continue;
        if (*p < '0' || *p > '9' || (p[1] && p[1] != ',')) return 0;   /* single digits, comma-separated */
        if (k > 0 && g->layout == MUSE_PASS_RING) {
            if (n >= max) return 0;
            xs[n] = (int)pround(g->cx);
            ys[n] = (int)pround(g->cy);
            n++;
        }
        if (n >= max) return 0;
        xs[n] = (int)pround(g->key_x[*p - '0']);
        ys[n] = (int)pround(g->key_y[*p - '0']);
        n++;
        k++;
    }
    return k >= 2 ? n : 0;
}

int muse_pass_stroke_down(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y,
                          int out[MUSE_PASS_EV_MAX])
{
    memset(st, 0, sizeof(*st));
    st->down = true;
    st->x0 = st->lx = x;
    st->y0 = st->ly = y;
    st->blocked = -1;
    st->land_cell = muse_pass_cell(g, x, y);
    int c = st->land_cell;
    st->under = c >= 0 && c <= 9 ? c : -1;
    int n = 0;
    if (c >= 0 && c <= 9) {
        n = registered(st, c, out, n);   /* touch-down anywhere in a key's cell enters it at once */
    }
    return n;
}

static int follow(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y, int *out, int n)
{
    st->lx = x;
    st->ly = y;
    if (!st->slop_passed && hypot(x - st->x0, y - st->y0) >= g->slop_px) {
        st->slop_passed = true;   /* no second key before the finger moved 1.5 mm */
    }
    int c = muse_pass_cell(g, x, y);
    st->under = c >= 0 && c <= 9 ? c : -1;
    if (st->blocked >= 0 && c != st->blocked) {
        st->blocked = -1;   /* left the key's cell: it may register again */
    }
    if (st->slop_passed && c >= 0 && c <= 9 && c != st->blocked && muse_pass_inner(g, c, x, y)) {
        n = registered(st, c, out, n);
    }
    return n;
}

int muse_pass_stroke_move(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y,
                          int out[MUSE_PASS_EV_MAX])
{
    if (!st->down) {
        return 0;
    }
    /* Along the line from the last sample, in steps well under the inner
     * area, so a swipe between two reads never skips a key. */
    double dx = x - st->lx, dy = y - st->ly;
    double len = hypot(dx, dy);
    double step = (g->layout == MUSE_PASS_RING ? g->inner_r : (g->inner_rx < g->inner_ry ? g->inner_rx : g->inner_ry)) / 3.0;
    int steps = step > 0 ? (int)ceil(len / step) : 1;
    if (steps < 1) steps = 1;
    if (steps > 256) steps = 256;
    double sx = st->lx, sy = st->ly;
    int n = 0;
    for (int i = 1; i <= steps; i++) {
        n = follow(st, g, sx + dx * i / steps, sy + dy * i / steps, out, n);
    }
    return n;
}

int muse_pass_stroke_up(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y,
                        int out[MUSE_PASS_EV_MAX])
{
    if (!st->down) {
        return 0;
    }
    int n = muse_pass_stroke_move(st, g, x, y, out);
    st->down = false;
    st->under = -1;
    /* Delete: a tap in the ring's center zone or on the grid's delete cell,
     * down and up inside it within the slop, no key entered. */
    bool del_cell = g->layout == MUSE_PASS_RING ? st->land_cell == MUSE_PASS_KEY_CENTER
                                                : st->land_cell == MUSE_PASS_KEY_DEL;
    if (del_cell && !st->entered && !st->slop_passed && muse_pass_cell(g, x, y) == st->land_cell) {
        n = push(out, n, MUSE_PASS_EV_DEL);
    }
    return n;
}
