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

#include "mg_queue.h"

#include <string.h>

#define MAGIC 0x3151474Du    /* "MGQ1" little-endian: a 16-byte header */
#define MAGIC2 0x3251474Du   /* "MGQ2": a 24-byte one, + start_unix_ms */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

bool mg_queue_mount(mg_queue_t *q, const mg_store_t *store)
{
    memset(q, 0, sizeof(*q));
    if (!store || !store->size || !store->sector || store->size % store->sector) {
        return false;
    }
    q->store = *store;
    uint32_t off = 0;
    for (;;) {
        uint8_t h[MG_QUEUE_HEADER2_SIZE];
        if (off + MG_QUEUE_HEADER_SIZE > q->store.size) {
            break;
        }
        if (!q->store.read(q->store.ctx, off, h, MG_QUEUE_HEADER_SIZE)) {
            q->dirty = true;   /* unread, not proven blank: clips may lie past here, so none is written over them */
            break;
        }
        if (le32(h) != MAGIC && le32(h) != MAGIC2) {
            break;
        }
        uint32_t hs = le32(h) == MAGIC2 ? MG_QUEUE_HEADER2_SIZE : MG_QUEUE_HEADER_SIZE;
        if (off + hs > q->store.size) {
            break;
        }
        uint32_t bytes = le32(h + 4);
        uint32_t next = align4(off + hs + bytes);
        if (bytes == 0xFFFFFFFFu || next > q->store.size || next <= off) {
            break;   /* a header cut off before its length was written */
        }
        if (h[13] == 0x00 && q->count < MG_QUEUE_MAX_CLIPS) {
            q->clips[q->count++] = (mg_clip_t){
                .offset = off, .bytes = bytes, .start_ms = le32(h + 8), .codec = h[12],
                .header = (uint8_t)hs,
            };
        }
        off = next;
    }
    q->end = off;
    /*
     * Every sector the log enters is erased first, so the rest of the one it
     * ends in is erased too. Unless a reset cut off a header as it was being
     * written: then the queue takes no more clips until it's cleared.
     */
    uint8_t tail[MG_QUEUE_HEADER2_SIZE];
    uint32_t n = q->store.size - off < sizeof(tail) ? q->store.size - off : sizeof(tail);
    if (n && q->store.read(q->store.ctx, off, tail, n)) {
        for (uint32_t i = 0; i < n; i++) {
            q->dirty |= tail[i] != 0xFF;
        }
    } else if (n) {
        q->dirty = true;   /* the tail unread: not known to be erased */
    }
    uint32_t sec = q->store.sector;
    q->erased_to = off % sec ? (off / sec + 1) * sec : off;
    return true;
}

uint32_t mg_queue_used(const mg_queue_t *q)
{
    return q->end;
}

uint32_t mg_queue_capacity(const mg_queue_t *q)
{
    return q->store.size;
}

bool mg_queue_fits(const mg_queue_t *q, uint32_t bytes)
{
    return q->store.size && !q->dirty && !q->wipe_to && q->count < MG_QUEUE_MAX_CLIPS
           && (uint64_t)q->end + MG_QUEUE_HEADER2_SIZE + bytes <= q->store.size;
}

const mg_clip_t *mg_queue_clip(const mg_queue_t *q, uint16_t index)
{
    return index < q->count ? &q->clips[index] : NULL;
}

/* Erases whole sectors so that [q->end, to) is erased. */
static bool erase_to(mg_queue_t *q, uint32_t to)
{
    uint32_t sec = q->store.sector;
    while (q->erased_to < to) {
        uint32_t start = q->erased_to - q->erased_to % sec;
        if (start < q->erased_to) {
            start += sec;   /* the rest of this sector is already erased */
            q->erased_to = start;
            continue;
        }
        if (!q->store.erase(q->store.ctx, start, sec)) {
            return false;
        }
        q->erased_to = start + sec;
    }
    return true;
}

/* Where a clip of `bytes` would end, and how far it needs erasing: also where
 * the next header goes, so the log visibly ends there. */
static uint32_t clip_end_h(const mg_queue_t *q, uint32_t header, uint32_t bytes, uint32_t *need)
{
    uint32_t next = align4(q->end + header + bytes);
    *need = next + MG_QUEUE_HEADER2_SIZE <= q->store.size ? next + MG_QUEUE_HEADER2_SIZE : q->store.size;
    return next;
}

static uint32_t clip_end(const mg_queue_t *q, uint32_t bytes, uint32_t *need)
{
    return clip_end_h(q, MG_QUEUE_HEADER2_SIZE, bytes, need);   /* the larger header: prepare covers either */
}

/* The first sector start at or past erased_to: everything below it is erased. */
static uint32_t erased_sector_end(const mg_queue_t *q)
{
    uint32_t sec = q->store.sector;
    return q->erased_to % sec ? (q->erased_to / sec + 1) * sec : q->erased_to;
}

bool mg_queue_prepare_next(const mg_queue_t *q, uint32_t bytes, uint32_t *sector_off, uint32_t *gen)
{
    if (!bytes || !mg_queue_fits(q, bytes)) {
        return false;
    }
    uint32_t need;
    clip_end(q, bytes, &need);
    uint32_t start = erased_sector_end(q);
    if (start >= need) {
        return false;
    }
    *sector_off = start;
    *gen = q->gen;
    return true;
}

void mg_queue_prepare_done(mg_queue_t *q, uint32_t sector_off, uint32_t gen)
{
    if (q->gen == gen && erased_sector_end(q) == sector_off) {
        q->erased_to = sector_off + q->store.sector;
    }
}

bool mg_queue_prepare(mg_queue_t *q, uint32_t bytes)
{
    if (!bytes || !mg_queue_fits(q, bytes)) {
        return false;
    }
    uint32_t off, gen;
    while (mg_queue_prepare_next(q, bytes, &off, &gen)) {
        if (!q->store.erase(q->store.ctx, off, q->store.sector)) {
            return false;
        }
        mg_queue_prepare_done(q, off, gen);
    }
    return true;
}

bool mg_queue_append(mg_queue_t *q, uint8_t codec, uint32_t start_ms, const uint8_t *data, uint32_t bytes)
{
    return mg_queue_append_at(q, codec, start_ms, 0, data, bytes);
}

bool mg_queue_append_at(mg_queue_t *q, uint8_t codec, uint32_t start_ms, uint64_t start_unix_ms, const uint8_t *data,
                        uint32_t bytes)
{
    if (!bytes || !mg_queue_fits(q, bytes)) {
        return false;
    }
    uint32_t hs = start_unix_ms ? MG_QUEUE_HEADER2_SIZE : MG_QUEUE_HEADER_SIZE;
    uint32_t off = q->end;
    uint32_t need;
    uint32_t next = clip_end_h(q, hs, bytes, &need);
    if (!erase_to(q, need)) {
        return false;
    }
    uint8_t h[MG_QUEUE_HEADER2_SIZE];
    memset(h, 0xFF, sizeof(h));
    put32(h, hs == MG_QUEUE_HEADER2_SIZE ? MAGIC2 : MAGIC);
    put32(h + 4, bytes);
    put32(h + 8, start_ms);
    h[12] = codec;
    put32(h + 16, (uint32_t)start_unix_ms);
    put32(h + 20, (uint32_t)(start_unix_ms >> 32));
    uint8_t committed = 0x00;
    if (!q->store.write(q->store.ctx, off, h, hs)
        || !q->store.write(q->store.ctx, off + hs, data, bytes)) {
        q->dirty = true;   /* don't write over it; full until cleared */
        return false;
    }
    if (!q->store.write(q->store.ctx, off + 13, &committed, 1)) {
        q->end = next;   /* uncommitted: skipped on the next mount */
        return false;
    }
    q->end = next;
    q->clips[q->count++] = (mg_clip_t){ .offset = off, .bytes = bytes, .start_ms = start_ms, .codec = codec,
                                        .header = (uint8_t)hs };
    return true;
}

uint64_t mg_queue_clip_unix_ms(const mg_queue_t *q, uint16_t index)
{
    const mg_clip_t *c = mg_queue_clip(q, index);
    uint8_t t[8];
    if (!c || c->header != MG_QUEUE_HEADER2_SIZE || !q->store.read(q->store.ctx, c->offset + 16, t, sizeof(t))) {
        return 0;
    }
    return (uint64_t)le32(t) | (uint64_t)le32(t + 4) << 32;
}

bool mg_queue_read(const mg_queue_t *q, uint16_t index, uint32_t off, void *buf, size_t n)
{
    const mg_clip_t *c = mg_queue_clip(q, index);
    if (!c || off > c->bytes || n > c->bytes - off) {
        return false;
    }
    return q->store.read(q->store.ctx, c->offset + c->header + off, buf, n);
}

bool mg_queue_clear(mg_queue_t *q)
{
    q->gen++;
    if (!q->store.size || !q->store.erase(q->store.ctx, 0, q->store.sector)) {
        /* Nothing it held is readable or added to until a clear succeeds. */
        q->count = 0;
        q->dirty = true;
        return false;
    }
    q->end = 0;
    q->erased_to = q->store.sector;
    q->count = 0;
    q->dirty = false;
    return true;
}

uint32_t mg_queue_written_to(const mg_queue_t *q)
{
    return q->store.size;
}

bool mg_queue_wipe_begin(mg_queue_t *q, uint32_t to)
{
    if (!mg_queue_clear(q)) {
        return false;
    }
    uint32_t sec = q->store.sector;
    to = to >= q->store.size ? q->store.size : to - to % sec + (to % sec ? sec : 0);
    q->wipe_next = sec;
    q->wipe_to = to > sec ? to : 0;
    return true;
}

bool mg_queue_wipe_next(const mg_queue_t *q, uint32_t *sector_off)
{
    if (!q->wipe_to) {
        return false;
    }
    *sector_off = q->wipe_next;
    return true;
}

void mg_queue_wipe_done(mg_queue_t *q, uint32_t sector_off, bool erased)
{
    if (!erased || !q->wipe_to || sector_off != q->wipe_next) {
        return;   /* tried again on the next step; a wipe begun again meanwhile has its own */
    }
    q->wipe_next += q->store.sector;
    if (q->wipe_next >= q->wipe_to) {
        if (!q->count && !q->end) {
            q->erased_to = q->wipe_to;   /* [0, wipe_to) is all erased and the log is empty */
        }
        q->wipe_to = q->wipe_next = 0;
    }
}

bool mg_queue_sector_blank(const mg_queue_t *q, uint32_t sector_off)
{
    uint8_t buf[256];
    for (uint32_t at = 0; at < q->store.sector; at += sizeof(buf)) {
        size_t n = q->store.sector - at < sizeof(buf) ? q->store.sector - at : sizeof(buf);
        if (!q->store.read(q->store.ctx, sector_off + at, buf, n)) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != 0xFF) {
                return false;
            }
        }
    }
    return true;
}

bool mg_queue_wipe_step(mg_queue_t *q)
{
    uint32_t off;
    if (!mg_queue_wipe_next(q, &off)) {
        return false;
    }
    bool ok = mg_queue_sector_blank(q, off) || q->store.erase(q->store.ctx, off, q->store.sector);
    mg_queue_wipe_done(q, off, ok);
    return mg_queue_wiping(q);
}

bool mg_queue_wiping(const mg_queue_t *q)
{
    return q->wipe_to != 0;
}
