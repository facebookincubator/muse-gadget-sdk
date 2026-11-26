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

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_queue.h"
#include "mg_setup.h"
#include "mg_setup_store.h"

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
#define QUEUE_PART PARTITION_ID(slot1_partition)
#define QTRAILER   CONFIG_MG_QUEUE_SLOT1_TRAILER
#else
#define QUEUE_PART PARTITION_ID(audio_queue_partition)
#define QTRAILER   0
#endif

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	fake_settle_wipe();
	zassert_equal(mg_queue_clear(), 0);
}

static void fill(uint8_t *b, size_t n, uint8_t seed)
{
	for (size_t i = 0; i < n; i++) {
		b[i] = (uint8_t)(seed + i * 7);
	}
}

ZTEST(queue, test_clips_roundtrip)
{
	static uint8_t a[1000], b[333], rd[1000];
	struct mg_clip_info ci;
	struct mg_queue_status st;

	fill(a, sizeof(a), 1);
	fill(b, sizeof(b), 99);

	/* Clip 0 appended in frame-sized pieces, clip 1 in one go. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 100), 0);
	zassert_true(mg_queue_recording());
	for (size_t off = 0; off < sizeof(a); off += 40) {
		zassert_equal(mg_queue_append(&a[off], MIN(40, sizeof(a) - off)), 0);
	}
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);
	zassert_equal(mg_queue_end(), 0);
	zassert_false(mg_queue_recording());
	/* Clip 1 keeps its format: LC3 16 kb/s. */
	zassert_equal(mg_queue_begin_fmt(mg_data_type_audio_lc3, 16000, 1, 20, 10000, 5000,
					 1793800000), 0);
	zassert_equal(mg_queue_append(b, sizeof(b)), 0);
	zassert_equal(mg_queue_end(), 0);

	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 2);
	zassert_true(st.used_bytes >= sizeof(a) + sizeof(b) + 64);

	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.codec, mg_data_type_audio_sbc);
	zassert_equal(ci.start_ms, 100);
	zassert_equal(ci.bytes, sizeof(a));
	zassert_true(ci.rate == 0 && ci.channels == 0 && ci.frame_bytes == 0 && ci.frame_us == 0 &&
		     ci.unix_s == 0,
		     "no format stored: the codec's default; no clock");
	zassert_equal(mg_queue_read(0, 0, rd, sizeof(a)), 0);
	zassert_mem_equal(rd, a, sizeof(a));
	zassert_equal(mg_queue_read(0, 990, rd, 10), 0);
	zassert_mem_equal(rd, &a[990], 10);
	zassert_equal(mg_queue_read(0, 995, rd, 10), -EINVAL, "past the end");

	zassert_equal(mg_queue_clip_info(1, &ci), 0);
	zassert_equal(ci.codec, mg_data_type_audio_lc3);
	zassert_equal(ci.start_ms, 5000);
	zassert_equal(ci.bytes, sizeof(b));
	zassert_true(ci.rate == 16000 && ci.channels == 1 && ci.frame_bytes == 20 &&
		     ci.frame_us == 10000 && ci.unix_s == 1793800000);
	zassert_equal(mg_queue_read(1, 0, rd, sizeof(b)), 0);
	zassert_mem_equal(rd, b, sizeof(b));

	/* Survives a reboot (re-scan). */
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 2);
	zassert_equal(mg_queue_read(1, 0, rd, sizeof(b)), 0);
	zassert_mem_equal(rd, b, sizeof(b));
	zassert_equal(mg_queue_clip_info(1, &ci), 0);
	zassert_true(ci.codec == mg_data_type_audio_lc3 && ci.rate == 16000 && ci.channels == 1 &&
		     ci.frame_bytes == 20 && ci.frame_us == 10000 && ci.unix_s == 1793800000,
		     "the format and wall-clock start survive a re-scan");

	/* Appending after a re-scan continues after the last clip. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 7), 0);
	zassert_equal(mg_queue_append(a, 60), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_clip_info(2, &ci), 0);
	zassert_equal(ci.bytes, 60);
	zassert_equal(mg_queue_read(0, 0, rd, sizeof(a)), 0);
	zassert_mem_equal(rd, a, sizeof(a), "earlier clip intact");
}

static bool rewritable(void)
{
	const struct flash_area *fa;

	zassert_equal(flash_area_open(QUEUE_PART, &fa), 0);
	return flash_get_parameters(flash_area_get_device(fa))->caps.no_explicit_erase;
}

ZTEST(queue, test_recovers_unfinished_clip)
{
	static uint8_t a[5000], rd[5000];
	struct mg_clip_info ci;
	struct mg_queue_status st;

	fill(a, sizeof(a), 3);
	/* A reset mid-recording: the clip never closes. On flash everything up
	 * to the first erased block survives (19 whole 256-byte buffers); on
	 * RRAM the length saved every 16 buffers does. */
	uint32_t want = rewritable() ? 16 * 256 : 19 * 256;

	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 42), 0);
	zassert_equal(mg_queue_append(a, sizeof(a)), 0);
	zassert_equal(mg_queue_init(), 0); /* "reboot" without mg_queue_end() */

	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 1);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.start_ms, 42);
	zassert_equal(ci.bytes, want, "recovered %u", ci.bytes);
	zassert_equal(mg_queue_read(0, 0, rd, ci.bytes), 0);
	zassert_mem_equal(rd, a, ci.bytes);

	/* The recovered length was committed: a second scan agrees, and the
	 * next clip goes after it. */
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, want);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 43), 0);
	zassert_equal(mg_queue_append(a, 100), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 2);

	/* A clip with a frame size (60-byte SBC frames, which don't divide the
	 * checkpointed length): only whole frames are recovered: 4080 of the
	 * RRAM checkpoint's 4096 bytes, 4860
	 * of flash's 4864. */
	uint32_t whole = (want / 60) * 60;

	zassert_equal(mg_queue_begin_fmt(mg_data_type_audio_sbc, 16000, 1, 60, 7500, 44, 0), 0);
	zassert_equal(mg_queue_append(a, 4980), 0); /* 83 frames */
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(mg_queue_clip_info(2, &ci), 0);
	zassert_equal(ci.bytes, whole, "recovered %u, want %u", ci.bytes, whole);
	zassert_equal(ci.bytes % 60, 0);
	zassert_equal(mg_queue_read(2, 0, rd, ci.bytes), 0);
	zassert_mem_equal(rd, a, ci.bytes);
	zassert_equal(mg_queue_init(), 0); /* the second scan agrees */
	zassert_equal(mg_queue_clip_info(2, &ci), 0);
	zassert_equal(ci.bytes, whole);
}

ZTEST(queue, test_clear_forgets_old_clips)
{
	static uint8_t a[3000];
	struct mg_queue_status st;
	struct mg_clip_info ci;

	fill(a, sizeof(a), 9);
	for (int i = 0; i < 3; i++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, i), 0);
		zassert_equal(mg_queue_append(a, sizeof(a)), 0);
		zassert_equal(mg_queue_end(), 0);
	}
	zassert_equal(mg_queue_clear(), 0);
	/* One short clip over the old ones: a rescan must not find the stale
	 * clips behind it. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_lc3, 99), 0);
	zassert_equal(mg_queue_append(a, 80), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 1);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, 80);
	zassert_equal(ci.codec, mg_data_type_audio_lc3);
	/* And an empty queue after clear stays empty across a reboot. */
	zassert_equal(mg_queue_clear(), 0);
	zassert_equal(mg_queue_init(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
}

ZTEST(queue, test_full)
{
	static uint8_t frame[240];
	struct mg_queue_status st;
	struct mg_clip_info ci;
	int err = 0;
	uint32_t appended = 0;

	fill(frame, sizeof(frame), 5);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 0), 0);
	while ((err = mg_queue_append(frame, sizeof(frame))) == 0) {
		appended += sizeof(frame);
	}
	zassert_equal(err, -ENOSPC);
	zassert_equal(mg_queue_append(frame, 1), -ENOSPC, "stays full");
	zassert_equal(mg_queue_end(), 0);
	mg_queue_get_status(&st);
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, appended, "whole frames only");
	zassert_true(st.used_bytes <= st.capacity_bytes);
	/* What's left is less than one more frame. */
	if (mg_queue_begin(mg_data_type_audio_sbc, 0) == 0) {
		zassert_equal(mg_queue_append(frame, sizeof(frame)), -ENOSPC);
		zassert_equal(mg_queue_end(), 0);
	}

	zassert_equal(mg_queue_clear(), 0);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 0), 0, "room again");
	zassert_equal(mg_queue_end(), 0);
}

ZTEST(queue, test_clear_while_recording)
{
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 0), 0);
	zassert_equal(mg_queue_clear(), -EBUSY);
	zassert_equal(mg_queue_end(), 0);
}

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)

/* MCUboot's image header magic, and what it reads at the end of a slot. */
#define IMAGE_MAGIC 0x96f3b83dU

static const struct flash_area *slot(void)
{
	const struct flash_area *fa;

	zassert_equal(flash_area_open(QUEUE_PART, &fa), 0);
	return fa;
}

static bool region_blank(const struct flash_area *fa, uint32_t off, uint32_t len)
{
	uint8_t b[256];
	uint8_t e = flash_area_erased_val(fa);

	for (uint32_t done = 0; done < len; done += sizeof(b)) {
		uint32_t n = MIN(sizeof(b), len - done);

		zassert_equal(flash_area_read(fa, off + done, b, n), 0);
		for (uint32_t i = 0; i < n; i++) {
			if (b[i] != e) {
				return false;
			}
		}
	}
	return true;
}

static uint32_t word_at(const struct flash_area *fa, uint32_t off)
{
	uint8_t b[4];

	zassert_equal(flash_area_read(fa, off, b, 4), 0);
	return sys_get_le32(b);
}

/* Filled to the brim, the queue never writes the image magic at offset 0
 * nor anything in the trailer MCUboot reads. */
ZTEST(queue, test_slot_never_looks_like_an_image)
{
	static uint8_t frame[240];
	const struct flash_area *fa = slot();
	struct mg_queue_status st;

	zassert_equal(mg_queue_reclaim(), 0);
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER));
	mg_queue_get_status(&st);
	zassert_equal(st.capacity_bytes, fa->fa_size - QTRAILER);
	fill(frame, sizeof(frame), 0x3d); /* bytes of the magic, for good measure */
	for (int clip = 0; clip < 3; clip++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, clip), 0);
		zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);
		while (mg_queue_append(frame, sizeof(frame)) == 0) {
		}
		zassert_equal(mg_queue_end(), 0);
		zassert_equal(word_at(fa, 0), 0x3151474dU, "a clip header (MGQ1) at 0");
	}
	mg_queue_get_status(&st);
	zassert_true(st.used_bytes > st.capacity_bytes - 2 * sizeof(frame));
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER), "trailer untouched");
	/* Clearing (instant on RRAM) leaves blank cells at 0, never the magic. */
	zassert_equal(mg_queue_clear(), 0);
	zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER));
	flash_area_close(fa);
}

/* The slot changes hands: clips -> OTA -> (reboot with an image) -> reclaimed. */
ZTEST(queue, test_slot_ownership)
{
	static uint8_t a[2000], img[512];
	const struct flash_area *fa = slot();
	struct mg_queue_status st;
	struct mg_clip_info ci;

	zassert_equal(mg_queue_reclaim(), 0);
	fill(a, sizeof(a), 7);
	for (int i = 0; i < 2; i++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, i), 0);
		zassert_equal(mg_queue_append(a, sizeof(a)), 0);
		zassert_equal(mg_queue_end(), 0);
	}
	/* A third clip is recording when the upload starts. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 9), 0);
	zassert_equal(mg_queue_append(a, 100), 0);
	zassert_equal(mg_queue_hand_to_ota(), 3, "two clips and the open one dropped");
	zassert_false(mg_queue_ready());
	zassert_true(mg_queue_present());
	zassert_false(mg_queue_recording());
	zassert_equal(strcmp(mg_queue_owner(), "OTA"), 0);
	zassert_equal(mg_queue_append(a, 100), -EINVAL, "recording stopped");
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);
	zassert_equal(mg_queue_clear(), -EBUSY);
	zassert_equal(mg_queue_clip_info(0, &ci), -ENOENT);
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count + st.used_bytes + st.capacity_bytes, 0, "shown as unavailable");
	zassert_equal(mg_queue_hand_to_ota(), 0, "nothing more to drop");

	/* img_mgmt writes an image, and MCUboot's pending-upgrade magic. */
	memset(img, 0xa5, sizeof(img));
	sys_put_le32(IMAGE_MAGIC, img);
	zassert_equal(flash_area_write(fa, 0, img, sizeof(img)), 0);
	memset(img, 0x77, 16);
	zassert_equal(flash_area_write(fa, fa->fa_size - 16, img, 16), 0);

	/* Reboot: the scan finds an image, not clips, and leaves it alone. */
	zassert_equal(mg_queue_init(), 0);
	zassert_false(mg_queue_ready());
	zassert_equal(strcmp(mg_queue_owner(), "other data"), 0);
	zassert_equal(word_at(fa, 0), IMAGE_MAGIC, "untouched while MCUboot may need it");
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);

	/* MCUboot is done (no swap pending, image confirmed): reclaim. */
	zassert_false(mg_queue_slot_needed_by_ota(false, false, true));
	zassert_equal(mg_queue_reclaim(), 0);
	zassert_true(mg_queue_ready());
	zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);
	zassert_true(region_blank(fa, fa->fa_size - QTRAILER, QTRAILER), "trailer blanked");
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
	zassert_equal(st.capacity_bytes, fa->fa_size - QTRAILER);

	/* Clips again, kept across a reboot; the old clips' headers deeper in
	 * the slot are never read as clips. */
	zassert_equal(mg_queue_begin(mg_data_type_audio_lc3, 77), 0);
	zassert_equal(mg_queue_append(a, 400), 0);
	zassert_equal(mg_queue_end(), 0);
	zassert_equal(mg_queue_init(), 0);
	zassert_true(mg_queue_ready());
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 1, "no stale clips from before the upload");
	zassert_equal(mg_queue_clip_info(0, &ci), 0);
	zassert_equal(ci.bytes, 400);
	zassert_equal(ci.start_ms, 77);
	flash_area_close(fa);
}

/* An upload that starts and is abandoned before writing anything: the
 * dropped clips stay dropped across a reboot (a clean, empty store). */
ZTEST(queue, test_dropped_clips_stay_dropped)
{
	static uint8_t a[500];
	const struct flash_area *fa = slot();
	struct mg_queue_status st;

	zassert_equal(mg_queue_reclaim(), 0);
	fill(a, sizeof(a), 3);
	for (int i = 0; i < 2; i++) {
		zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, i), 0);
		zassert_equal(mg_queue_append(a, sizeof(a)), 0);
		zassert_equal(mg_queue_end(), 0);
	}
	zassert_equal(mg_queue_hand_to_ota(), 2);
	zassert_not_equal(word_at(fa, 0), 0x3151474dU, "first header blanked at once");
	zassert_not_equal(word_at(fa, 0), IMAGE_MAGIC);

	/* Reboot with nothing written: blank at 0, so empty and ours again. */
	zassert_equal(mg_queue_init(), 0);
	zassert_true(mg_queue_ready());
	mg_queue_get_status(&st);
	zassert_equal(st.clip_count, 0);
	zassert_equal(st.used_bytes, 0);
	flash_area_close(fa);
}

#endif

static uint16_t clip_count(void)
{
	struct mg_queue_status st;

	mg_queue_get_status(&st);
	return st.clip_count;
}

static void record_clip(const uint8_t *a, size_t n)
{
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), 0);
	zassert_equal(mg_queue_append(a, n), 0);
	zassert_equal(mg_queue_end(), 0);
}

/* The owner change's erase, run to its end in the background. */
static void wait_erased(void)
{
	for (int i = 0; i < 10000 && (mg_queue_wiping() || mg_setup_store_clips_due()); i++) {
		k_sleep(K_MSEC(1));
	}
	zassert_false(mg_queue_wiping());
	zassert_false(mg_setup_store_clips_due(), "the mark goes once the store is blank");
}

/* Every byte of the store reads blank. */
static void assert_store_blank(void)
{
	const struct flash_area *fa;
	static uint8_t b[256];
	uint8_t blank = 0xff;

	zassert_ok(flash_area_open(QUEUE_PART, &fa));
	blank = flash_get_parameters(flash_area_get_device(fa))->erase_value;
	for (uint32_t off = 0; off + sizeof(b) <= fa->fa_size - QTRAILER; off += sizeof(b)) {
		zassert_ok(flash_area_read(fa, off, b, sizeof(b)));
		for (size_t i = 0; i < sizeof(b); i++) {
			zassert_equal(b[i], blank, "earlier owner's byte at 0x%x", off + i);
		}
	}
	flash_area_close(fa);
}

/* The offline clips go with their owner: a re-setup's commit carries the
 * mark, so a power loss before the erase still erases them at the next boot;
 * Forget marks them before it erases the setup, and takes a clip being
 * recorded too. The index goes at once and the whole store, old clips a
 * clear left behind included, is blank before the mark goes; the queue stays
 * closed until then. A first setup keeps what was recorded before it. */
ZTEST(queue, test_owner_change_erases_clips)
{
	static const uint8_t k[MG_SETUP_K_LEN] = {1};
	static uint8_t a[600];

	fill(a, sizeof(a), 3);
	(void)mg_setup_store_erase();
	zassert_ok(mg_setup_store_clips_gone());
	/* Audio a same-owner clear left on the flash, then two clips. */
	record_clip(a, sizeof(a));
	record_clip(a, sizeof(a));
	zassert_ok(mg_queue_clear());
	record_clip(a, sizeof(a));
	record_clip(a, 100);
	zassert_ok(mg_setup_store_commit("access-1", 8, "r1", 2, k));
	zassert_false(mg_setup_store_clips_due());
	zassert_equal(clip_count(), 2);

	/* Set up again, then a power loss before the clips were erased. */
	zassert_ok(mg_setup_store_commit("access-2", 8, "r2", 2, k));
	zassert_true(mg_setup_store_clips_due());
	zassert_ok(mg_setup_store_load());
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(clip_count(), 2);
	zassert_true(mg_setup_store_clips_due());
	mg_setup_clips_resume();
	zassert_equal(clip_count(), 0, "the index goes at once");
	zassert_true(mg_setup_store_clips_due(), "the mark stays until the store is blank");
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1), -EBUSY);
	wait_erased();
	assert_store_blank();
	zassert_ok(mg_setup_store_load());
	zassert_true(mg_setup_store_complete(), "the new set stays");
	zassert_false(mg_setup_store_clips_due(), "the mark is cleared");
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(clip_count(), 0);

	/* Forget, with a power loss right after the setup's erase, then one in
	 * the middle of the store's erase: the next boot starts it over. */
	record_clip(a, sizeof(a));
	record_clip(a, sizeof(a));
	zassert_ok(mg_setup_store_mark_clips());
	zassert_ok(mg_setup_store_erase());
	zassert_ok(mg_setup_store_load());
	zassert_false(mg_setup_store_complete());
	zassert_true(mg_setup_store_clips_due());
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(clip_count(), 2);
	mg_setup_clips_resume();
	zassert_equal(mg_queue_init(), 0, "a reset before the erase ran");
	zassert_ok(mg_setup_store_load());
	zassert_true(mg_setup_store_clips_due());
	mg_setup_clips_resume();
	wait_erased();
	assert_store_blank();
	zassert_ok(mg_setup_store_load());
	zassert_false(mg_setup_store_clips_due());

	/* Forget while a clip is being recorded: it goes too. */
	zassert_ok(mg_setup_store_commit("access-3", 8, "r3", 2, k));
	record_clip(a, sizeof(a));
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 2), 0);
	zassert_equal(mg_queue_append(a, sizeof(a)), 0);
	zassert_ok(mg_setup_factory_reset("test"));
	zassert_false(mg_queue_recording());
	zassert_equal(mg_queue_append(a, 10), -EINVAL);
	zassert_equal(clip_count(), 0);
	wait_erased();
	assert_store_blank();
	zassert_equal(mg_queue_init(), 0);
	zassert_equal(clip_count(), 0);
}

ZTEST_SUITE(queue, NULL, setup, before, NULL, NULL);
