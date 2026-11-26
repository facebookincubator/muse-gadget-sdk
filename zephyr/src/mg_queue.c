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

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mg_queue, CONFIG_MG_LOG_LEVEL);

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
/* MCUboot's secondary slot, shared with OTA (mg_queue.h). */
#define QUEUE_ID PARTITION_ID(slot1_partition)
#define TRAILER  CONFIG_MG_QUEUE_SLOT1_TRAILER
#else
#define QUEUE_ID PARTITION_ID(audio_queue_partition)
#define TRAILER  0
#endif

#define MAGIC_OPEN   0x3151474dU /* "MGQ1" */
#define MAGIC_COMMIT 0x4351474dU /* "MGQC" */
#define MAGIC_PARTIAL 0x5051474dU /* "MGQP": length so far (overwritable media) */
#define PARTIAL_EVERY 16 /* write-buffer flushes between partial length updates */
#define HALF         16U
#define HDR          (2 * HALF)
#define WBUF         256U

struct clip {
	uint32_t hdr; /* offset of the header */
	uint32_t bytes;
	uint32_t start_ms;
	uint8_t codec;
	uint8_t channels;
	uint16_t frame_bytes;
	uint16_t rate;
	uint16_t frame_us;
	uint32_t unix_s;
};

static const struct flash_area *fa;
static uint32_t cap;     /* bytes the queue may use: the partition less the slot trailer */
static bool ota_owned;   /* OTA has the slot: the queue is unavailable */
static bool foreign;     /* the slot holds something else (an image) until reclaimed */
static K_MUTEX_DEFINE(lock);
static struct clip clips[CONFIG_MG_AUDIO_QUEUE_MAX_CLIPS];
static uint16_t nclips;
static uint32_t tail;  /* next free, aligned offset */
static uint32_t align; /* write alignment, at least HALF */
static uint8_t erased;
/* RRAM and the like: rewritable in place, no erase needed. */
static bool overwrite;

/* An owner change's erase of the whole store, in the background. */
#define WIPE_STEP 1024U
static bool wiping;
static uint32_t wipe_pos;
static void (*wipe_done)(int result);
static K_SEM_DEFINE(wipe_sem, 0, 1);

/* Recording state. */
static bool rec;
static bool rec_full;
static int rec_err; /* a write of this clip failed: it is never committed */
static struct clip cur;
static uint32_t cur_crc;
static uint8_t wbuf[WBUF];
static size_t wlen;
static uint32_t wpos; /* where wbuf goes */
static uint32_t flushes;

static bool all_erased(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (b[i] != erased) {
			return false;
		}
	}
	return true;
}

static int write_half2(uint32_t hdr, uint32_t bytes, uint32_t crc, uint32_t magic, uint32_t unix_s)
{
	uint8_t h[HALF];

	memset(h, 0, sizeof(h));
	sys_put_le32(bytes, &h[0]);
	sys_put_le32(crc, &h[4]);
	sys_put_le32(magic, &h[8]);
	sys_put_le32(unix_s, &h[12]);
	return flash_area_write(fa, hdr + HALF, h, sizeof(h));
}

/* On overwritable media nothing past the last clip is erased, so the
 * header slot after it is blanked to end the scan there. */
static void blank_next_header(uint32_t off)
{
	if (overwrite && off + HDR <= cap) {
		(void)flash_area_flatten(fa, off, HDR);
	}
}

/* A clip whose second half never got written: its data runs up to the first
 * fully erased alignment block. */
static uint32_t recover_length(uint32_t data)
{
	uint8_t b[64];
	uint32_t off = data;
	size_t step = MIN((size_t)align, sizeof(b));
	while (off + step <= cap) {
		if (flash_area_read(fa, off, b, step) || all_erased(b, step)) {
			break;
		}
		off += step;
	}
	return off - data;
}

static int scan(void)
{
	uint8_t h[HDR];
	uint32_t off = 0;

	nclips = 0;
	while (off + HDR <= cap) {
		if (flash_area_read(fa, off, h, sizeof(h))) {
			return -EIO;
		}
		if (all_erased(h, HALF)) {
			break;
		}
		if (sys_get_le32(&h[0]) != MAGIC_OPEN) {
			LOG_WRN("queue: bad header at 0x%x, ignoring the rest", off);
			break;
		}
		struct clip c = {
			.hdr = off,
			.codec = h[4],
			.frame_bytes = sys_get_le16(&h[5]),
			.channels = h[7],
			.start_ms = sys_get_le32(&h[8]),
			.rate = sys_get_le16(&h[12]),
			.frame_us = sys_get_le16(&h[14]),
		};

		uint32_t magic = sys_get_le32(&h[HALF + 8]);

		if (magic == MAGIC_COMMIT || magic == MAGIC_PARTIAL) {
			c.unix_s = sys_get_le32(&h[HALF + 12]);
		}
		if (magic == MAGIC_COMMIT) {
			c.bytes = sys_get_le32(&h[HALF]);
		} else if (magic == MAGIC_PARTIAL || all_erased(&h[HALF], HALF)) {
			/* Interrupted by a reset: the length saved so far on RRAM, or
			 * up to the first erased block on flash. */
			c.bytes = magic == MAGIC_PARTIAL ? sys_get_le32(&h[HALF])
							 : recover_length(off + HDR);
			if (c.frame_bytes) {
				/* Whole frames only: a reset can cut the last one. */
				c.bytes -= c.bytes % c.frame_bytes;
			}
			if (c.bytes > cap - off - HDR) {
				/* Bounded before anything is written from it. */
				LOG_WRN("queue: clip at 0x%x too long, ignoring the rest", off);
				break;
			}
			LOG_WRN("queue: recovered unfinished clip at 0x%x (%u bytes)", off, c.bytes);
			(void)write_half2(off, c.bytes, 0, MAGIC_COMMIT, c.unix_s);
			blank_next_header(ROUND_UP(off + HDR + c.bytes, align));
		} else {
			LOG_WRN("queue: bad header at 0x%x, ignoring the rest", off);
			break;
		}
		if (c.bytes > cap - off - HDR) {
			LOG_WRN("queue: clip at 0x%x too long, ignoring the rest", off);
			break;
		}
		if (nclips == ARRAY_SIZE(clips)) {
			break;
		}
		clips[nclips++] = c;
		off = ROUND_UP(off + HDR + c.bytes, align);
	}
	tail = off;
	return 0;
}

int mg_queue_init(void)
{
	int err = flash_area_open(QUEUE_ID, &fa);

	if (err) {
		LOG_ERR("queue: no partition (%d)", err);
		fa = NULL;
		return err;
	}
	const struct device *dev = flash_area_get_device(fa);
	const struct flash_parameters *fp = flash_get_parameters(dev);

	erased = fp->erase_value;
	overwrite = fp->caps.no_explicit_erase;
	cap = fa->fa_size > TRAILER + 4 * HDR ? fa->fa_size - TRAILER : 0;
	if (!cap) {
		LOG_ERR("queue: partition too small");
		fa = NULL;
		return -ENOSPC;
	}
	align = MAX(HALF, flash_get_write_block_size(dev));
	if (align > HALF * 4 || (WBUF % align) != 0) {
		LOG_ERR("queue: write block %u unsupported", align);
		fa = NULL;
		return -ENOTSUP;
	}
	k_mutex_lock(&lock, K_FOREVER);
	rec = false;
	ota_owned = false;
	foreign = false;
	wiping = false;
	err = scan();
	if (IS_ENABLED(CONFIG_MG_QUEUE_IN_SLOT1) && nclips == 0) {
		/* Not a queue and not blank: an image, or what was there before. */
		uint8_t h[HALF];

		foreign = flash_area_read(fa, 0, h, sizeof(h)) != 0 ||
			  (!all_erased(h, sizeof(h)) && sys_get_le32(&h[0]) != MAGIC_OPEN);
	}
	k_mutex_unlock(&lock);
	if (foreign) {
		LOG_INF("queue: the slot holds something else; it waits to be reclaimed");
	} else {
		LOG_INF("queue: %u clips, %u of %u bytes used", nclips, tail, cap);
	}
	return err;
}

bool mg_queue_present(void)
{
	return fa != NULL;
}

bool mg_queue_ready(void)
{
	return fa != NULL && !ota_owned && !foreign;
}

const char *mg_queue_owner(void)
{
	return !fa ? "none" : ota_owned ? "OTA" : foreign ? "other data" : "clips";
}

bool mg_queue_slot_needed_by_ota(bool upload_active, bool upgrade_pending, bool running_confirmed)
{
	return upload_active || upgrade_pending || !running_confirmed;
}

int mg_queue_hand_to_ota(void)
{
	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	uint32_t clips_lost = nclips + (rec ? 1 : 0);
	uint32_t bytes_lost = rec ? cur.hdr + HDR + cur.bytes : tail;

	if (!ota_owned && clips_lost) {
		LOG_WRN("queue: firmware update takes the clip store: %u clip(s), %u bytes "
			"dropped (download clips before updating)",
			clips_lost, bytes_lost);
	} else if (!ota_owned) {
		LOG_INF("queue: firmware update takes the clip store (it was empty)");
	}
	/* Dropped means gone: blank the first header now, so a reset before the
	 * upload writes anything can't bring the dropped clips back, and the
	 * index never mixes them with an image (the apps
	 * download clips before an upload). */
	if (!ota_owned && clips_lost) {
		(void)flash_area_flatten(fa, 0, overwrite ? ROUND_UP(HDR, align) : fa->fa_size);
	}
	rec = false;
	nclips = 0;
	tail = 0;
	ota_owned = true;
	foreign = false;
	k_mutex_unlock(&lock);
	return (int)clips_lost;
}

int mg_queue_reclaim(void)
{
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (overwrite) {
		/* RRAM rewrites in place, so only what MCUboot reads needs blanking:
		 * the image header at the start and the trailer at the end. The old
		 * image's bytes in between are inert and get overwritten by clips. */
		err = flash_area_flatten(fa, 0, ROUND_UP(HDR, align));
		if (!err && fa->fa_size > cap) {
			err = flash_area_flatten(fa, cap, fa->fa_size - cap);
		}
	} else {
		/* Flash needs erased cells under every write: the whole slot. */
		err = flash_area_flatten(fa, 0, fa->fa_size);
	}
	if (!err) {
		nclips = 0;
		tail = 0;
		rec = false;
		ota_owned = false;
		foreign = false;
	}
	k_mutex_unlock(&lock);
	LOG_INF("queue: clip store reclaimed from firmware update (%d), %u bytes free", err, cap);
	return err;
}

void mg_queue_get_status(struct mg_queue_status *st)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool ok = fa && !ota_owned && !foreign;

	/* Held by a firmware update: nothing stored, no room. */
	st->used_bytes = ok ? tail : 0;
	st->capacity_bytes = ok ? cap : 0;
	st->clip_count = nclips;
	k_mutex_unlock(&lock);
}

int mg_queue_clip_info(uint16_t index, struct mg_clip_info *ci)
{
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (index >= nclips) {
		err = -ENOENT;
	} else {
		ci->codec = clips[index].codec;
		ci->start_ms = clips[index].start_ms;
		ci->bytes = clips[index].bytes;
		ci->offset = clips[index].hdr + HDR;
		ci->rate = clips[index].rate;
		ci->channels = clips[index].channels;
		ci->frame_bytes = clips[index].frame_bytes;
		ci->frame_us = clips[index].frame_us;
		ci->unix_s = clips[index].unix_s;
	}
	k_mutex_unlock(&lock);
	return err;
}

int mg_queue_read(uint16_t index, uint32_t off, void *buf, size_t len)
{
	struct mg_clip_info ci;
	int err = mg_queue_clip_info(index, &ci);

	if (err) {
		return err;
	}
	if (off > ci.bytes || len > ci.bytes - off) {
		return -EINVAL;
	}
	return flash_area_read(fa, ci.offset + off, buf, len);
}

bool mg_queue_recording(void)
{
	return rec;
}

int mg_queue_begin(uint8_t codec, uint32_t start_ms)
{
	return mg_queue_begin_fmt(codec, 0, 0, 0, 0, start_ms, 0);
}

int mg_queue_begin_fmt(uint8_t codec, uint16_t rate, uint8_t channels, uint16_t frame_bytes,
		       uint16_t frame_us, uint32_t start_ms, uint32_t unix_s)
{
	uint8_t h[HALF];
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (rec || ota_owned || foreign || wiping) {
		err = -EBUSY;
		goto out;
	}
	if (nclips == ARRAY_SIZE(clips) || tail + HDR + align > cap) {
		err = -ENOSPC;
		goto out;
	}
	memset(h, 0, sizeof(h));
	sys_put_le32(MAGIC_OPEN, &h[0]);
	h[4] = codec;
	sys_put_le16(frame_bytes, &h[5]);
	h[7] = channels;
	sys_put_le32(start_ms, &h[8]);
	sys_put_le16(rate, &h[12]);
	sys_put_le16(frame_us, &h[14]);
	/* On RRAM the second half holds whatever was there (an earlier owner's
	 * clip included): mark it partial before the first half makes the
	 * header valid, so a reset between the two can't leave a valid header
	 * whose length is rebuilt from old bytes. */
	err = overwrite ? write_half2(tail, 0, 0, MAGIC_PARTIAL, unix_s) : 0;
	if (!err) {
		err = flash_area_write(fa, tail, h, sizeof(h));
	}
	if (err) {
		goto out;
	}
	cur = (struct clip){.hdr = tail, .codec = codec, .start_ms = start_ms, .rate = rate,
			    .channels = channels, .frame_bytes = frame_bytes, .frame_us = frame_us,
			    .unix_s = unix_s};
	cur_crc = 0;
	wlen = 0;
	wpos = tail + HDR;
	flushes = 0;
	rec = true;
	rec_full = false;
	rec_err = 0;
out:
	k_mutex_unlock(&lock);
	return err;
}

static int flush(bool final)
{
	size_t n = final ? ROUND_UP(wlen, align) : wlen;

	if (n == 0) {
		return 0;
	}
	memset(&wbuf[wlen], erased, n - wlen);
	int err = flash_area_write(fa, wpos, wbuf, n);

	wpos += n;
	wlen = 0;
	/* On RRAM, keep the length saved so a reset loses at most a moment. */
	if (!err && !final && overwrite && ++flushes % PARTIAL_EVERY == 0) {
		err = write_half2(cur.hdr, wpos - cur.hdr - HDR, 0, MAGIC_PARTIAL, cur.unix_s);
	}
	return err;
}

int mg_queue_append(const void *data, size_t len)
{
	const uint8_t *p = data;
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (!rec) {
		err = -EINVAL;
		goto out;
	}
	if (rec_err) {
		err = rec_err;
		goto out;
	}
	/* Whole frames only: a frame that doesn't fit ends the clip's growth. */
	if (rec_full || cur.hdr + HDR + cur.bytes + len > cap) {
		rec_full = true;
		err = -ENOSPC;
		goto out;
	}
	cur_crc = crc32_ieee_update(cur_crc, p, len);
	cur.bytes += len;
	while (len) {
		size_t n = MIN(len, WBUF - wlen);

		memcpy(&wbuf[wlen], p, n);
		wlen += n;
		p += n;
		len -= n;
		if (wlen == WBUF) {
			err = flush(false);
			if (err) {
				LOG_WRN("queue: write failed (%d): the clip won't be kept", err);
				rec_err = err;
				goto out;
			}
		}
	}
out:
	k_mutex_unlock(&lock);
	return err;
}

int mg_queue_end(void)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	if (!rec) {
		k_mutex_unlock(&lock);
		return -EINVAL;
	}
	err = rec_err ?: flush(true);
	if (!err) {
		err = write_half2(cur.hdr, cur.bytes, cur_crc, MAGIC_COMMIT, cur.unix_s);
	}
	if (!err) {
		clips[nclips++] = cur;
		tail = ROUND_UP(cur.hdr + HDR + cur.bytes, align);
		blank_next_header(tail);
		LOG_INF("queue: clip %u saved, %u bytes", nclips - 1, cur.bytes);
	} else {
		/* Not kept: the next clip takes its place, and the next boot
		 * doesn't recover it (on media that can be overwritten). */
		LOG_WRN("queue: clip not saved (%d)", err);
		blank_next_header(cur.hdr);
	}
	rec = false;
	k_mutex_unlock(&lock);
	return err;
}

/* Empties the queue: everything up to used (the bytes clips may occupy)
 * reads as blank from offset 0 on. Called with the lock held. */
static int blank_used(uint32_t used)
{
	uint32_t end = HDR;

	if (!overwrite) {
		/* Flash: erase the pages that were used, so every later write
		 * lands on erased cells. */
		const struct device *dev = flash_area_get_device(fa);
		struct flash_pages_info pi;

		end = used;
		if (flash_get_page_info_by_offs(dev, fa->fa_off + MAX(end, 1) - 1, &pi) == 0) {
			end = pi.start_offset + pi.size - fa->fa_off;
		}
		end = MIN(ROUND_UP(MAX(end, HDR), align), cap);
	}
	/* RRAM rewrites in place: blanking the first header empties the queue
	 * at once (erasing 700 KB would take minutes while the radio runs). */
	int err = flash_area_flatten(fa, 0, end);

	if (!err) {
		nclips = 0;
		tail = 0;
	}
	return err;
}

int mg_queue_clear(void)
{
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (rec || ota_owned || foreign || wiping) {
		err = -EBUSY;
		goto out;
	}
	err = blank_used(tail);
	LOG_INF("queue: cleared (%d)", err);
out:
	k_mutex_unlock(&lock);
	return err;
}

int mg_queue_forget(void (*done)(int result))
{
	int err;

	if (!fa) {
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (ota_owned || foreign) {
		/* Not the queue's now: erased once it is taken back. */
		err = -EAGAIN;
		goto out;
	}
	uint32_t n = nclips + (rec ? 1 : 0);

	/* The index first (the first header, or page on flash): nothing is
	 * listed or read from here on; the rest goes in the background. */
	err = blank_used(HDR);
	if (err) {
		LOG_WRN("queue: owner change: index not erased (%d)", err);
		goto out;
	}
	rec = false;
	wiping = true;
	wipe_pos = 0;
	wipe_done = done;
	LOG_INF("queue: owner change: %u clip(s) gone, erasing the store", n);
	k_sem_give(&wipe_sem);
out:
	k_mutex_unlock(&lock);
	return err;
}

bool mg_queue_wiping(void)
{
	return wiping;
}

/* One step of the erase. Returns false when it ended, with *err its result. */
static bool wipe_step(int *err)
{
	static uint8_t b[64];
	uint32_t step = WIPE_STEP;

	if (!wiping) {
		*err = -ECANCELED;
		return false;
	}
	if (ota_owned || foreign) {
		/* Handed to a firmware update meanwhile: resumed once taken back. */
		wiping = false;
		*err = -EAGAIN;
		return false;
	}
	if (!overwrite) {
		struct flash_pages_info pi;

		if (flash_get_page_info_by_offs(flash_area_get_device(fa), fa->fa_off + wipe_pos,
						&pi) == 0) {
			step = pi.start_offset + pi.size - fa->fa_off - wipe_pos;
		}
	}
	step = MIN(step, cap - wipe_pos);
	/* Only what isn't blank yet is written. */
	bool dirty = false;

	for (uint32_t off = 0; off < step && !dirty; off += sizeof(b)) {
		size_t n = MIN(sizeof(b), step - off);

		*err = flash_area_read(fa, wipe_pos + off, b, n);
		dirty = *err != 0 || !all_erased(b, n);
	}
	*err = dirty ? flash_area_flatten(fa, wipe_pos, step) : 0;
	if (*err) {
		wiping = false;
		return false;
	}
	wipe_pos += step;
	if (wipe_pos >= cap) {
		wiping = false;
		return false;
	}
	return true;
}

static void wipe_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&wipe_sem, K_FOREVER);
		bool more = true;
		int err = 0;
		void (*done)(int result) = NULL;

		while (more) {
			/* One step at a time under the lock (a hand-off to OTA waits
			 * for it), then the other threads run. */
			k_mutex_lock(&lock, K_FOREVER);
			more = wipe_step(&err);
			done = wipe_done;
			k_mutex_unlock(&lock);
			if (more) {
				k_sleep(K_MSEC(1));
			}
		}
		if (err == -ECANCELED) {
			continue; /* the queue started over (mg_queue_init) */
		}
		LOG_INF("queue: store erased (%d)", err);
		if (done) {
			done(err);
		}
	}
}

K_THREAD_DEFINE(mg_queue_wipe, 1024, wipe_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
