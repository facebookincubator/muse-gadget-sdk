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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The offline clip queue (mg_command_audio_queue): push-to-talk clips
 * recorded while no mg client is connected, kept in a flash partition until
 * a client reads them and clears the queue.
 *
 * Layout: an append-only log from offset 0. Each clip is a 16-byte header
 *   LE32 magic "MGQ1", LE32 compressed bytes, LE32 start uptime ms,
 *   u8 codec (mg_data_type_t), u8 committed (0xFF, then 0x00), 2 bytes 0xFF
 * and its codec frames, then the next header at the next 4-byte boundary. A
 * whole clip is written at once from RAM, header first; `committed` is
 * programmed last, so a clip cut off by a reset is skipped. The log ends at
 * the first offset without the magic. Sectors are erased as the log enters
 * them, so clearing only erases the first sector. There's no wrap: a full
 * queue refuses new clips until it's cleared, as does one whose last header
 * was cut off as it was being written.
 */

#define MG_QUEUE_MAX_CLIPS 64
#define MG_QUEUE_HEADER_SIZE 16      /* "MGQ1": a clip without wall time */
#define MG_QUEUE_HEADER2_SIZE 24     /* "MGQ2": + uint64 start_unix_ms at 16 (clip_info) */

typedef struct {
    void *ctx;
    uint32_t size;     /* bytes, a multiple of sector */
    uint32_t sector;   /* erase unit */
    bool (*read)(void *ctx, uint32_t off, void *buf, size_t n);
    bool (*write)(void *ctx, uint32_t off, const void *buf, size_t n);
    bool (*erase)(void *ctx, uint32_t off, size_t n);   /* sector-aligned */
} mg_store_t;

typedef struct {
    uint32_t offset;   /* of the header */
    uint32_t bytes;
    uint32_t start_ms;
    uint8_t codec;
    uint8_t header;   /* its header's size (16, or 24 with start_unix_ms: mg_queue_clip_unix_ms) */
} mg_clip_t;          /* 16 bytes: 64 of them sit in internal RAM on the StopWatch */

typedef struct {
    mg_store_t store;
    uint32_t end;        /* where the next header goes */
    uint32_t erased_to;  /* everything from end up to here is erased */
    uint32_t gen;        /* bumped by every clear: an erase ahead begun before it doesn't count */
    uint16_t count;
    bool dirty;          /* a cut-off header ends the log: full until cleared */
    uint32_t wipe_next;  /* a wipe under way (mg_queue_wipe_begin): the next sector to erase ... */
    uint32_t wipe_to;    /* ... up to here; 0: none */
    mg_clip_t clips[MG_QUEUE_MAX_CLIPS];
} mg_queue_t;

/* Scans the log. False if the store is unusable. */
bool mg_queue_mount(mg_queue_t *q, const mg_store_t *store);
uint32_t mg_queue_used(const mg_queue_t *q);
uint32_t mg_queue_capacity(const mg_queue_t *q);
/* Whether a clip of `bytes` fits. */
bool mg_queue_fits(const mg_queue_t *q, uint32_t bytes);
const mg_clip_t *mg_queue_clip(const mg_queue_t *q, uint16_t index);
/* Erases ahead for a clip of `bytes`, so the append that follows only writes.
 * Optional. For a caller that erases outside the lock guarding q (the slow
 * part), the same in steps: mg_queue_prepare_next, under the lock, names the
 * next sector to erase (false: none left, or the clip doesn't fit); the caller
 * erases it without the lock, then reports it under the lock again with
 * mg_queue_prepare_done, which counts it only if no clear came between. */
bool mg_queue_prepare(mg_queue_t *q, uint32_t bytes);
bool mg_queue_prepare_next(const mg_queue_t *q, uint32_t bytes, uint32_t *sector_off, uint32_t *gen);
void mg_queue_prepare_done(mg_queue_t *q, uint32_t sector_off, uint32_t gen);
bool mg_queue_append(mg_queue_t *q, uint8_t codec, uint32_t start_ms, const uint8_t *data, uint32_t bytes);
/* The same with the clip's wall-clock start (ms since the epoch; 0: unknown,
 * stored as before, in the 16-byte header). */
bool mg_queue_append_at(mg_queue_t *q, uint8_t codec, uint32_t start_ms, uint64_t start_unix_ms, const uint8_t *data,
                        uint32_t bytes);
/* Clip `index`'s wall-clock start (ms since the epoch), read from its header
 * in the store; 0 when it has none (the clock wasn't set) or the read fails. */
uint64_t mg_queue_clip_unix_ms(const mg_queue_t *q, uint16_t index);

/* Reads `n` bytes of clip `index`'s data from `off`. */
bool mg_queue_read(const mg_queue_t *q, uint16_t index, uint32_t off, void *buf, size_t n);
bool mg_queue_clear(mg_queue_t *q);

/*
 * The wipe at an ownership change: every byte an earlier owner's clips may
 * have reached, erased. mg_queue_written_to() is how far that is: the whole
 * store, since a clear erases only the head and clips from before it (or
 * before a reboot) can sit past the log's current end. mg_queue_wipe_begin()
 * clears the queue (the head first, so the log is gone at once) and arms the
 * rest, [one sector, to). While a wipe is under way the queue takes no clips.
 * mg_queue_wipe_step() does one sector (skipped when already blank) and
 * returns whether more remain. For a caller that erases outside the lock
 * guarding q, the same in steps: mg_queue_wipe_next, under the lock, names the
 * sector; the caller checks it (mg_queue_sector_blank) and erases it without
 * the lock; mg_queue_wipe_done, under the lock again, moves on if it worked.
 * Erasing is idempotent: after a power cut, begin again with the same `to`
 * (the caller keeps it, e.g. in NVS, until the wipe ends).
 */
uint32_t mg_queue_written_to(const mg_queue_t *q);
bool mg_queue_wipe_begin(mg_queue_t *q, uint32_t to);
bool mg_queue_wipe_next(const mg_queue_t *q, uint32_t *sector_off);
void mg_queue_wipe_done(mg_queue_t *q, uint32_t sector_off, bool erased);
bool mg_queue_wipe_step(mg_queue_t *q);
bool mg_queue_wiping(const mg_queue_t *q);
/* Whether the sector at `sector_off` reads all erased (0xFF). */
bool mg_queue_sector_blank(const mg_queue_t *q, uint32_t sector_off);
