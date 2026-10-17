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
#include "muse_vstream.h"

#include <string.h>


#if defined(__GNUC__)
#define BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
#define BARRIER()
#endif

void muse_vstream_init(muse_vstream_t *s, uint8_t *buf, uint32_t size, uint32_t max_frame)
{
    memset(s, 0, sizeof(*s));
    s->buf = buf;
    s->size = size;
    s->max_frame = max_frame;
}

void muse_vstream_start(muse_vstream_t *s)
{
    s->active = false;
    BARRIER();
    s->head = s->tail = 0;
    s->received = 0;
    s->resyncs = s->overruns = 0;
    BARRIER();
    s->active = true;
}

void muse_vstream_stop(muse_vstream_t *s)
{
    s->active = false;
}

uint32_t muse_vstream_available(const muse_vstream_t *s)
{
    return s->size - (s->head - s->tail);
}

uint32_t muse_vstream_received(const muse_vstream_t *s)
{
    return s->received;
}

uint32_t muse_vstream_space(muse_vstream_t *s, uint8_t **dst)
{
    if (!s->active || !s->buf) {
        return 0;
    }
    uint32_t room = muse_vstream_available(s);
    uint32_t at = s->head & (s->size - 1);
    uint32_t run = s->size - at < room ? s->size - at : room;
    *dst = s->buf + at;
    return run;
}

void muse_vstream_commit(muse_vstream_t *s, uint32_t n)
{
    s->received += n;
    BARRIER();
    s->head += n;
}

void muse_vstream_write(muse_vstream_t *s, const uint8_t *d, size_t n)
{
    if (!s->active || !s->buf) {
        return;
    }
    s->received += (uint32_t)n;
    uint32_t room = muse_vstream_available(s);
    if (n > room) {
        s->overruns += (uint32_t)(n - room);
        n = room;   /* the rest is lost; the consumer resyncs on the next header */
    }
    uint32_t h = s->head;
    uint32_t at = h & (s->size - 1);
    uint32_t first = s->size - at < n ? s->size - at : (uint32_t)n;
    memcpy(s->buf + at, d, first);
    memcpy(s->buf, d + first, n - first);
    BARRIER();
    s->head = h + (uint32_t)n;
}

static uint8_t peek(const muse_vstream_t *s, uint32_t off)
{
    return s->buf[(s->tail + off) & (s->size - 1)];
}

static void copy_out(muse_vstream_t *s, uint32_t off, uint8_t *dst, uint32_t n)
{
    uint32_t at = (s->tail + off) & (s->size - 1);
    uint32_t first = s->size - at < n ? s->size - at : n;
    memcpy(dst, s->buf + at, first);
    memcpy(dst + first, s->buf, n - first);
}

static void consume(muse_vstream_t *s, uint32_t n)
{
    BARRIER();
    s->tail += n;
}

/* A header that looks right: magic, a sane length, and a start code after it. */
/* 00 00 01 or 00 00 00 01 at offset i */
static bool start_code_at(const muse_vstream_t *s, uint32_t i)
{
    if (peek(s, i) != 0 || peek(s, i + 1) != 0) {
        return false;
    }
    uint8_t c = peek(s, i + 2);
    return c == 1 || (c == 0 && peek(s, i + 3) == 1);
}

static bool header_at(const muse_vstream_t *s, uint32_t avail, muse_vframe_t *f)
{
    if (peek(s, 0) != MUSE_VIDEO_FRAME_MAGIC) {
        return false;
    }
    f->flags = peek(s, 1);
    f->number = (uint16_t)(peek(s, 2) | peek(s, 3) << 8);
    f->len = (uint32_t)peek(s, 4) | (uint32_t)peek(s, 5) << 8 | (uint32_t)peek(s, 6) << 16 |
             (uint32_t)peek(s, 7) << 24;
    if (f->len < 4 || f->len > s->max_frame) {
        return false;
    }
    if (avail >= MUSE_VIDEO_FRAME_HEADER_LEN + 4 && !start_code_at(s, 8)) {
        /* Two bands (muse_video_params_t.tiles): the top band's u32
         * length, then its access unit. */
        if (f->len < 8 || avail < MUSE_VIDEO_FRAME_HEADER_LEN + 8 || !start_code_at(s, 12)) {
            return false;
        }
        uint32_t len0 = (uint32_t)peek(s, 8) | (uint32_t)peek(s, 9) << 8 | (uint32_t)peek(s, 10) << 16 |
                        (uint32_t)peek(s, 11) << 24;
        if (len0 < 4 || len0 > f->len - 4) {
            return false;
        }
    }
    return true;
}

int32_t muse_vstream_pop(muse_vstream_t *s, uint8_t *dst, uint32_t cap, muse_vframe_t *f)
{
    bool damaged = false;
    f->after_gap = false;
    for (;;) {
        uint32_t avail = s->head - s->tail;
        BARRIER();
        if (avail < MUSE_VIDEO_FRAME_HEADER_LEN + 4) {
            return damaged ? -1 : 0;
        }
        if (!header_at(s, avail, f)) {
            /* Search byte by byte for the next frame. */
            consume(s, 1);
            if (!damaged) {
                s->resyncs++;
            }
            damaged = true;
            continue;
        }
        if (avail < MUSE_VIDEO_FRAME_HEADER_LEN + f->len) {
            return damaged ? -1 : 0;   /* the rest is on its way */
        }
        if (f->len > cap) {
            consume(s, MUSE_VIDEO_FRAME_HEADER_LEN + f->len);
            return -1;
        }
        copy_out(s, MUSE_VIDEO_FRAME_HEADER_LEN, dst, f->len);
        consume(s, MUSE_VIDEO_FRAME_HEADER_LEN + f->len);
        f->after_gap = damaged;   /* may depend on what was lost: the caller decides */
        return (int32_t)f->len;
    }
}
