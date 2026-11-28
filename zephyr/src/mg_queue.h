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

#ifndef MG_QUEUE_H_
#define MG_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Offline clip queue on a flash partition: audio_queue_partition, or with
 * CONFIG_MG_QUEUE_IN_SLOT1 MCUboot's secondary slot (slot1_partition), which
 * it shares with firmware updates:
 *
 * - The queue owns the slot while MCUboot needs nothing in it. It never
 *   writes the last CONFIG_MG_QUEUE_SLOT1_TRAILER bytes (where MCUboot reads
 *   the slot's trailer), and offset 0, where MCUboot looks for an image
 *   header, only ever holds a clip header ("MGQ1") or blank cells, never the
 *   image magic.
 * - An upload's first chunk hands the slot to OTA (mg_queue_hand_to_ota):
 *   recording stops and the clips are dropped. It stays OTA's while an upload
 *   runs, an upgrade is pending, or the running image isn't confirmed yet
 *   (MCUboot may still swap back), see mg_queue_slot_needed_by_ota().
 * - Once MCUboot needs nothing there, mg_queue_reclaim() blanks the image
 *   header and the trailer (the whole slot on flash that needs erasing) and
 *   the queue starts empty.
 *
 * Clips are appended back to back. Each starts with a 32-byte header in two
 * 16-byte halves: the first (magic, codec, start time) is written when the
 * clip opens, the second (length, CRC-32, commit magic) when it closes.
 * On NOR flash both halves start erased and are written once; clear erases
 * the pages used, and a clip cut short by a reset ends at the first erased
 * block. On RRAM, which rewrites in place, the second half carries the
 * length so far (updated every few KB) until the clip closes, the header
 * slot after the last clip is kept blank, and clear only blanks the first
 * header, so it is instant; an owner change (mg_queue_forget) erases every
 * byte.
 */

struct mg_queue_status {
	uint32_t used_bytes;
	uint32_t capacity_bytes;
	uint16_t clip_count;
};

/* A clip's format (clip_info's trailing fields):
 * 0 in any field means "the codec's default" (clips written before the
 * format was stored: 16000 Hz, mono, SBC's own frames or LC3 40 B / 10 ms). */
struct mg_clip_info {
	uint8_t codec;
	uint32_t start_ms;
	uint32_t bytes;
	uint32_t offset; /* of the clip data in the partition */
	uint16_t rate;
	uint8_t channels;
	uint16_t frame_bytes;
	uint16_t frame_us;
	uint32_t unix_s; /* wall-clock start, unix seconds; 0: the clock wasn't set */
};

int mg_queue_init(void);
/* The partition exists. */
bool mg_queue_present(void);
/* ...and the queue owns it: recording and downloads work. */
bool mg_queue_ready(void);
/* Who holds the storage now: "clips", "OTA", "other data" or "none". */
const char *mg_queue_owner(void);

/* Slot sharing (CONFIG_MG_QUEUE_IN_SLOT1). Whether MCUboot or img_mgmt may
 * need the slot: an upload is running, an upgrade is pending (test,
 * permanent or revert), or the running image is unconfirmed. */
bool mg_queue_slot_needed_by_ota(bool upload_active, bool upgrade_pending, bool running_confirmed);
/* Hands the slot to OTA: stops recording, drops the clips (logged).
 * Returns the number of clips dropped. */
int mg_queue_hand_to_ota(void);
/* Takes the slot back: blanks what MCUboot reads, the queue starts empty. */
int mg_queue_reclaim(void);
void mg_queue_get_status(struct mg_queue_status *st);
int mg_queue_clip_info(uint16_t index, struct mg_clip_info *ci);
int mg_queue_read(uint16_t index, uint32_t off, void *buf, size_t len);

/* Recording. Only one clip is open at a time. The header's first half
 * keeps the format: [magic u32, codec u8, frame_bytes u16, channels u8,
 * start_ms u32, rate u16, frame_us u16]; mg_queue_begin() leaves the format
 * at the codec's default (zeros). */
int mg_queue_begin(uint8_t codec, uint32_t start_ms);
/* unix_s: the wall-clock start (0 unknown), kept in the second header half's
 * last word: [bytes u32, crc u32, magic u32, unix_s u32]. */
int mg_queue_begin_fmt(uint8_t codec, uint16_t rate, uint8_t channels, uint16_t frame_bytes,
		       uint16_t frame_us, uint32_t start_ms, uint32_t unix_s);
/* Appends whole codec frames. -ENOSPC once the partition is full: the clip
 * stays open and keeps what fit. */
int mg_queue_append(const void *data, size_t len);
int mg_queue_end(void);
bool mg_queue_recording(void);

int mg_queue_clear(void);
/* An owner change: every clip goes, the one being recorded included (its
 * writes fail from then on). The index is blanked at once (nothing is listed
 * or read after it), then a background thread erases the whole store, a step
 * at a time, skipping what is already blank, and calls done (on that thread)
 * with 0 once every byte is blank, or an error (-EAGAIN: handed to a
 * firmware update meanwhile; the erase is owed until the queue takes the slot
 * back). Recording and clear answer -EBUSY meanwhile. Returns 0 once the
 * erase started, -EAGAIN while OTA holds the slot or it holds an image (the
 * clips went at the hand-off; erased once taken back), -ENODEV before
 * mg_queue_init() or without the partition. */
int mg_queue_forget(void (*done)(int result));
/* The owner change's erase is running. */
bool mg_queue_wiping(void);

#endif
