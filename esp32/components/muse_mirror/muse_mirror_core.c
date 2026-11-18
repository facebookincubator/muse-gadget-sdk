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
#include "muse_mirror_core.h"

#include <string.h>

#define WHOLE_PX 60000
#define RECT_HDR MUSE_MIRROR_RECT_HDR

bool muse_mirror_core_init(muse_mirror_core_t *c, int w, int h)
{
    memset(c, 0, sizeof(*c));
    if (w <= 0 || h <= 0 || w > UINT16_MAX || h > UINT16_MAX || (size_t)w > SIZE_MAX / 2 / h) return false;
    c->w = w;
    c->h = h;
    return true;
}

bool muse_mirror_core_pending(const muse_mirror_core_t *c)
{
    return c->ndirty > 0 || c->tail_on;
}

void muse_mirror_core_dirty(muse_mirror_core_t *c, int x1, int y1, int x2, int y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= c->w) x2 = c->w - 1;
    if (y2 >= c->h) y2 = c->h - 1;
    if (x1 > x2 || y1 > y2) return;
    for (int i = 0; i < c->ndirty; i++) {
        /* Touching or overlapping: one rectangle. */
        if (x1 <= c->dirty[i].x2 + 1 && x2 >= c->dirty[i].x1 - 1 && y1 <= c->dirty[i].y2 + 1 && y2 >= c->dirty[i].y1 - 1) {
            if (x1 < c->dirty[i].x1) c->dirty[i].x1 = x1;
            if (y1 < c->dirty[i].y1) c->dirty[i].y1 = y1;
            if (x2 > c->dirty[i].x2) c->dirty[i].x2 = x2;
            if (y2 > c->dirty[i].y2) c->dirty[i].y2 = y2;
            return;
        }
    }
    if (c->ndirty < MUSE_MIRROR_MAX_DIRTY) {
        c->dirty[c->ndirty].x1 = x1;
        c->dirty[c->ndirty].y1 = y1;
        c->dirty[c->ndirty].x2 = x2;
        c->dirty[c->ndirty].y2 = y2;
        c->ndirty++;
    } else {
        if (x1 < c->dirty[0].x1) c->dirty[0].x1 = x1;
        if (y1 < c->dirty[0].y1) c->dirty[0].y1 = y1;
        if (x2 > c->dirty[0].x2) c->dirty[0].x2 = x2;
        if (y2 > c->dirty[0].y2) c->dirty[0].y2 = y2;
    }
}

static inline void put16(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static inline void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* Stop counting once the candidate cannot beat limit. No candidate scratch. */
static size_t rle(const uint16_t *fb, int stride,
                  uint8_t *out, size_t limit, int x, int y, int w, int h, bool cols)
{
    size_t n = 0;
    uint16_t colour = 0;
    unsigned run = 0;
    int outer = cols ? w : h, inner = cols ? h : w;
    for (int a = 0; a < outer; a++) {
        size_t base = cols ? (size_t)y * stride + x + a : (size_t)(y + a) * stride + x;
        int step = cols ? stride : 1;
        for (int b = 0; b < inner; b++) {
            size_t pos = base + (size_t)b * step;
            uint16_t px = fb[pos];
            if (run && px == colour && run < UINT16_MAX) {
                run++;
                continue;
            }
            if (run) {
                if (n + 4 > limit) return SIZE_MAX;
                if (out) { put16(out + n, run); put16(out + n + 2, colour); }
                n += 4;
            }
            colour = px;
            run = 1;
        }
    }
    if (n + 4 > limit) return SIZE_MAX;
    if (out) { put16(out + n, run); put16(out + n + 2, colour); }
    return n + 4;
}

static size_t code_rect(const muse_mirror_core_t *c, const uint16_t *fb, uint8_t *out,
                        size_t room, int x, int y, int w, int h, bool try_cols)
{
    if (room <= RECT_HDR) return 0;
    size_t n = (size_t)w * h * 2;
    int enc = 0;
    uint8_t *p = out + RECT_HDR;
    for (int candidate = 1; candidate <= (try_cols ? 2 : 1); candidate++) {
        size_t limit = n - 1;
        if (limit > room - RECT_HDR) limit = room - RECT_HDR;
        /* Rows alone are written as they are counted; with columns to compare, the winner is written below. */
        size_t size = try_cols ? rle(fb, c->w, NULL, limit, x, y, w, h, candidate == 2)
                               : rle(fb, c->w, p, limit, x, y, w, h, false);
        if (size < n) { n = size; enc = candidate; }
    }
    if (RECT_HDR + n > room) return 0;
    if (enc && try_cols) {
        rle(fb, c->w, p, n, x, y, w, h, enc == 2);
    } else if (!enc) {
        for (int r = 0; r < h; r++) {
            const uint16_t *row = fb + (size_t)(y + r) * c->w + x;
            for (int i = 0; i < w; i++) put16(p + ((size_t)r * w + i) * 2, row[i]);
        }
    }
    put16(out, x);
    put16(out + 2, y);
    put16(out + 4, w);
    put16(out + 6, h);
    out[8] = enc;
    out[9] = 0;
    put32(out + 10, n);
    return RECT_HDR + n;
}

size_t muse_mirror_core_build(muse_mirror_core_t *c, const uint16_t *fb,
                              uint8_t *out, size_t capacity, uint8_t state)
{
    if (c->w <= 0 || c->h <= 0 || capacity < 64 ||
        capacity < (size_t)c->w * 2 + MUSE_MIRROR_RECT_HDR + 8) return 0;
    muse_mirror_rect_t todo[MUSE_MIRROR_MAX_DIRTY];
    int nt = c->tail_on ? 0 : c->ndirty;
    memcpy(todo, c->dirty, sizeof(todo[0]) * nt);
    if (!c->tail_on) c->ndirty = 0;

    out[0] = 'F';
    out[1] = state;
    put16(out + 4, c->w);
    put16(out + 6, c->h);
    size_t len = 8;
    int rects = 0;
    /* The tail of the last message (so a screen that changes all the time still
     * gets through to its bottom), or the dirty list; what does not fit stays
     * for the next message. */
    bool tail_first = c->tail_on;
    c->tail_on = false;
    for (int i = tail_first ? -1 : 0; i < nt; i++) {
        int x1 = i < 0 ? c->tail.x1 : todo[i].x1, y1 = i < 0 ? c->tail.y1 : todo[i].y1;
        int x2 = i < 0 ? c->tail.x2 : todo[i].x2, y2 = i < 0 ? c->tail.y2 : todo[i].y2;
        int w = x2 - x1 + 1, h_all = y2 - y1 + 1;
        size_t n = 0;
        if ((size_t)w * h_all <= WHOLE_PX) n = code_rect(c, fb, out + len, capacity - len, x1, y1, w, h_all, true);
        if (n) {
            len += n;
            rects++;
            continue;
        }
        /* Big, or too big for this message as a whole: bands whose raw size always fits an empty message. */
        int band = (int)(((capacity - 64) / 2) / w);
        if (band < 1) band = 1;
        for (int y = y1; y <= y2;) {
            int h = y2 - y + 1 < band ? y2 - y + 1 : band;
            n = code_rect(c, fb, out + len, capacity - len, x1, y, w, h, false);
            if (!n) {
                if (c->tail_on) {
                    muse_mirror_core_dirty(c, x1, y, x2, y2);       /* a second tail waits its turn behind the first */
                } else {
                    c->tail.x1 = x1;
                    c->tail.y1 = y;
                    c->tail.x2 = x2;
                    c->tail.y2 = y2;
                    c->tail_on = true;
                }
                break;
            }
            len += n;
            rects++;
            y += h;
        }
    }
    put16(out + 2, rects);
    return len;
}

bool muse_mirror_core_text(const uint8_t *buf, size_t len, uint8_t *edit, char text[64])
{
    memset(text, 0, 64);
    *edit = 0;
    if (len == 2 && buf[0] == 'K' && buf[1] >= 1 && buf[1] <= 5) {
        *edit = buf[1];
        return true;
    }
    if (len < 2 || len > 64 || buf[0] != 'I') return false;
    for (size_t i = 1; i < len;) {
        uint32_t c = buf[i++], min = 0;
        unsigned more = 0;
        if (c >= 0xC2 && c <= 0xDF) { c &= 31; more = 1; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { c &= 15; more = 2; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { c &= 7; more = 3; min = 0x10000; }
        else if (c >= 0x80 || !c) return false;
        if (i + more > len) return false;
        while (more--) {
            if ((buf[i] & 0xC0) != 0x80) return false;
            c = c << 6 | (buf[i++] & 63);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
    }
    memcpy(text, buf + 1, len - 1);
    return true;
}
