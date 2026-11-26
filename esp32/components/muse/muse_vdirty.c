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

#include "muse_vdirty.h"

#include <string.h>

/* ---- the digest ---------------------------------------------------------- */

#define P1 0x9E3779B1u
#define P2 0x85EBCA77u
#define P3 0xC2B2AE3Du
#define P4 0x27D4EB2Fu

static inline uint32_t rotl(uint32_t x, int r)
{
    return x << r | x >> (32 - r);
}

static inline void mix(muse_vdirty_hash_t *h, uint32_t w)
{
    h->a = rotl(h->a + w * P2, 13) * P1;
    h->b = rotl(h->b ^ (w * P4), 17) * P3;
}

void muse_vdirty_hash_init(muse_vdirty_hash_t *h)
{
    h->a = 0x165667B1u;
    h->b = 0x61C88647u;
}

void muse_vdirty_hash_bytes(muse_vdirty_hash_t *h, const uint8_t *p, int n)
{
    if (n <= 0) {
        return;
    }
    if (((uintptr_t)p & 3) == 0) {
        const uint32_t *q = (const uint32_t *)p;
        for (; n >= 16; n -= 16, q += 4) {
            mix(h, q[0]);
            mix(h, q[1]);
            mix(h, q[2]);
            mix(h, q[3]);
        }
        for (; n >= 4; n -= 4) {
            mix(h, *q++);
        }
        p = (const uint8_t *)q;
    } else {
        for (; n >= 4; n -= 4, p += 4) {
            mix(h, (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        }
    }
    if (n > 0) {
        uint32_t w = (uint32_t)n << 24;   /* the tail's length too: 00 and 00 00 differ */
        for (int i = 0; i < n; i++) {
            w |= (uint32_t)p[i] << (8 * i);
        }
        mix(h, w);
    }
}

/* A whole macroblock with word-aligned rows (the decoders' planes): the same digest muse_vdirty_hash_bytes makes
 * of its 16 + 2 x 8 rows, without a function call or a check per row. */
static void mb_fast(muse_vdirty_hash_t *d, const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride,
                    int cstride)
{
    for (int r = 0; r < 16; r++) {
        const uint32_t *q = (const uint32_t *)(y + (size_t)r * ystride);
        mix(d, q[0]);
        mix(d, q[1]);
        mix(d, q[2]);
        mix(d, q[3]);
    }
    for (int r = 0; r < 8; r++) {
        const uint32_t *qu = (const uint32_t *)(u + (size_t)r * cstride), *qv = (const uint32_t *)(v + (size_t)r * cstride);
        mix(d, qu[0]);
        mix(d, qu[1]);
        mix(d, qv[0]);
        mix(d, qv[1]);
    }
}

int muse_vdirty_digest(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int w, int h,
                       muse_vdirty_hash_t *dg, bool prev_ok, const uint8_t *same, int same_cols, uint8_t *changed)
{
    const int cols = (w + 15) / 16, rows = (h + 15) / 16, cw = w / 2, ch = (h + 1) / 2;
    /* The planes' rows on whole words: every whole macroblock's rows are (its columns start on 16 and 8). */
    const bool aligned = ((((uintptr_t)y | (uintptr_t)u | (uintptr_t)v) | (uintptr_t)ystride | (uintptr_t)cstride) & 3) == 0;
    int n = 0;
    for (int r = 0; r < rows; r++) {
        int y0 = r * 16, y1 = y0 + 16 < h ? y0 + 16 : h;
        int c0 = r * 8, c1 = c0 + 8 < ch ? c0 + 8 : ch;
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
            if (prev_ok && same && same[r * same_cols + c]) {
                changed[i] = 0;
                continue;
            }
            int x0 = c * 16, x1 = x0 + 16 < w ? x0 + 16 : w;
            int u0 = c * 8, u1 = u0 + 8 < cw ? u0 + 8 : cw;
            muse_vdirty_hash_t d;
            muse_vdirty_hash_init(&d);
            if (aligned && x1 - x0 == 16 && y1 - y0 == 16 && u1 - u0 == 8 && c1 - c0 == 8) {
                mb_fast(&d, y + (size_t)y0 * ystride + x0, u + (size_t)c0 * cstride + u0, v + (size_t)c0 * cstride + u0,
                        ystride, cstride);
            } else {
                for (int yy = y0; yy < y1; yy++) {
                    muse_vdirty_hash_bytes(&d, y + (size_t)yy * ystride + x0, x1 - x0);
                }
                for (int yy = c0; yy < c1; yy++) {
                    muse_vdirty_hash_bytes(&d, u + (size_t)yy * cstride + u0, u1 - u0);
                    muse_vdirty_hash_bytes(&d, v + (size_t)yy * cstride + u0, u1 - u0);
                }
            }
            uint8_t ch1 = !prev_ok || !muse_vdirty_hash_eq(&d, &dg[i]);
            changed[i] = ch1;
            n += ch1;
            dg[i] = d;
        }
    }
    return n;
}

void muse_vdirty_or(uint8_t *acc, const uint8_t *map, int n)
{
    int i = 0;
    if ((((uintptr_t)acc | (uintptr_t)map) & 3) == 0) {
        for (; i + 4 <= n; i += 4) {
            *(uint32_t *)(acc + i) |= *(const uint32_t *)(map + i);
        }
    }
    for (; i < n; i++) {
        acc[i] |= map[i];
    }
}

/* ---- the output's cells -------------------------------------------------- */

int muse_vdirty_src(int o, int crop, int num, int den, int src_n)
{
    long long s = ((long long)(o + crop) * 2 + 1) * den, d = 2LL * num;
    long long q = s >= 0 ? s / d : -((-s + d - 1) / d);
    return q < 0 ? 0 : q >= src_n ? src_n - 1 : (int)q;
}

bool muse_vdirty_axis(muse_vdirty_axis_t *ax, int out_n, int src_n, int num, int den, int crop, int align)
{
    ax->n = 0;
    if (out_n <= 0 || src_n <= 0 || num <= 0 || den <= 0 || align <= 0 || (align & (align - 1)) ||
        src_n > 16 * 256) {
        return false;
    }
    int n = 0, o = 0;
    while (o < out_n) {
        if (n >= MUSE_VDIRTY_MAX_CELLS) {
            return false;
        }
        int mb = muse_vdirty_src(o, crop, num, den, src_n) / 16;
        int e = o + 1;
        while (e < out_n && muse_vdirty_src(e, crop, num, den, src_n) / 16 == mb) {
            e++;
        }
        if (e < out_n) {
            e = (e + align - 1) & ~(align - 1);
            e = e > out_n ? out_n : e;
        }
        ax->b[n] = (int16_t)o;
        ax->m0[n] = (uint8_t)mb;
        ax->m1[n] = (uint8_t)(muse_vdirty_src(e - 1, crop, num, den, src_n) / 16);
        n++;
        o = e;
    }
    ax->b[n] = (int16_t)out_n;
    ax->n = n;
    return true;
}

void muse_vdirty_cells(const uint8_t *mb, int mb_cols, int mb_rows, const muse_vdirty_axis_t *ax,
                       const muse_vdirty_axis_t *ay, uint8_t *cells)
{
    uint8_t col_or[64];   /* this cell row's macroblock rows, ORed, per macroblock column (wider: dirty) */
    int nc = mb_cols < (int)sizeof(col_or) ? mb_cols : (int)sizeof(col_or);
    for (int cy = 0; cy < ay->n; cy++) {
        bool out_rows = ay->m1[cy] >= mb_rows;
        memset(col_or, 0, (size_t)nc);
        for (int r = ay->m0[cy]; r <= ay->m1[cy] && r < mb_rows; r++) {
            muse_vdirty_or(col_or, mb + (size_t)r * mb_cols, nc);
        }
        uint8_t *row = cells + (size_t)cy * ax->n;
        for (int cx = 0; cx < ax->n; cx++) {
            uint8_t d = out_rows || ax->m1[cx] >= nc;
            for (int c = ax->m0[cx]; !d && c <= ax->m1[cx]; c++) {
                d = col_or[c];
            }
            row[cx] = d ? 1 : 0;
        }
    }
}

/* The cells [*i0, *i1) an output span [a, b) touches; false: none. */
static bool span(const muse_vdirty_axis_t *ax, int a, int b, int *i0, int *i1)
{
    a = a < 0 ? 0 : a;
    b = b > ax->b[ax->n] ? ax->b[ax->n] : b;
    if (b <= a) {
        return false;
    }
    int s = 0;
    while (s < ax->n && ax->b[s + 1] <= a) {
        s++;
    }
    int e = s;
    while (e < ax->n && ax->b[e] < b) {
        e++;
    }
    *i0 = s;
    *i1 = e;
    return e > s;
}

void muse_vdirty_mark(uint8_t *cells, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay, int x, int y, int w,
                      int h)
{
    int x0, x1, y0, y1;
    if (w <= 0 || h <= 0 || !span(ax, x, x + w, &x0, &x1) || !span(ay, y, y + h, &y0, &y1)) {
        return;
    }
    for (int cy = y0; cy < y1; cy++) {
        memset(cells + (size_t)cy * ax->n + x0, 1, (size_t)(x1 - x0));
    }
}

bool muse_vdirty_cell_hidden(const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay, int cx, int cy, int ox, int oy,
                             int r)
{
    int x0 = ax->b[cx], x1 = ax->b[cx + 1] - 1, y0 = ay->b[cy], y1 = ay->b[cy + 1] - 1;
    long long px = ox < x0 ? x0 : ox > x1 ? x1 : ox, py = oy < y0 ? y0 : oy > y1 ? y1 : oy;   /* its point nearest */
    long long dx = px - ox, dy = py - oy, rr = (long long)(r + 1) * (r + 1);
    return dx * dx + dy * dy > rr;
}

int muse_vdirty_clip_round(uint8_t *cells, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay, int ox, int oy,
                           int r)
{
    int n = 0;
    for (int cy = 0; cy < ay->n; cy++) {
        for (int cx = 0; cx < ax->n; cx++) {
            uint8_t *c = &cells[cy * ax->n + cx];
            if (*c && muse_vdirty_cell_hidden(ax, ay, cx, cy, ox, oy, r)) {
                *c = 0;
                n++;
            }
        }
    }
    return n;
}

/* ---- the windows --------------------------------------------------------- */

typedef struct {
    int r0, r1;   /* cell rows, inclusive */
    int a, b;     /* cell columns, inclusive */
} win_c_t;

static void put(muse_vdirty_win_t *out, int max_out, int *n, bool *over, int *area, const win_c_t *c,
                const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay)
{
    int x0 = ax->b[c->a], x1 = ax->b[c->b + 1], y0 = ay->b[c->r0], y1 = ay->b[c->r1 + 1];
    *area += (x1 - x0) * (y1 - y0);
    if (*n < max_out) {
        out[*n] = (muse_vdirty_win_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
    } else {
        *over = true;
    }
    (*n)++;
}

int muse_vdirty_plan(const uint8_t *cells, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay,
                     const muse_vdirty_cfg_t *cfg, muse_vdirty_win_t *out, int max_out, muse_vdirty_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->res = MUSE_VDIRTY_NONE;
    const int cols = ax->n, rows = ay->n;
    if (!cells || cols <= 0 || rows <= 0) {
        st->res = MUSE_VDIRTY_FULL_AREA;
        return 0;
    }
    st->cells = cols * rows;
    st->total = ax->b[cols] * ay->b[rows];
    win_c_t cur = { -1, -1, 0, 0 };
    int n = 0, area = 0;
    bool over = false;
    for (int r = 0; r < rows; r++) {
        const uint8_t *m = cells + (size_t)r * cols;
        int a = -1, b = -1;
        for (int c = 0; c < cols; c++) {
            if (m[c]) {
                a = a < 0 ? c : a;
                b = c;
                st->dirty++;
            }
        }
        if (a < 0) {
            continue;   /* nothing in this row */
        }
        if (cur.r0 >= 0) {
            /* Merged into the window open, or a window of its own: the cheaper. */
            int na = a < cur.a ? a : cur.a, nb = b > cur.b ? b : cur.b;
            int merged = (nb - na + 1) * (r - cur.r0 + 1);
            int apart = (cur.b - cur.a + 1) * (cur.r1 - cur.r0 + 1) + (b - a + 1) + cfg->win_cost;
            if (merged <= apart) {
                cur.a = na;
                cur.b = nb;
                cur.r1 = r;
                continue;
            }
            put(out, max_out, &n, &over, &area, &cur, ax, ay);
        }
        cur = (win_c_t){ r, r, a, b };
    }
    if (cur.r0 >= 0) {
        put(out, max_out, &n, &over, &area, &cur, ax, ay);
    }
    st->area = area;
    if (n == 0) {
        return 0;   /* MUSE_VDIRTY_NONE */
    }
    if (over || n > cfg->max_windows) {
        st->res = MUSE_VDIRTY_FULL_MANY;
        return 0;
    }
    if ((long long)area * 100 > (long long)cfg->full_pct * st->total) {
        st->res = MUSE_VDIRTY_FULL_AREA;
        return 0;
    }
    st->res = MUSE_VDIRTY_WINDOWS;
    return n;
}

/* ---- the panel's model --------------------------------------------------- */

bool muse_vdirty_model_rows(muse_vdirty_hash_t *cell_h, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay,
                            const uint8_t *px, int stride, int x, int y, int w, int rows)
{
    int x0, x1;
    if (rows <= 0 || !span(ax, x, x + w, &x0, &x1)) {
        return true;
    }
    if (ax->b[x0] != x || ax->b[x1] != x + w) {
        return false;   /* not whole cells */
    }
    int cy = 0;
    for (int r = 0; r < rows; r++) {
        int yy = y + r;
        while (cy < ay->n && ay->b[cy + 1] <= yy) {
            cy++;
        }
        if (cy >= ay->n || yy < 0) {
            continue;
        }
        bool first = ay->b[cy] == yy;
        muse_vdirty_hash_t *hr = cell_h + (size_t)cy * ax->n;
        const uint8_t *row = px + (size_t)r * stride;
        for (int cx = x0; cx < x1; cx++) {
            if (first) {
                muse_vdirty_hash_init(&hr[cx]);
            }
            muse_vdirty_hash_bytes(&hr[cx], row + (size_t)(ax->b[cx] - x) * 2, (ax->b[cx + 1] - ax->b[cx]) * 2);
        }
    }
    return true;
}
