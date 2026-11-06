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

#include <assert.h>
#include <string.h>


#ifndef BARRIER   /* a test may stand in here, to interleave the two sides */
#if defined(__GNUC__)
#define BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
#define BARRIER()
#endif
#endif

void muse_vstream_init(muse_vstream_t *s, uint8_t *buf, uint32_t size, uint32_t max_frame)
{
    /* Positions wrap with size - 1 as a mask. */
    assert(size && (size & (size - 1)) == 0);
    assert(max_frame <= size / 2);
    memset(s, 0, sizeof(*s));
    s->buf = buf;
    s->size = size;
    s->max_frame = max_frame;
}

/* The producer's side of a start: tail is the consumer's, and it may be
 * mid-pop, about to consume a frame of the old stream (which lies wholly
 * before head). So nothing is reset here: the new stream begins at head, and
 * the consumer skips to it at its next pop. */
void muse_vstream_start(muse_vstream_t *s)
{
    s->active = false;
    BARRIER();
    s->start_at = s->head;
    s->received = 0;
    s->resyncs = s->overruns = 0;
    BARRIER();
    s->starts++;
    BARRIER();
    s->active = true;
}

void muse_vstream_stop(muse_vstream_t *s)
{
    s->active = false;
}

uint32_t muse_vstream_available(const muse_vstream_t *s)
{
    /* After a start, what's left of the old stream is as good as gone, though
     * the consumer only skips it at its next pop: count from start_at, or the
     * credits the new stream starts with could be too few for its sender to
     * begin, and the consumer, with nothing to pop, would never skip. */
    uint32_t tail = s->started != s->starts ? s->start_at : s->tail;
    return s->size - (s->head - tail);
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

/* A VP8 frame tag at offset i that fits a frame of n bytes: version 0-3, its
 * first partition inside it, and for a keyframe the sync code 9d 01 2a
 * (RFC 6386 9.1). Checked as far as the bytes are in. */
static bool vp8_tag_ok(const muse_vstream_t *s, uint32_t avail, uint32_t i, uint32_t n, bool key)
{
    if (avail < i + 3) {
        return true;
    }
    uint32_t tag = (uint32_t)peek(s, i) | (uint32_t)peek(s, i + 1) << 8 | (uint32_t)peek(s, i + 2) << 16;
    bool is_key = !(tag & 1);
    if (((tag >> 1) & 7) > 3 || (tag >> 5) > n || is_key != key) {
        return false;
    }
    if (key && avail >= i + 6 && (peek(s, i + 3) != 0x9d || peek(s, i + 4) != 0x01 || peek(s, i + 5) != 0x2a)) {
        return false;
    }
    return true;
}

/* A VP8 frame, or two bands of them (the top band's u32 length first): which
 * one is told by whether a frame tag fits at the start. */
static bool vp8_frame_ok(const muse_vstream_t *s, uint32_t avail, const muse_vframe_t *f)
{
    bool key = f->flags & MUSE_VIDEO_FRAME_KEY;
    const uint32_t at = MUSE_VIDEO_FRAME_HEADER_LEN;
    if (f->len >= 3 && vp8_tag_ok(s, avail, at, f->len, key)) {
        return true;
    }
    if (f->len < 10 || avail < at + 4) {
        return avail < at + 4 && f->len >= 10;
    }
    uint32_t len0 = (uint32_t)peek(s, at) | (uint32_t)peek(s, at + 1) << 8 | (uint32_t)peek(s, at + 2) << 16 |
                    (uint32_t)peek(s, at + 3) << 24;
    if (len0 < 3 || len0 > f->len - 4 - 3) {
        return false;
    }
    return vp8_tag_ok(s, avail, at + 4, len0, key) && vp8_tag_ok(s, avail, at + 4 + len0, f->len - 4 - len0, key);
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
    if (s->codec == MUSE_VIDEO_CODEC_VP8) {
        return vp8_frame_ok(s, avail, f);
    }
    if (avail >= MUSE_VIDEO_FRAME_HEADER_LEN + 4 && !start_code_at(s, 8)) {
        /* Two bands (muse_video_params_t.tiles): the top band's u32
         * length, then its access unit. Its start code is checked as far as
         * the bytes are in, as for VP8: a frame that has only reached the
         * length so far waits for the rest instead of being skipped. */
        if (f->len < 8) {
            return false;
        }
        uint32_t len0 = (uint32_t)peek(s, 8) | (uint32_t)peek(s, 9) << 8 | (uint32_t)peek(s, 10) << 16 |
                        (uint32_t)peek(s, 11) << 24;
        if (len0 < 4 || len0 > f->len - 4) {
            return false;
        }
        if (avail >= MUSE_VIDEO_FRAME_HEADER_LEN + 8 && !start_code_at(s, 12)) {
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
        if (!s->active) {
            return damaged ? -1 : 0;   /* a stream ended or restarted under us */
        }
        if (s->started != s->starts) {
            /* A new stream: skip what's left of the old one. */
            uint32_t starts = s->starts;
            BARRIER();
            s->tail = s->start_at;
            s->started = starts;
            damaged = false;
        }
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
