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

#pragma once

/*
 * A ring of length-prefixed writes (2 bytes of length, little-endian, then the write), one producer and one
 * consumer, under the caller's lock: the SMP glue's FIFO (mg_ble.c). Host-tested (tests/test_mg_ble.py).
 *
 * head and tail count the bytes in and out, free-running: head - tail is what the ring holds, across their
 * wrap. Where they fall in the ring is kept apart (wpos, rpos, always below its size): the ring isn't a power of
 * two, so head % size would jump at the counters' wrap (after about 4 GB) and misread every length after it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t head, tail;   /* bytes in and out, free-running */
    uint32_t wpos, rpos;   /* where the next byte goes in and comes out: 0 .. size - 1 */
} mg_fifo_t;

static inline uint32_t mg_fifo_put(uint8_t *ring, uint32_t size, uint32_t at, const uint8_t *p, uint32_t n)
{
    uint32_t k = size - at < n ? size - at : n;
    memcpy(ring + at, p, k);
    memcpy(ring, p + k, n - k);
    at += n;
    return at >= size ? at - size : at;
}

static inline uint32_t mg_fifo_get(const uint8_t *ring, uint32_t size, uint32_t at, uint8_t *p, uint32_t n)
{
    uint32_t k = size - at < n ? size - at : n;
    memcpy(p, ring + at, k);
    memcpy(p + k, ring, n - k);
    at += n;
    return at >= size ? at - size : at;
}

static inline bool mg_fifo_empty(const mg_fifo_t *f)
{
    return f->head == f->tail;
}

/* One write in, if there's room for it now. */
static inline bool mg_fifo_push(mg_fifo_t *f, uint8_t *ring, uint32_t size, const uint8_t *msg, uint16_t n)
{
    if (size - (f->head - f->tail) < 2u + n) {
        return false;
    }
    const uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
    f->wpos = mg_fifo_put(ring, size, f->wpos, len, 2);
    f->wpos = mg_fifo_put(ring, size, f->wpos, msg, n);
    f->head += 2u + n;
    return true;
}

/* The oldest write out: its length, 0 for an empty write (or one longer than cap: dropped), -1 when empty. */
static inline int mg_fifo_pop(mg_fifo_t *f, const uint8_t *ring, uint32_t size, uint8_t *out, size_t cap)
{
    if (mg_fifo_empty(f)) {
        return -1;
    }
    uint8_t len[2];
    f->rpos = mg_fifo_get(ring, size, f->rpos, len, 2);
    uint32_t k = (uint32_t)(len[0] | len[1] << 8);
    int n = k <= cap ? (int)k : 0;
    if (n) {
        f->rpos = mg_fifo_get(ring, size, f->rpos, out, k);
    } else {
        f->rpos = (uint32_t)(((uint64_t)f->rpos + k) % size);   /* skipped */
    }
    f->tail += 2u + k;
    return n;
}
