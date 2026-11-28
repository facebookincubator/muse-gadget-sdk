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

/*
 * Memory for firmware updates (update support takes no base memory). MCUmgr's buffers and work queue
 * (src/zephyr/smp.c, the copy of Zephyr's) and the image writer's context
 * (img_mgmt_write_image_data(), wrapped here) come from the C library's
 * malloc arena, which on this board is all the RAM no static data uses
 * (CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1), only while SMP is in use:
 *
 * - a request or response buffer from when it is needed until it is sent;
 * - the work queue from the first request until no request or buffer has
 *   been about for CONFIG_MG_SMP_SESSION_IDLE_MS;
 * - the image writer from an upload's first chunk to its last (or an error),
 *   or to the end of the session for an upload that stops (a dropped link):
 *   the upload stays resumable, from what is on flash, with a fresh writer.
 *
 * The device session (mg_vm_session.c) shares the arena: about 30 KB from an
 * answered hello to the connection's end, so an update can overlap it. SMP
 * needs about 10 KB of it at most (the queue's 4 KB stack, four 512-byte
 * buffers, the 4 KB image writer): with a 64 KB arena, an upload still runs
 * with 54 KB of it held and fails cleanly (rc 2, nothing written) with 55 KB
 * held (tests/smp_link, Zephyr's malloc). On the XIAO the arena is about
 * 146 KB, so SMP, the BLE recovery tier, has more than ten times what it needs
 * with a session open.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include "mg_zephyr_version.h"

MG_ZEPHYR_WRAPS_CHECK(); /* the -Wl,--wrap functions below */

LOG_MODULE_DECLARE(mg_ota, CONFIG_MG_LOG_LEVEL);

/* SMP needs a request and its response out at once, and a request no
 * longer than an Android write; the arena that holds them is checked after
 * every link (tools/check_arena.py). */
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT >= 2, "SMP needs a request and a response buffer");
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE <= 512, "an SMP request fits one Android write");

static atomic_t blocks; /* out right now: what mg_smp_mem_blocks() reports */

void *mg_smp_mem_get(size_t align, size_t size)
{
	void *p = aligned_alloc(align < sizeof(void *) ? sizeof(void *) : align, size);

	if (p != NULL) {
		atomic_inc(&blocks);
	}
	return p;
}

void mg_smp_mem_put(void *block)
{
	if (block != NULL) {
		atomic_dec(&blocks);
		free(block);
	}
}

int mg_smp_mem_blocks(void)
{
	return (int)atomic_get(&blocks);
}

/*
 * The image writer (Zephyr's flash_img_context: a stream_flash context and
 * the image block buffer, CONFIG_IMG_BLOCK_BUF_SIZE): allocated at an
 * upload's first chunk, freed at its last (or an error), and released when
 * the SMP session ends mid-upload (no request for
 * CONFIG_MG_SMP_SESSION_IDLE_MS: a dropped link, a client that stopped). Then
 * the bytes still in its buffer are dropped and img_mgmt's offset moves back
 * to what is on flash (whole blocks, so aligned), which the client is told
 * when it resumes (off 0 with the same sha answers it; any other chunk is
 * answered with it) and resends from. A fresh writer picks up there.
 *
 * Only the SMP work queue writes (this wrap) and only the session's end
 * releases, after that queue has stopped: no lock.
 */
static struct flash_img_context *img_ctx;
static bool img_released; /* an upload lost its writer at a session's end */

/* From src/zephyr/smp.c: the session ended (its queue stopped, its lock held). */
void mg_smp_session_ended(void)
{
	size_t on_flash;

	if (img_ctx == NULL) {
		return;
	}
	on_flash = stream_flash_bytes_written(&img_ctx->stream);
	LOG_INF("firmware upload idle: writer freed (%zu buffered bytes dropped); it resumes at %zu",
		g_img_mgmt_state.off - on_flash, on_flash);
	mg_smp_mem_put(img_ctx);
	img_ctx = NULL;
	if (on_flash == 0) {
		/* Nothing on flash to resume: forget the upload (as
		 * img_mgmt_reset_upload() does), or img_mgmt would answer every
		 * off-0 retry with the same sha as a resume at 0 and write
		 * nothing. */
		memset(&g_img_mgmt_state, 0, sizeof(g_img_mgmt_state));
		g_img_mgmt_state.area_id = -1;
		img_released = false;
		return;
	}
	g_img_mgmt_state.off = on_flash;
	img_released = true;
}

/* The upload as it stands (src/mg_ota.c hashes it before taking a last
 * chunk): the bytes on slot1 (*on_flash) and those still in the writer's
 * buffer after them (returned, at *buf). With no writer (none yet, or
 * released) img_mgmt's offset is all on flash. On the SMP work queue only. */
size_t mg_smp_upload_buffered(const uint8_t **buf, size_t *on_flash)
{
	if (img_ctx == NULL) {
		*buf = NULL;
		*on_flash = g_img_mgmt_state.off;
		return 0;
	}
	*buf = img_ctx->stream.buf;
	*on_flash = stream_flash_bytes_written(&img_ctx->stream);
	return img_ctx->stream.buf_bytes;
}

/* A fresh writer continuing a released upload at off (bytes on flash). */
static int writer_resume(struct flash_img_context *ctx, size_t off)
{
	struct stream_flash_ctx *sf = &ctx->stream;

	if (flash_img_init_id(ctx, g_img_mgmt_state.area_id) != 0) {
		return IMG_MGMT_ERR_FLASH_OPEN_FAILED;
	}
	sf->bytes_written = off;
#if defined(CONFIG_STREAM_FLASH_ERASE)
	/* As stream_flash's own progress loader: the page holding the last byte
	 * written is erased already (never erase it again). */
	if (off > 0) {
		struct flash_pages_info page;

		if (flash_get_page_info_by_offs(sf->fdev, sf->offset + off - 1, &page) != 0) {
			return IMG_MGMT_ERR_FLASH_OPEN_FAILED;
		}
		sf->erased_up_to = page.start_offset + page.size - sf->offset;
	}
#endif
	return IMG_MGMT_ERR_OK;
}

/*
 * Zephyr's own, with the context allocated as in its
 * CONFIG_MCUMGR_GRP_IMG_USE_HEAP_FOR_FLASH_IMG_CONTEXT variant (which needs a
 * system heap), from mg_smp_mem_get() instead, and resumable after a release;
 * built with -Wl,--wrap=img_mgmt_write_image_data, so the static context of
 * the original is never linked. img_mgmt calls it with offset == its own
 * g_img_mgmt_state.off.
 */
int __wrap_img_mgmt_write_image_data(unsigned int offset, const void *data, unsigned int num_bytes,
				     bool last)
{
	int rc = IMG_MGMT_ERR_OK;
	bool resume = offset != 0 && img_ctx == NULL && img_released;

	if (offset != 0 && img_ctx == NULL && !resume) {
		return IMG_MGMT_ERR_FLASH_CONTEXT_NOT_SET;
	}
	if (offset == 0 || resume) {
		/* A new upload, or a released one going on. */
		mg_smp_mem_put(img_ctx);
		img_released = false;
		img_ctx = mg_smp_mem_get(__alignof__(struct flash_img_context), sizeof(*img_ctx));
		if (img_ctx == NULL) {
			return IMG_MGMT_ERR_NO_FREE_MEMORY;
		}
		if (resume) {
			rc = writer_resume(img_ctx, offset);
		} else if (flash_img_init_id(img_ctx, g_img_mgmt_state.area_id) != 0) {
			rc = IMG_MGMT_ERR_FLASH_OPEN_FAILED;
		}
		if (rc != IMG_MGMT_ERR_OK) {
			goto out;
		}
	}
	if (flash_img_buffered_write(img_ctx, data, num_bytes, last) != 0) {
		rc = IMG_MGMT_ERR_FLASH_WRITE_FAILED;
	}
out:
	if (last || rc != IMG_MGMT_ERR_OK) {
		mg_smp_mem_put(img_ctx);
		img_ctx = NULL;
	}
	return rc;
}
