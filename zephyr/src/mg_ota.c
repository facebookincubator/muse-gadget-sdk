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


#include "mg_health.h"
#include "mg_ota.h"

#include <string.h>

#include <bootutil/bootutil_public.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/storage/flash_map.h>
#if defined(CONFIG_MCUMGR_TRANSPORT_REASSEMBLY)
#include <mgmt/mcumgr/transport/smp_reassembly.h>
#endif

#if defined(MG_SMP_LINK_SIM)
#include <tinycrypt/constants.h>
#include <tinycrypt/sha256.h>
#else
#include <psa/crypto.h>
#endif

#include "mg_app.h"
#include "mg_boot.h"
#include "mg_button.h"
#include "mg_connparam.h"
#include "mg_led.h"
#include "mg_audio.h"
#include "mg_ota_policy.h"
#include "mg_reach_policy.h"
#include "mg_session.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
#include "mg_queue.h"
#include "mg_setup.h"
#endif
#include "mg_zephyr_version.h"

/* src/zephyr/smp.c change 4 (the request now running) and src/mg_smp_mem.c
 * (the upload as it stands). */
struct net_buf *mg_smp_request_buf(void);
size_t mg_smp_upload_buffered(const uint8_t **buf, size_t *on_flash);
#define MG_SMP_HDR_LEN 8 /* struct smp_hdr */

MG_ZEPHYR_WRAPS_CHECK(); /* the -Wl,--wrap functions below */

LOG_MODULE_REGISTER(mg_ota, CONFIG_MG_LOG_LEVEL);

static volatile bool upload_active;

/* mg_ota_policy.h: the image may keep itself (the boot reached its healthy
 * mark AND a link proved the gadget's own ATT path: never on advertising or a
 * timer alone); a client confirmed the running image (held until then). Set from the SMP thread, the BLE callbacks and the app work
 * queue. */
static atomic_t healthy;
static atomic_t boot_healthy;
static atomic_t link_proven;
static void healthy_maybe(void);
static atomic_t update_read; /* an SMP image list / state read answered this boot (the Keep signal) */
static bool list_read_pending; /* the command in hand is one (SMP thread only) */
static atomic_t client_confirmed;
static bool waiting_logged;

/* The carry-on: the allowed transfer's last chunk is in. */
static void mg_ota_xfer_staged(void);

/* The decision runs on the app work queue, whoever triggers it. */
static void eval_fn(struct k_work *w);
static K_WORK_DEFINE(eval_work, eval_fn);

static const char *swap_name(int t)
{
	switch (t) {
	case BOOT_SWAP_TYPE_NONE: return "none";
	case BOOT_SWAP_TYPE_TEST: return "test";
	case BOOT_SWAP_TYPE_PERM: return "permanent";
	case BOOT_SWAP_TYPE_REVERT: return "revert";
	default: return "unknown";
	}
}

bool mg_ota_confirmed(void)
{
	return boot_is_img_confirmed();
}

bool mg_ota_upload_active(void)
{
	return upload_active;
}

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
/* An upload began this boot and isn't abandoned (mg_ota_slot_held()). */
static atomic_t slot_held;

static void slot_evt(enum mg_ota_slot_evt e)
{
	atomic_set(&slot_held, mg_ota_slot_held(atomic_get(&slot_held), e));
}

static bool slot_needed(void)
{
	int swap = mcuboot_swap_type();

	return mg_queue_slot_needed_by_ota(upload_active || atomic_get(&slot_held),
					   swap != BOOT_SWAP_TYPE_NONE, boot_is_img_confirmed());
}

/* Recording or a clip download must not touch the slot any more. */
static void stop_audio_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	enum mg_audio_activity a = mg_audio_activity();

	if (a == MG_AUDIO_RECORDING || a == MG_AUDIO_CLIP) {
		mg_audio_stop();
	}
}

static K_WORK_DEFINE(stop_audio_work, stop_audio_fn);
#endif

static enum mgmt_cb_return on_img(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				  uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(prev);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(size);

	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		/* Runs before img_mgmt writes the first chunk. */
		mg_connparam_busy(MG_XFER_DFU);
		upload_active = true;
		LOG_INF("firmware upload started");
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
		slot_evt(MG_OTA_SLOT_STARTED);
		(void)mg_queue_hand_to_ota();
		k_work_submit_to_queue(mg_app_wq(), &stop_audio_work);
#endif
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		/* Stopped (it may resume); the slot stays OTA's until a reset
		 * or the client's erase. */
		upload_active = false;
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
		slot_evt(MG_OTA_SLOT_STOPPED);
#endif
		LOG_INF("firmware upload stopped");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		/* Its last chunk is in: the upload is over (img_mgmt sends no
		 * STOPPED on success). The slot stays OTA's: the state write
		 * that stages the image is still to come. */
		upload_active = false;
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
		slot_evt(MG_OTA_SLOT_PENDING);
#endif
		LOG_INF("firmware update pending: it installs at the next reset");
		mg_ota_xfer_staged();
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED:
		/* A client's confirm: written already (healthy), or latched. */
		LOG_INF("firmware image %s", boot_is_img_confirmed() ? "confirmed by the client"
								  : "confirm latched until the boot is healthy");
		k_work_submit_to_queue(mg_app_wq(), &eval_work);
		break;
	default:
		break;
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback img_cb = {
	.callback = on_img,
	.event_id = MGMT_EVT_OP_IMG_MGMT_ALL,
};

/*
 * Every SMP request, before its handler runs. An image group request (an
 * upload chunk, or the list) keeps the link listening on every connection
 * event: each one is an action that holds the link fast.
 */
BUILD_ASSERT(MG_SMP_OP_WRITE == MGMT_OP_WRITE && MG_SMP_GROUP_OS == MGMT_GROUP_ID_OS &&
		     MG_SMP_GROUP_IMAGE == MGMT_GROUP_ID_IMAGE && MG_SMP_OS_RESET == OS_MGMT_ID_RESET,
	     "mg_ota_policy.h names MCUmgr's ids");

static atomic_t smp_refused;

uint32_t mg_ota_smp_refused(void)
{
	return (uint32_t)atomic_get(&smp_refused);
}

/*
 * The carry-on (mg_ota_policy.h): the transfer an upload that passed the
 * check started, on the connection it started on. Only the SMP work queue
 * touches it (the command hook, the upload check and img_mgmt's status
 * events all run there).
 */
static atomic_t owner_epoch; /* bumped at each setup's commit */

static struct {
	uint32_t gen; /* mg_transport_conn_gen() at its first chunk; 0: none */
	atomic_val_t epoch; /* owner_epoch at its first chunk */
	size_t size;
	uint8_t sha[IMG_MGMT_DATA_SHA_LEN];
	size_t sha_len;
	bool staged; /* its last chunk is in (img_mgmt's DFU pending) */
	bool logged;
} xfer;

/* A transfer started before an ownership change carries nothing over it:
 * the rest needs the new owner's proof. */
static bool xfer_here(void)
{
	return xfer.gen != 0 && xfer.gen == mg_transport_conn_gen() &&
	       xfer.gen == mg_smp_request_gen() && xfer.epoch == atomic_get(&owner_epoch);
}

static void mg_ota_xfer_staged(void)
{
	if (xfer_here()) {
		xfer.staged = true;
	}
}

/* A release build's update rule (unproven writes ask for the press);
 * smp_link plays both with its --release switch. */
#if defined(MG_SMP_LINK_SIM)
extern bool sim_release;
#define OTA_RELEASE() sim_release
#else
#define OTA_RELEASE() mg_ota_release_build(IS_ENABLED(CONFIG_MG_AUTOMATION), IS_ENABLED(CONFIG_MG_BENCH_MODE))
#endif

/* The prompt and binding (mg_ota_policy.h, mg-update-v2.json),
 * under one lock: the SMP thread, the app work queue (the press) and the
 * timer. The wired console's arm (arm_gen) needs no press. */
static struct mg_upd upd;
static K_MUTEX_DEFINE(upd_lock);
static atomic_t arm_gen;
static int64_t arm_until;

static void prompt_end_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(prompt_work, prompt_end_fn);

void mg_ota_window_open(uint32_t conn_gen)
{
	arm_until = k_uptime_get() + MG_UPDATE_WINDOW_MS;
	atomic_set(&arm_gen, (atomic_val_t)conn_gen);
	LOG_INF("mg.ota: update armed for this connection for %d s (no press)",
		MG_UPDATE_WINDOW_MS / 1000);
}

static bool window_open(void)
{
	return mg_ota_window_open_now((uint32_t)atomic_get(&arm_gen), arm_until,
				      mg_transport_conn_gen(), k_uptime_get());
}

static int upd_rc(int rc)
{
	return rc == MG_UPD_OK ? MGMT_ERR_EOK
	       : rc == MG_UPD_DENIED ? MGMT_ERR_EACCESSDENIED
				     : MGMT_ERR_EBUSY;
}

/* An update write on a link that needs the press (mg_upd_write()): what
 * the policy answered (MG_UPD_VERIFY included), *why the reason of an
 * MG_UPD_DENIED. */
static int upd_write(uint32_t gen, const struct mg_upd_req *r, const char **why)
{
	bool started;

	k_mutex_lock(&upd_lock, K_FOREVER);
	int rc = mg_upd_write(&upd, gen, k_uptime_get(), r, &started);

	*why = upd.why;
	k_mutex_unlock(&upd_lock);
	if (started) {
		/* Presses queued before now don't answer it. */
		mg_button_flush_injected();
		mg_led_set(MG_LED_UPDATE_PROMPT, true);
		k_work_reschedule(&prompt_work, K_MSEC(MG_UPDATE_PROMPT_MS));
		LOG_WRN("mg.ota: firmware update asked without the token proof: press the button "
			"once within %d s to allow it", MG_UPDATE_PROMPT_MS / 1000);
	}
	return rc;
}

static void prompt_end_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	k_mutex_lock(&upd_lock, K_FOREVER);
	bool was = upd.prompting;

	mg_upd_expire(&upd, k_uptime_get());
	bool over = was && !upd.prompting;
	bool on = upd.prompting;

	k_mutex_unlock(&upd_lock);
	if (!on) {
		/* Whatever ended it (the time, its link, a setup): the light
		 * follows the state. */
		mg_led_set(MG_LED_UPDATE_PROMPT, false);
	}
	if (over) {
		LOG_INF("mg.ota: update prompt over without a press");
	}
}

/* Whether slot1 holds the image a press was bound to: the press binds to an
 * upload's CLAIMED length and whole-image SHA-256, so before a last chunk or
 * an admitted state write is taken the slot's bytes are hashed: the first
 * flash_len on slot1, then t1 (the image writer's buffer) and t2 (the chunk
 * about to be written), len bytes in all, must hash to want. */
static bool slot_is_bound_image(uint32_t flash_len, const uint8_t *t1, size_t n1,
				const uint8_t *t2, size_t n2, uint32_t len, const uint8_t want[32])
{
	const struct flash_area *fa;
	static uint8_t buf[256];
	const uint8_t *tail[2] = {t1, t2};
	size_t tail_len[2] = {n1, n2};
	uint8_t got[32];
	bool ok = false;

	if (flash_area_open(PARTITION_ID(slot1_partition), &fa) != 0) {
		return false;
	}
	if (len == 0 || (uint64_t)flash_len + n1 + n2 != len || len > fa->fa_size) {
		flash_area_close(fa);
		return false;
	}
#if defined(MG_SMP_LINK_SIM)
	struct tc_sha256_state_struct st;

	(void)tc_sha256_init(&st);
#define SLOT_HASH(p, n) ((void)tc_sha256_update(&st, (p), (n)), true)
#else
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;

	if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		flash_area_close(fa);
		return false;
	}
#define SLOT_HASH(p, n) (psa_hash_update(&op, (p), (n)) == PSA_SUCCESS)
#endif
	for (uint32_t off = 0; off < flash_len; off += sizeof(buf)) {
		size_t n = MIN(sizeof(buf), flash_len - off);

		if (flash_area_read(fa, off, buf, n) != 0 || !SLOT_HASH(buf, n)) {
			goto out;
		}
	}
	for (int i = 0; i < 2; i++) {
		if (tail_len[i] && !SLOT_HASH(tail[i], tail_len[i])) {
			goto out;
		}
	}
#undef SLOT_HASH
#if defined(MG_SMP_LINK_SIM)
	(void)tc_sha256_final(got, &st);
	ok = memcmp(got, want, 32) == 0;
#else
	size_t gl;

	ok = psa_hash_finish(&op, got, sizeof(got), &gl) == PSA_SUCCESS && gl == 32 &&
	     memcmp(got, want, 32) == 0;
#endif
out:
#if !defined(MG_SMP_LINK_SIM)
	psa_hash_abort(&op);
#endif
	flash_area_close(fa);
	return ok;
}

/* MG_UPD_VERIFY: the last chunk or the state write on the link that carries
 * the bound upload is taken only if slot1 then holds the bound image
 * (mg_upd_verified()). A chunk img_mgmt won't write (another offset than
 * its own: it answers its offset; a first chunk naming the upload it has
 * under way: it answers where that is) changes nothing, so it goes on
 * unchecked. On the SMP work queue. */
static int upd_verify(uint32_t gen, const struct mg_upd_req *r, const char **why)
{
	uint32_t blen;
	uint8_t bsha[32];
	bool matches;
	int rc;

	k_mutex_lock(&upd_lock, K_FOREVER);
	blen = upd.blen;
	memcpy(bsha, upd.bsha, sizeof(bsha));
	k_mutex_unlock(&upd_lock);
	if (r->kind == MG_UPD_STATE) {
		matches = slot_is_bound_image(blen, NULL, 0, NULL, 0, blen, bsha);
	} else if (r->kind == MG_UPD_UPLOAD0) {
		if (g_img_mgmt_state.area_id != -1 && g_img_mgmt_state.data_sha_len == 32 &&
		    memcmp(g_img_mgmt_state.data_sha, r->sha, 32) == 0) {
			return MG_UPD_OK; /* img_mgmt's resume answer: nothing written */
		}
		matches = slot_is_bound_image(0, NULL, 0, r->data, r->data_len, blen, bsha);
	} else {
		const uint8_t *tail;
		size_t on_flash, buffered;

		if (g_img_mgmt_state.area_id == -1 ||
		    r->end - r->data_len != g_img_mgmt_state.off) {
			return MG_UPD_OK; /* img_mgmt answers its own offset: nothing written */
		}
		buffered = mg_smp_upload_buffered(&tail, &on_flash);
		matches = on_flash + buffered == g_img_mgmt_state.off &&
			  slot_is_bound_image((uint32_t)on_flash, tail, buffered, r->data,
					      r->data_len, blen, bsha);
	}
	k_mutex_lock(&upd_lock, K_FOREVER);
	rc = mg_upd_verified(&upd, gen, matches);
	*why = upd.why;
	k_mutex_unlock(&upd_lock);
	return rc;
}

/* The link ended (mg_core.c): its prompt ends with it, and its light. */
void mg_ota_link_ended(void)
{
	k_mutex_lock(&upd_lock, K_FOREVER);
	mg_upd_link_ended(&upd);
	k_mutex_unlock(&upd_lock);
	(void)k_work_cancel_delayable(&prompt_work);
	mg_led_set(MG_LED_UPDATE_PROMPT, false);
}

bool mg_ota_prompt_take(bool real, bool injected_counts)
{
	k_mutex_lock(&upd_lock, K_FOREVER);
	bool took = mg_upd_press(&upd, mg_transport_conn_gen(), k_uptime_get(), real,
				 injected_counts);

	k_mutex_unlock(&upd_lock);
	if (!took) {
		return false;
	}
	(void)k_work_cancel_delayable(&prompt_work);
	mg_led_set(MG_LED_UPDATE_PROMPT, false);
	LOG_INF("mg.ota: update confirmed by a press: bound to the upload that asked, or covering "
		"the change that asked on this link, for %d s", MG_UPDATE_CONFIRM_MS / 1000);
	return true;
}

const char *mg_ota_prompt_state(void)
{
	uint32_t gen = mg_transport_conn_gen();
	int64_t now = k_uptime_get();

	k_mutex_lock(&upd_lock, K_FOREVER);
	const char *s = mg_upd_allowed(&upd, gen)          ? "admitted"
			: mg_upd_prompting(&upd, gen, now) ? "press"
							   : "idle";

	k_mutex_unlock(&upd_lock);
	return s;
}

static bool update_proof_ok(uint16_t group, uint8_t id, uint8_t op)
{
	return mg_ota_smp_allowed(group, id, op, mg_token_proof_matched(), mg_health_safe(),
				  mg_health_safe_by_button(), OTA_RELEASE(), window_open());
}

/* A SAFE the gadget entered by itself on a release build: update writes are
 * refused (rc 11), never asked (mg_ota_smp_closed()). */
static bool update_closed(uint16_t group, uint8_t id, uint8_t op)
{
	return mg_ota_smp_closed(group, id, op, mg_health_safe(), mg_health_safe_by_button(),
				 OTA_RELEASE(), window_open());
}

static void refuse_log(uint16_t group, uint8_t id, const char *why)
{
	atomic_inc(&smp_refused);
	LOG_WRN("mg.ota: SMP write (group %u id %u) refused: %s", group, id, why);
}

/*
 * Each upload request img_mgmt accepts, before it writes (MCUmgr's upload
 * check hook). An upload the command check allowed by the proof is recorded
 * at its first chunk; one it let through as the carry-on must be that
 * transfer: its size and sha, on its connection.
 */
#define TRIAL_STEP_MS 10000
static atomic_t last_chunk_s; /* uptime s of the last accepted upload chunk (0: none) */

/* Only an accepted chunk defers the trial's restart; a refused one doesn't. */
static atomic_t last_chunk_gen; /* the connection that chunk came on */
static void chunk_accepted(void)
{
	atomic_set(&last_chunk_s, (atomic_val_t)MAX(1, k_uptime_get() / 1000));
	atomic_set(&last_chunk_gen, (atomic_val_t)mg_transport_conn_gen());
}

/* The gadget's own update path is live on this connection (reachability
 * counts the link, mg_reach_link_counts()): it took an update chunk on it
 * within MG_REACH_UPDATE_MS, or a press admitted an update on it within
 * MG_UPDATE_CONFIRM_MS (mg_upd_admitted_live()); never the prompt alone
 * (mg_reach_update_live()). */
bool mg_ota_update_live(void)
{
	uint32_t gen = mg_transport_conn_gen();
	int64_t now = k_uptime_get();
	int64_t last = (int64_t)atomic_get(&last_chunk_s) * 1000;
	bool chunk_recent, prompting, admitted;

	if (gen == 0) {
		return false;
	}
	chunk_recent = (uint32_t)atomic_get(&last_chunk_gen) == gen && last &&
		       now - last < MG_REACH_UPDATE_MS;
	k_mutex_lock(&upd_lock, K_FOREVER);
	prompting = mg_upd_prompting(&upd, gen, now);
	admitted = mg_upd_admitted_live(&upd, gen, now);
	k_mutex_unlock(&upd_lock);
	return mg_reach_update_live(chunk_recent, prompting, admitted);
}

/* What on_cmd() decided for the upload request now running (only the SMP
 * work queue touches it): on_upload_check() takes an upload only as decided
 * there, where its whole request was read. */
static enum { UPD_NONE, UPD_PROVEN, UPD_CARRY, UPD_PRESS } upload_verdict;

static enum mgmt_cb_return on_upload_check(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
					   uint16_t *group, bool *abort_more, void *data,
					   size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	const struct img_mgmt_upload_check *chk = data;
	const struct img_mgmt_upload_req *req;

	if (size < sizeof(*chk) || chk->req == NULL) {
		*rc = MGMT_ERR_EACCESSDENIED; /* fails closed (unreachable with MCUmgr) */
		return MGMT_CB_ERROR_RC;
	}
	req = chk->req;
	/* The request's own connection (src/zephyr/smp.c change 3): the proof
	 * and the transfer it records belong to it, and it must still be the
	 * link, or it's refused. */
	uint32_t gen = mg_smp_request_gen();

	if (gen == 0 || gen != mg_transport_conn_gen()) {
		refuse_log(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, "its connection is gone");
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	if (req->off == 0) {
		xfer.staged = false; /* a new or repeated upload: nothing staged yet */
	}
	if (upload_verdict == UPD_PROVEN) {
		if (req->off == 0 || !xfer_here()) {
			/* Its first chunk, or a resume a newly proven connection
			 * took over: this connection's transfer. */
			const uint8_t *sha = req->off == 0 ? req->data_sha.value
							   : g_img_mgmt_state.data_sha;
			size_t n = MIN(req->off == 0 ? req->data_sha.len
						     : g_img_mgmt_state.data_sha_len,
				       sizeof(xfer.sha));

			xfer.gen = gen;
			xfer.epoch = atomic_get(&owner_epoch);
			xfer.size = req->off == 0 ? req->size : g_img_mgmt_state.size;
			memcpy(xfer.sha, sha, n);
			xfer.sha_len = n;
			xfer.logged = false;
		}
		chunk_accepted();
		return MGMT_CB_OK;
	}
	if ((upload_verdict == UPD_CARRY && xfer_here() &&
	     (req->off == 0
		      ? mg_ota_smp_same_transfer(xfer.size, xfer.sha, xfer.sha_len, req->size,
						 req->data_sha.value, req->data_sha.len)
		      : mg_ota_smp_same_transfer(xfer.size, xfer.sha, xfer.sha_len,
						 g_img_mgmt_state.size, g_img_mgmt_state.data_sha,
						 g_img_mgmt_state.data_sha_len))) ||
	    upload_verdict == UPD_PRESS) {
		chunk_accepted();
		return MGMT_CB_OK;
	}
	/* Nothing on_cmd() admitted (fails closed). */
	refuse_log(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, "not admitted by the command check");
	*rc = MGMT_ERR_EACCESSDENIED;
	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback upload_check_cb = {
	.callback = on_upload_check,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK,
};

/* The request now running, as src/zephyr/smp.c change 4 holds it: its
 * header (pulled just before its body) and body, if it's the command
 * MCUmgr announced. */
static bool request_bytes(const struct mgmt_evt_op_cmd_arg *cmd, const uint8_t **hdr,
			  const uint8_t **body, size_t *n)
{
	struct net_buf *nb = mg_smp_request_buf();
	const uint8_t *h;

	if (nb == NULL || net_buf_headroom(nb) < MG_SMP_HDR_LEN) {
		return false;
	}
	h = nb->data - MG_SMP_HDR_LEN;
	*n = (size_t)(h[2] << 8 | h[3]);
	if (*n > nb->len || (h[0] & 7) != cmd->op || (uint16_t)(h[4] << 8 | h[5]) != cmd->group ||
	    h[7] != cmd->id) {
		return false;
	}
	*hdr = h;
	*body = nb->data;
	return true;
}

/* An update write on a link without the proof, the wired arm or the button's
 * SAFE (a release build): the proof's carry-on, else the update prompt
 * (mg_ota_policy.h, struct mg_upd), decided on the request's own fields.
 * MGMT_ERR_EOK if taken, else what to answer. */
static int gated_write(uint32_t gen, const struct mgmt_evt_op_cmd_arg *cmd)
{
	bool upload = cmd->group == MGMT_GROUP_ID_IMAGE && cmd->id == MG_SMP_IMG_UPLOAD;
	const uint8_t *hdr, *body;
	uint8_t key[MG_UPD_KEY_MAX];
	struct mg_upd_req r;
	const char *why = NULL;
	size_t n;
	int w = request_bytes(cmd, &hdr, &body, &n) ? mg_upd_parse(hdr, body, n, &r, key) : -1;
	int rc;

	if (w < 0) {
		refuse_log(cmd->group, cmd->id, "an update write the gate can't read");
		return MGMT_ERR_EACCESSDENIED;
	}
	if (w == 0) {
		return MGMT_ERR_EOK; /* not an update write (unreachable: the proof check) */
	}
	/* The proof's carry-on: the rest of the transfer a proven upload
	 * started on this connection (mg_ota_smp_carries()). */
	if (upload ? xfer_here() && (r.kind == MG_UPD_UPLOAD0
					     ? r.sha != NULL &&
						       mg_ota_smp_same_transfer(xfer.size, xfer.sha,
										xfer.sha_len, r.len,
										r.sha, 32)
					     : mg_ota_smp_same_transfer(
						       xfer.size, xfer.sha, xfer.sha_len,
						       g_img_mgmt_state.size, g_img_mgmt_state.data_sha,
						       g_img_mgmt_state.data_sha_len))
		   : mg_ota_smp_carries(cmd->group, cmd->id, cmd->op, xfer_here(), xfer.staged)) {
		if (upload) {
			upload_verdict = UPD_CARRY;
		}
		if (!xfer.logged) {
			xfer.logged = true;
			LOG_INF("mg.ota: the proof lapsed; finishing the transfer this connection "
				"started with it");
		}
		return MGMT_ERR_EOK;
	}
	rc = upd_write(gen, &r, &why);
	if (rc == MG_UPD_VERIFY) {
		rc = upd_verify(gen, &r, &why);
	}
	if (rc == MG_UPD_OK) {
		if (upload) {
			upload_verdict = UPD_PRESS;
		}
		return MGMT_ERR_EOK;
	}
	refuse_log(cmd->group, cmd->id,
		   rc == MG_UPD_BUSY ? "no token proof: waiting for one button press"
		   : why != NULL     ? why
				     : "refused");
	return upd_rc(rc);
}

static enum mgmt_cb_return on_cmd(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				  uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev);
	ARG_UNUSED(group);
	const struct mgmt_evt_op_cmd_arg *cmd = data;

	ARG_UNUSED(abort_more);
	/* On a release build, update writes without this connection's token
	 * proof, the wired console's arm or the button's SAFE go through the
	 * update prompt (gated_write()), decided here on the request's own
	 * fields before img_mgmt or os_mgmt touches anything (an upload's
	 * offset query, a write past its length, its last chunk included).
	 * Bench and automation builds take them with no press. The proof's
	 * carry-on still finishes a transfer it started. */
	uint32_t gen = mg_smp_request_gen();

	upload_verdict = UPD_NONE;
	if (size < sizeof(*cmd)) {
		/* A short argument fails closed (unreachable with Zephyr's
		 * MCUmgr). */
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	bool ok = update_proof_ok(cmd->group, cmd->id, cmd->op);

	if (gen == 0 || gen != mg_transport_conn_gen()) {
		/* The request came on a connection that's gone, or the link
		 * changed while deciding: no proof of another connection counts
		 * for it. */
		refuse_log(cmd->group, cmd->id, "its connection is gone");
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	if (!ok && update_closed(cmd->group, cmd->id, cmd->op)) {
		refuse_log(cmd->group, cmd->id, "a SAFE the gadget entered itself has no way to prompt");
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	if (!ok) {
		int r = gated_write(gen, cmd);

		if (r != MGMT_ERR_EOK) {
			*rc = r;
			return MGMT_CB_ERROR_RC;
		}
	} else if (cmd->group == MGMT_GROUP_ID_IMAGE && cmd->id == MG_SMP_IMG_UPLOAD) {
		upload_verdict = UPD_PROVEN;
	}
	/* The Keep signal is taken when this read is answered (on_cmd_done),
	 * not when it arrives. */
	list_read_pending = cmd->group == MGMT_GROUP_ID_IMAGE && cmd->id == MG_SMP_IMG_STATE &&
			    cmd->op == MGMT_OP_READ;
	if (cmd->group == MGMT_GROUP_ID_IMAGE) {
		/* Uploads, and the image list a client reads just before one, so
		 * latency is already off when the first chunk arrives. */
		mg_connparam_busy(MG_XFER_DFU);
	} else {
		mg_connparam_busy(MG_XFER_DFU); /* any other SMP request: MCUmgr is activity */
	}
	/* A client sends more only once answered: TX progress. */
	mg_transport_rx_request();
	return MGMT_CB_OK;
}

static struct mgmt_callback cmd_cb = {
	.callback = on_cmd,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};

/* A command answered (smp.c: its handler succeeded and its response was
 * encoded): the Keep signal, a central's image list read answered by this
 * image on its own update path (the web updater's post-install check). */
static enum mgmt_cb_return on_cmd_done(uint32_t event, enum mgmt_cb_return prev_status,
				       int32_t *rc, uint16_t *group, bool *abort_more, void *data,
				       size_t size)
{
	const struct mgmt_evt_op_cmd_arg *d = data;

	ARG_UNUSED(event);
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	if (list_read_pending && size >= sizeof(*d) && d->group == MGMT_GROUP_ID_IMAGE &&
	    d->id == MG_SMP_IMG_STATE && d->err == MGMT_ERR_EOK && !atomic_set(&update_read, 1)) {
		LOG_INF("mg.ota: a central's image list read was answered: the update path works "
			"on this image");
		healthy_maybe();
	}
	list_read_pending = false;
	return MGMT_CB_OK;
}

static struct mgmt_callback cmd_done_cb = {
	.callback = on_cmd_done,
	.event_id = MGMT_EVT_OP_CMD_DONE,
};

/*
 * The SMP reset (an update's last step): os_mgmt answers, then reboots
 * CONFIG_MCUMGR_GRP_OS_RESET_MS later. In between the gadget stops media,
 * stops advertising and drops the link itself, so the phone sees
 * the disconnect at once instead of after the supervision timeout, and
 * can't reconnect to the image that is about to reboot.
 */
static void reset_quiesce_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	mg_audio_stop();
	mg_ble_hold_for_reset();
}

static K_WORK_DELAYABLE_DEFINE(reset_quiesce_work, reset_quiesce_fn);

/* The WDT keeps running across this soft reset, so its window must
 * cover the bootloader's copy and the new image's boot up to its early
 * watchdog start: fed last, just before the restart, so all of it is left. */
static void reset_feed_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	mg_health_feed_now();
}

static K_WORK_DELAYABLE_DEFINE(reset_feed_work, reset_feed_fn);
BUILD_ASSERT(CONFIG_MCUMGR_GRP_OS_RESET_MS > 30, "the last feed goes 30 ms before the reset");
BUILD_ASSERT(CONFIG_MG_DFU_RESET_DISCONNECT_MS < CONFIG_MCUMGR_GRP_OS_RESET_MS,
	     "drop the link before the SMP reset restarts the gadget");

static enum mgmt_cb_return on_reset(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				    uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	LOG_INF("SMP reset: dropping the link, restarting in %d ms",
		CONFIG_MCUMGR_GRP_OS_RESET_MS);
	/* After the answer has gone out (one or two connection events). */
	k_work_reschedule_for_queue(mg_app_wq(), &reset_quiesce_work,
				    K_MSEC(CONFIG_MG_DFU_RESET_DISCONNECT_MS));
	k_work_reschedule(&reset_feed_work, K_MSEC(CONFIG_MCUMGR_GRP_OS_RESET_MS - 30));
	return MGMT_CB_OK;
}

static struct mgmt_callback reset_cb = {
	.callback = on_reset,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

/*
 * img_mgmt's image state write ends in boot_set_next(): the only place it
 * changes the next boot (built with -Wl,--wrap=boot_set_next). A client's
 * confirm of the running image before the boot is healthy is answered OK
 * and applied at the healthy mark; everything else passes straight through
 * (the token proof gate is on_cmd's, before this). The app's
 * own confirm (boot_write_img_confirmed() in eval_fn) comes here too, from
 * the app work queue: SMP requests run on MCUmgr's own work queue.
 */
int __real_boot_set_next(const struct flash_area *fa, bool active, bool confirm);

#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
/* An image erase hands slot1 to the update only once img_mgmt has accepted
 * it (-Wl,--wrap=img_mgmt_erase_slot: called after its in-use check), so an
 * erase it refuses (the active slot, a slot in use) never drops the clips.
 * The clips go to OTA before the erase, as at an upload's start, so the
 * queue never writes over erased flash with a stale index. Runs on MCUmgr's
 * thread. */
int __real_img_mgmt_erase_slot(int slot);

int __wrap_img_mgmt_erase_slot(int slot)
{
	int err;

	if (slot == 1) {
		LOG_INF("image erase: the clip store goes to OTA");
		(void)mg_queue_hand_to_ota();
		k_work_submit_to_queue(mg_app_wq(), &stop_audio_work);
	}
	err = __real_img_mgmt_erase_slot(slot);
	if (slot == 1 && err == 0) {
		slot_evt(MG_OTA_SLOT_ERASED); /* any upload abandoned: nothing left to stage */
	}
	return err;
}

#endif

/* Forget (mg_setup_store.c): an ownership change, so a pending update
 * prompt ends and nothing it admitted, bound or covered carries over, nor
 * what the link's proof started; with no device token the next unproven
 * update write still asks for the press (mg-update-v2's "after Forget, the
 * press still", "Forget ends the binding", "Forget ends the link's proof"). */
void mg_setup_store_erased(void)
{
	/* The link's proof ended with the token (mg_setup_factory_reset()), and
	 * so does the transfer it started (the carry-on, xfer_here()). */
	atomic_inc(&owner_epoch);
	k_mutex_lock(&upd_lock, K_FOREVER);
	mg_upd_forget(&upd);
	k_mutex_unlock(&upd_lock);
	(void)k_work_cancel_delayable(&prompt_work);
	mg_led_set(MG_LED_UPDATE_PROMPT, false);
}

/* A setup committed (mg_setup_store_commit_device_key(); never a token
 * refresh): an image staged in slot1 for the next boot (test or permanent)
 * came before this owner, when anyone could upload; it's dropped on every
 * board, so it never installs on the owner's gadget. An image the running
 * one may revert to (the running image on test) stays. An upload still
 * running carries nothing over the change: its next chunk or state write
 * needs the new owner's proof (owner_epoch), so it can't be staged unasked
 * either. */
void mg_ota_setup_committed(void)
{
	int swap = mcuboot_swap_type();
	int err;

	atomic_inc(&owner_epoch);
	/* A press admission or binding from before the ownership change
	 * carries nothing over it: the next
	 * unproven update write prompts again. */
	k_mutex_lock(&upd_lock, K_FOREVER);
	mg_upd_forget(&upd);
	k_mutex_unlock(&upd_lock);
	(void)k_work_cancel_delayable(&prompt_work);
	mg_led_set(MG_LED_UPDATE_PROMPT, false);
	if (upload_active || !boot_is_img_confirmed() ||
	    (swap != BOOT_SWAP_TYPE_TEST && swap != BOOT_SWAP_TYPE_PERM)) {
		return;
	}
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
	err = mg_queue_reclaim();
#else
	err = boot_erase_img_bank(PARTITION_ID(slot1_partition));
#endif
	if (err) {
		LOG_ERR("mg.ota: an image staged before this setup could not be dropped (%d)", err);
	} else {
		LOG_WRN("mg.ota: an image staged before this setup is dropped (it won't install)");
	}
}

int __wrap_boot_set_next(const struct flash_area *fa, bool active, bool confirm)
{
	if (k_current_get() == &mg_app_wq()->thread) {
		return __real_boot_set_next(fa, active, confirm);
	}
	switch (mg_ota_set_next(atomic_get(&healthy), active, confirm, mg_health_safe())) {
	case MG_OTA_SET_NEXT_REFUSE:
		LOG_WRN("mg.ota: a client's confirm of the running image refused in SAFE mode");
		return -EPERM;
	case MG_OTA_SET_NEXT_LATCH:
		atomic_set(&client_confirmed, 1);
		/* The healthy mark may have come since: decide again there. */
		k_work_submit_to_queue(mg_app_wq(), &eval_work);
		return 0;
	default:
		break;
	}
	if (active && confirm) {
		atomic_set(&client_confirmed, 1);
	}
	return __real_boot_set_next(fa, active, confirm);
}

#if defined(CONFIG_MCUMGR_TRANSPORT_REASSEMBLY)
/*
 * Built with -Wl,--wrap=smp_reassembly_expected,--wrap=smp_reassembly_collect
 * (smp_bt asks the first before it collects each write, on the Bluetooth RX
 * thread, the only caller of both):
 * - the request's source: the connection its first fragment came on is
 *   recorded, a partly received request whose connection isn't the link any
 *   more is dropped (mg_smp_rx_orphan: a central that never subscribed
 *   leaves its partial request behind, GATT's cleanup skips it), and
 *   src/zephyr/smp.c's change 3 stamps each request with it;
 * - the gap check (mg_ota_policy.h, mg_smp_rx_stale): a write that comes
 *   more than CONFIG_MG_SMP_RX_GAP_MS after the previous one drops a partly
 *   received request first, so a client's retry after a lost write is read as
 *   a new request instead of the rest of the old one.
 * The drops happen in smp_reassembly_expected(), not in collect: smp_bt
 * attaches the connection only to a request it saw start, so dropped any
 * later, the first request after it would have no connection to answer on.
 */
static int64_t smp_rx_last_ms;
static uint32_t smp_rx_first_gen; /* the partial request's first fragment's connection */

int __real_smp_reassembly_expected(const struct smp_transport *smpt);
int __real_smp_reassembly_collect(struct smp_transport *smpt, const void *buf, uint16_t len);

int __wrap_smp_reassembly_expected(const struct smp_transport *smpt)
{
	int expected = __real_smp_reassembly_expected(smpt);
	int64_t now = k_uptime_get();

	if (mg_smp_rx_orphan(expected, smp_rx_first_gen, mg_transport_conn_gen())) {
		LOG_WRN("SMP: dropped a partly received request from a connection that's gone");
		(void)smp_reassembly_drop((struct smp_transport *)smpt);
		return -EINVAL;
	}
#if CONFIG_MG_SMP_RX_GAP_MS > 0
	if (mg_smp_rx_stale(now, smp_rx_last_ms, expected, CONFIG_MG_SMP_RX_GAP_MS)) {
		LOG_WRN("SMP: dropped a partly received request (%d bytes never came, %lld ms ago)",
			expected, (long long)(now - smp_rx_last_ms));
		(void)smp_reassembly_drop((struct smp_transport *)smpt);
		return -EINVAL;
	}
#else
	ARG_UNUSED(now);
#endif
	return expected;
}

int __wrap_smp_reassembly_collect(struct smp_transport *smpt, const void *buf, uint16_t len)
{
	if (__real_smp_reassembly_expected(smpt) < 0) {
		smp_rx_first_gen = mg_transport_conn_gen(); /* a first fragment */
	}
	smp_rx_last_ms = k_uptime_get();
	mg_transport_peer_wrote(); /* every fragment: SAFE's idle-link drop */
	return __real_smp_reassembly_collect(smpt, buf, len);
}

/* src/zephyr/smp.c change 3: a completed request's source is its first
 * fragment's. */
uint32_t mg_smp_rx_source_gen(void)
{
	return smp_rx_first_gen;
}
#endif

/* The trial window (mg_ota_policy.h): scheduled at boot for an image on
 * test, cancelled when it's confirmed. */
static void trial_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(trial_work, trial_fn);

static int64_t trial_counted_ms;

static enum mg_ota_swap swap_now(void)
{
	switch (mcuboot_swap_type()) {
	case BOOT_SWAP_TYPE_NONE: return MG_OTA_SWAP_NONE;
	case BOOT_SWAP_TYPE_TEST: return MG_OTA_SWAP_TEST;
	case BOOT_SWAP_TYPE_PERM: return MG_OTA_SWAP_PERM;
	case BOOT_SWAP_TYPE_REVERT: return MG_OTA_SWAP_REVERT;
	default: return MG_OTA_SWAP_OTHER;
	}
}

static void trial_fn(struct k_work *w)
{
	int64_t now = k_uptime_get();
	/* A link up unproven now or at the last tick, or any connection or
	 * disconnection since the last tick, pauses this tick (one sample a tick
	 * would miss a link that came and went in between, or one that held the
	 * start of the interval and dropped just before the tick). */
	static bool prev_unproven;
	bool seen = mg_transport_link_seen_take(MG_LINK_SEEN_TRIAL);
	bool up = mg_transport_link_up();
	bool proven = up && mg_token_proof_matched();
	bool unproven_seen = mg_ota_trial_unproven(seen, up, proven, prev_unproven);
	enum mg_ota_trial_act a;

	prev_unproven = up && !proven;

	ARG_UNUSED(w);
	a = mg_ota_trial_tick(&trial_counted_ms, TRIAL_STEP_MS, now, boot_is_img_confirmed(),
			      swap_now(), unproven_seen, (int64_t)atomic_get(&last_chunk_s) * 1000);
	if (a == MG_OTA_TRIAL_DONE) {
		LOG_INF("trial: over (confirmed, or a newer image is staged)");
		return;
	}
	if (a == MG_OTA_TRIAL_GO_ON) {
		k_work_reschedule_for_queue(mg_app_wq(), &trial_work, K_MSEC(TRIAL_STEP_MS));
		return;
	}
	LOG_ERR("image %s still on test (%lld min counted, %lld min up): restarting, so MCUboot "
		"reverts it", mg_app_version(), (long long)(trial_counted_ms / 60000),
		(long long)(now / 60000));
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_WARM);
}

void mg_ota_init(void)
{
	int swap = mcuboot_swap_type();
	bool confirmed = boot_is_img_confirmed();

	if (!confirmed) {
		mg_health_peer_bound_clear("a new image"); /* the owner installed it */
	}

	LOG_INF("image %s, %s; MCUboot swap: %s", mg_app_version(),
		confirmed ? "confirmed" : "on test", swap_name(swap));
	if (!confirmed && !mg_health_safe()) {
		k_work_schedule_for_queue(mg_app_wq(), &trial_work, K_MSEC(TRIAL_STEP_MS));
	}
	mgmt_callback_register(&img_cb);
	mgmt_callback_register(&cmd_cb);
	mgmt_callback_register(&cmd_done_cb);
	mgmt_callback_register(&upload_check_cb);
	mgmt_callback_register(&reset_cb);
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
	if (slot_needed()) {
		/* An image MCUboot may install or swap back sits in the slot. */
		(void)mg_queue_hand_to_ota();
	} else if (!mg_queue_ready()) {
		/* An old image, an abandoned upload or older data: MCUboot is done with it. */
		(void)mg_queue_reclaim();
	}
#endif
}

static void eval_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	bool confirmed = boot_is_img_confirmed();
	bool asked = atomic_get(&client_confirmed);

	if (!atomic_get(&healthy) || mg_health_safe()) {
		return;
	}
	if (mg_ota_should_confirm(true, false, confirmed, asked,
				  IS_ENABLED(CONFIG_MG_BENCH_NO_CONFIRM))) {
		int err = boot_write_img_confirmed();

		LOG_INF("image %s confirmed (%s; %d): it stays after the next reset",
			mg_app_version(), asked ? "healthy, and the client confirmed it" : "healthy",
			err);
		confirmed = err == 0;
		if (confirmed) {
			(void)k_work_cancel_delayable(&trial_work);
		}
	} else if (!confirmed && !waiting_logged) {
		/* Bench build for the revert test: MCUboot swaps the previous
		 * image back at the next reset. */
		waiting_logged = true;
		LOG_WRN("image %s left unconfirmed (CONFIG_MG_BENCH_NO_CONFIRM): the next reset "
			"reverts to the previous image unless a client confirms it",
			mg_app_version());
	}
#if defined(CONFIG_MG_QUEUE_IN_SLOT1)
	if (confirmed && !mg_queue_ready() && !slot_needed() && mg_queue_reclaim() == 0) {
		mg_setup_clips_resume(); /* an owner change's erase still owed */
	}
#endif
}

static void healthy_maybe(void)
{
	if (mg_ota_confirm_ready(atomic_get(&boot_healthy), atomic_get(&link_proven),
				 atomic_get(&update_read)) &&
	    !atomic_set(&healthy, 1)) {
		/* A test image keeps itself now (normal mode; eval_fn), and a
		 * client's earlier confirm takes effect. */
		k_work_submit_to_queue(mg_app_wq(), &eval_work);
	}
}

void mg_ota_healthy(void)
{
	atomic_set(&boot_healthy, 1);
	if (!atomic_get(&link_proven) && !atomic_get(&update_read)) {
		LOG_INF("mg.ota: healthy; the image keeps itself after a link proves ATT works "
			"or a central reads its image list");
	}
	healthy_maybe();
}

void mg_ota_link_proven(void)
{
	atomic_set(&link_proven, 1);
	healthy_maybe();
}
