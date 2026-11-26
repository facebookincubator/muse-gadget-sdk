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

#ifndef MG_OTA_POLICY_H_
#define MG_OTA_POLICY_H_

#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Who may update and when a test image is kept, as pure decisions so the
 * host tests can run them (mg_ota.c applies them):
 *
 * - An update write from a proven central, the button's SAFE, the wired
 *   console's arm or any bench or automation build is taken; any other
 *   answers 10 and starts the update prompt (below). SMP is the XIAO's BLE
 *   recovery tier: uploads, erases, image state writes and resets run in
 *   every mode, healthy or not.
 * - One exception, the timing of a confirm: a client's confirm of the
 *   running image before the boot is healthy (mg_health.c: BLE up and
 *   advertising for CONFIG_MG_HEALTHY_AFTER_MS with its threads alive) is
 *   accepted at once and takes effect at the healthy mark.
 * - A test image keeps itself at the healthy mark (board-ready included)
 *   once a proven link's ATT worked or a central's image list read was
 *   answered this boot (mg_ota_confirm_ready(), the Keep signal): an image
 *   that never gets there is swapped back by MCUboot.
 */

enum mg_ota_set_next {
	MG_OTA_SET_NEXT_PASS,   /* write the boot state now, as asked */
	MG_OTA_SET_NEXT_LATCH,  /* answer OK, apply at the healthy mark */
	MG_OTA_SET_NEXT_REFUSE, /* SAFE mode: a confirm of the running image is refused */
};

/*
 * Which SAFE is open: only one the user asked for with the button (the boot
 * hold); a SAFE the gadget entered by itself refuses update writes on a
 * release build, set up or not (mg_ota_smp_closed(): it has no way to
 * prompt). The hardware tier (SWD through the board's USB probe) is
 * unchanged.
 */
#define MG_SMP_OP_WRITE    2 /* MGMT_OP_WRITE */
#define MG_SMP_GROUP_OS    0 /* MGMT_GROUP_ID_OS */
#define MG_SMP_GROUP_IMAGE 1 /* MGMT_GROUP_ID_IMAGE */
#define MG_SMP_OS_RESET    5 /* OS_MGMT_ID_RESET */

/*
 * The update prompt (mg-update-v2.json): a web updater updates a gadget no
 * phone is connected to. On a release build, set up or not (a gadget with no
 * device token, not set up or after Forget, asks the same), an update write
 * from a central without this connection's proof (normal or the button's
 * SAFE; its own SAFE refuses,
 * mg_ota_smp_closed()) is answered MGMT_ERR_EBUSY at once and starts the
 * prompt (the LED's "update confirm" on screenless boards) for
 * MG_UPDATE_PROMPT_MS, on that connection only, at most once per
 * MG_UPDATE_REARM_MS per connection; ONE physical press of any
 * key (the talk button, or a board's update key) while the LED shows that
 * cue and no setup confirm is pending confirms the upload that started it
 * and binds the press to that upload's length and whole-image SHA-256 for
 * MG_UPDATE_CONFIRM_MS across link drops (struct mg_upd): an upload that
 * names no SHA-256 gets 11 and no prompt, a write past the bound length gets
 * 11, and the last chunk and the state write are taken only if slot1 then
 * hashes to it (mg_ota.c), each ending the binding when refused. A press on
 * a prompt an erase, the reset or a state write started covers only that
 * request on its link. Forget ends the binding, the covered change and the
 * link's proof. Injected
 * input (a console or Nordic UART word, a scripted press) never counts on a
 * release build; the wired console and the Nordic UART's `update window`
 * arm the connection with no press. A proven central (the owner's app), the
 * button's SAFE and bench or automation builds take update writes with no
 * press.
 * Reads of confidential things keep their own gates.
 */
#define MG_UPDATE_PROMPT_MS 30000
#define MG_UPDATE_REARM_MS  10000 /* a new prompt at most this often per connection */
#define MG_UPDATE_WINDOW_MS 60000

/* Whether a press answers the update prompt: the prompt is up on the
 * connection the press came on, and the press counts (real, or an injected
 * one where the build takes it). */
static inline bool mg_ota_prompt_press(uint32_t prompt_gen, int64_t prompt_until,
				       uint32_t conn_gen, int64_t now_ms, bool real,
				       bool injected_counts)
{
	return prompt_gen != 0 && prompt_gen == conn_gen && now_ms < prompt_until &&
	       (real || injected_counts);
}

/* The prompt as protocols/test-vectors/mg-update-v2.json and mg_update_ref.py
 * have it. On a build that needs the press (a release, on a link
 * without the proof, the wired console's arm or the button's SAFE), an update
 * write (image upload, state write, erase, OS reset):
 *  - an upload's first chunk that names no whole-image SHA-256, and a later
 *    chunk with no upload started on its link, get 11 and no prompt;
 *  - on the link that carries the bound upload (the press's link, or one
 *    that started that upload again, the same length and SHA-256, within
 *    MG_UPDATE_CONFIRM_MS of the press): its chunks, its state write, an
 *    erase and the reset are taken; a write past the bound length gets 11
 *    and ends the binding; the last chunk and the state write are taken
 *    only if the slot then holds the bound image (MG_UPD_VERIFY,
 *    mg_upd_verified()), else 11 and the binding ends; its first chunk
 *    again after MG_UPDATE_CONFIRM_MS, or another upload, asks again (a
 *    transfer under way still finishes);
 *  - the erase, reset or state write a press covered (the same group, id
 *    and body), on its link within MG_UPDATE_CONFIRM_MS, is taken;
 *  - any other: 11 once after the prompt ended unanswered, else 10 (the
 *    first starts the prompt, at most once per MG_UPDATE_REARM_MS per
 *    connection).
 * A press during the prompt binds an upload's prompt to that upload's length
 * and SHA-256 across link drops, or covers the change that started it on its
 * link only. A new link drops this link's part in the upload, the covered
 * change and the prompt, never the binding; Forget drops them all. */
#define MG_UPD_OK     0
#define MG_UPD_BUSY   10 /* MGMT_ERR_EBUSY: press the button */
#define MG_UPD_DENIED 11 /* MGMT_ERR_EACCESSDENIED */
#define MG_UPD_VERIFY (-1) /* taken if the slot then holds the bound image */
#define MG_UPDATE_CONFIRM_MS 600000
/* A change's group, id and body (the request without its seq) a press can
 * cover; a longer one is never covered and asks again. */
#define MG_UPD_KEY_MAX 96

enum mg_upd_kind {
	MG_UPD_UPLOAD0, /* an upload's first chunk: offset 0, naming its length */
	MG_UPD_CHUNK,   /* any other upload write */
	MG_UPD_STATE,   /* an image state write */
	MG_UPD_OTHER,   /* an erase, the OS reset */
};

/* One update write, as the gate reads it from the request. */
struct mg_upd_req {
	enum mg_upd_kind kind;
	uint32_t len;       /* MG_UPD_UPLOAD0: the image length it names */
	const uint8_t *sha; /* MG_UPD_UPLOAD0: its whole-image SHA-256 (NULL: none, or not 32 bytes) */
	uint32_t end;       /* uploads: its offset plus its data's length */
	const uint8_t *data; /* uploads: its data (mg_ota.c hashes a last chunk's) */
	uint32_t data_len;
	const uint8_t *key; /* MG_UPD_STATE, MG_UPD_OTHER: its group, id and body */
	size_t key_len;
};

/* One CBOR item's head (major type and value) at b[*i], as an SMP body
 * carries it (definite lengths only). */
static inline bool mg_upd_cbor_head(const uint8_t *b, size_t n, size_t *i, uint8_t *major,
				    uint64_t *val)
{
	uint8_t info;
	size_t sz;

	if (*i >= n) {
		return false;
	}
	*major = b[*i] >> 5;
	info = b[*i] & 31;
	(*i)++;
	if (info < 24) {
		*val = info;
		return true;
	}
	if (info > 27) {
		return false;
	}
	sz = (size_t)1 << (info - 24);
	if (n - *i < sz) {
		return false;
	}
	*val = 0;
	while (sz--) {
		*val = *val << 8 | b[(*i)++];
	}
	return true;
}

/* An SMP request read as the update gate reads it, before img_mgmt or
 * os_mgmt acts on it: hdr its 8-byte header, body its CBOR (a map of text
 * keys to unsigned integers, byte or text strings and booleans, as MCUmgr
 * clients send). 1: an update write (an image upload, state write or erase,
 * the OS reset), *r filled (key, the change's group, id and body, in keybuf
 * when it fits MG_UPD_KEY_MAX); 0: not one; -1: an update write the gate
 * can't read (refused). */
static inline int mg_upd_parse(const uint8_t *hdr, const uint8_t *body, size_t n,
			       struct mg_upd_req *r, uint8_t keybuf[MG_UPD_KEY_MAX])
{
	uint16_t group = (uint16_t)(hdr[4] << 8 | hdr[5]);
	uint8_t id = hdr[7];
	bool upload = group == MG_SMP_GROUP_IMAGE && id == 1;
	uint64_t off = 0, len = 0, count, v;
	const uint8_t *sha = NULL, *data = NULL;
	size_t sha_len = 0, data_len = 0, i = 0;
	bool has_off = false, has_len = false;
	uint8_t major;

	memset(r, 0, sizeof(*r));
	if ((hdr[0] & 7) != MG_SMP_OP_WRITE ||
	    !(group == MG_SMP_GROUP_IMAGE ? id == 0 || id == 1 || id == 5
					  : group == MG_SMP_GROUP_OS && id == MG_SMP_OS_RESET)) {
		return 0;
	}
	if (!upload) {
		r->kind = group == MG_SMP_GROUP_IMAGE && id == 0 ? MG_UPD_STATE : MG_UPD_OTHER;
		if (3 + n <= MG_UPD_KEY_MAX) {
			keybuf[0] = hdr[4];
			keybuf[1] = hdr[5];
			keybuf[2] = id;
			memcpy(&keybuf[3], body, n);
			r->key = keybuf;
			r->key_len = 3 + n;
		}
		return 1;
	}
	/* The map: definite, or indefinite up to its break (MCUmgr clients
	 * send either). */
	bool indefinite = n > 0 && body[0] == 0xbf;

	if (indefinite) {
		i = 1;
		count = UINT64_MAX;
	} else if (!mg_upd_cbor_head(body, n, &i, &major, &count) || major != 5) {
		return -1;
	}
	while (count--) {
		const uint8_t *k;
		uint64_t klen;

		if (indefinite && i < n && body[i] == 0xff) {
			i++;
			break;
		}

		if (!mg_upd_cbor_head(body, n, &i, &major, &klen) || major != 3 || n - i < klen) {
			return -1;
		}
		k = &body[i];
		i += (size_t)klen;
		if (!mg_upd_cbor_head(body, n, &i, &major, &v)) {
			return -1;
		}
		if (major == 2 || major == 3) {
			if (n - i < v) {
				return -1;
			}
			i += (size_t)v;
		} else if (major == 7 ? v != 20 && v != 21 : major != 0) {
			return -1;
		}
#define MG_UPD_KEY_IS(s) (klen == sizeof(s) - 1 && memcmp(k, s, sizeof(s) - 1) == 0)
		if (MG_UPD_KEY_IS("off") || MG_UPD_KEY_IS("len")) {
			bool *seen = MG_UPD_KEY_IS("off") ? &has_off : &has_len;

			if (major != 0 || *seen || v > UINT32_MAX) {
				return -1;
			}
			*seen = true;
			*(seen == &has_off ? &off : &len) = v;
		} else if (MG_UPD_KEY_IS("sha") || MG_UPD_KEY_IS("data")) {
			bool is_sha = MG_UPD_KEY_IS("sha");

			if (major != 2 || (is_sha ? sha : data) != NULL) {
				return -1;
			}
			if (is_sha) {
				sha = &body[i - (size_t)v];
				sha_len = (size_t)v;
			} else {
				data = &body[i - (size_t)v];
				data_len = (size_t)v;
			}
		}
#undef MG_UPD_KEY_IS
	}
	if (off + data_len > UINT32_MAX) {
		return -1;
	}
	r->kind = has_off && off == 0 && has_len ? MG_UPD_UPLOAD0 : MG_UPD_CHUNK;
	r->len = (uint32_t)len;
	r->sha = r->kind == MG_UPD_UPLOAD0 && sha_len == 32 ? sha : NULL;
	r->end = (uint32_t)(off + data_len);
	r->data = data;
	r->data_len = (uint32_t)data_len;
	return 1;
}

struct mg_upd {
	uint32_t gen; /* the connection the link's fields below belong to */
	/* The link's. */
	bool here; /* this link carries the bound upload */
	bool refuse_next, prompting;
	bool shown; /* a prompt showed on this connection (at prompt_at) */
	int64_t prompt_at;
	bool covered; /* a press covered ckey on this link (at covered_at) */
	int64_t covered_at;
	uint16_t ckey_len;
	uint8_t ckey[MG_UPD_KEY_MAX];
	/* What started the prompt: an upload (alen, asha) or a change (akey). */
	bool armed_upload;
	uint32_t alen;
	uint8_t asha[32];
	uint16_t akey_len;
	uint8_t akey[MG_UPD_KEY_MAX];
	/* The binding: survives link drops. */
	bool bound;
	uint32_t blen;
	uint8_t bsha[32];
	int64_t bound_at;
	const char *why; /* why the last write got MG_UPD_DENIED (the log) */
};

static inline void mg_upd_conn(struct mg_upd *u, uint32_t gen)
{
	if (u->gen != gen) {
		u->gen = gen;
		u->here = u->refuse_next = u->prompting = u->shown = u->covered = false;
	}
}

static inline void mg_upd_expire(struct mg_upd *u, int64_t now_ms)
{
	if (u->prompting && now_ms - u->prompt_at >= MG_UPDATE_PROMPT_MS) {
		u->prompting = false;
		u->refuse_next = true;
	}
}

static inline bool mg_upd_bound_live(const struct mg_upd *u, int64_t now_ms)
{
	return u->bound && now_ms - u->bound_at < MG_UPDATE_CONFIRM_MS;
}

static inline bool mg_upd_same(const struct mg_upd *u, uint32_t len, const uint8_t *sha)
{
	return u->bound && sha != NULL && len == u->blen && memcmp(sha, u->bsha, 32) == 0;
}

static inline void mg_upd_end_binding(struct mg_upd *u)
{
	u->here = u->bound = false;
}

static inline bool mg_upd_covers(const struct mg_upd *u, const struct mg_upd_req *r,
				 int64_t now_ms)
{
	return u->covered && r->key != NULL && r->key_len == u->ckey_len &&
	       memcmp(r->key, u->ckey, r->key_len) == 0 &&
	       now_ms - u->covered_at < MG_UPDATE_CONFIRM_MS;
}

/* An update write on a build and link that need the press: MG_UPD_OK,
 * MG_UPD_BUSY, MG_UPD_DENIED, or MG_UPD_VERIFY (then mg_upd_verified() with
 * whether the slot holds the bound image once it's written). *started: this
 * write started the prompt. */
static inline int mg_upd_write(struct mg_upd *u, uint32_t gen, int64_t now_ms,
			       const struct mg_upd_req *r, bool *started)
{
	bool upload = r->kind == MG_UPD_UPLOAD0 || r->kind == MG_UPD_CHUNK;

	*started = false;
	mg_upd_conn(u, gen);
	if (r->kind == MG_UPD_UPLOAD0 && r->sha == NULL) {
		u->why = "an upload that names no SHA-256"; /* nothing to bind a press to */
		return MG_UPD_DENIED;
	}
	if (r->kind == MG_UPD_CHUNK && !u->here) {
		u->why = "a chunk with no upload started on this link";
		return MG_UPD_DENIED;
	}
	if (!u->here && r->kind == MG_UPD_UPLOAD0 && mg_upd_bound_live(u, now_ms) &&
	    mg_upd_same(u, r->len, r->sha)) {
		u->here = true; /* the bound upload started again on this link: no prompt */
	}
	if (u->here) {
		if (r->kind != MG_UPD_UPLOAD0 ||
		    (mg_upd_bound_live(u, now_ms) && mg_upd_same(u, r->len, r->sha))) {
			if (upload && r->end > u->blen) {
				mg_upd_end_binding(u);
				u->why = "a write past the bound length";
				return MG_UPD_DENIED;
			}
			if ((upload && r->end == u->blen) || r->kind == MG_UPD_STATE) {
				return MG_UPD_VERIFY; /* the last chunk, the state write */
			}
			return MG_UPD_OK;
		}
		u->here = false; /* another upload, or the same one after the window */
	}
	if (r->kind != MG_UPD_UPLOAD0 && mg_upd_covers(u, r, now_ms)) {
		return MG_UPD_OK;
	}
	mg_upd_expire(u, now_ms);
	if (u->refuse_next) {
		u->refuse_next = false;
		u->why = "no token proof: the press prompt ended unanswered";
		return MG_UPD_DENIED;
	}
	if (!u->prompting) {
		/* A new prompt at most once per MG_UPDATE_REARM_MS per
		 * connection; until then 10, nothing shows. */
		if (u->shown && now_ms - u->prompt_at < MG_UPDATE_REARM_MS) {
			return MG_UPD_BUSY;
		}
		u->prompting = true;
		u->shown = true;
		u->prompt_at = now_ms;
		u->armed_upload = r->kind == MG_UPD_UPLOAD0;
		u->akey_len = 0;
		if (u->armed_upload) {
			u->alen = r->len;
			memcpy(u->asha, r->sha, 32);
		} else if (r->key != NULL && r->key_len <= MG_UPD_KEY_MAX) {
			u->akey_len = (uint16_t)r->key_len;
			memcpy(u->akey, r->key, r->key_len);
		}
		*started = true;
	}
	return MG_UPD_BUSY;
}

/* After MG_UPD_VERIFY: slot_matches, the slot's first blen bytes (once the
 * write is in) hash to the bound SHA-256 (mg_ota.c computes it). A mismatch,
 * or a binding gone meanwhile, is MG_UPD_DENIED and ends the binding
 * (mg-update-v2's "bytes that aren't the bound image are refused at the last
 * chunk", "a state write before the last chunk doesn't stage"). */
static inline int mg_upd_verified(struct mg_upd *u, uint32_t gen, bool slot_matches)
{
	mg_upd_conn(u, gen);
	if (u->here && u->bound && slot_matches) {
		return MG_UPD_OK;
	}
	mg_upd_end_binding(u);
	u->why = "slot1 doesn't hold the image the press was bound to";
	return MG_UPD_DENIED;
}

/* An ownership change (Forget, a setup's commit): the prompt, the binding
 * and the covered change end (the link's proof ends with the token); the
 * prompt's rate stays. */
static inline void mg_upd_forget(struct mg_upd *u)
{
	u->prompting = u->armed_upload = u->covered = false;
	u->akey_len = 0;
	mg_upd_end_binding(u);
}

/* The link ended: its prompt, its part in the upload and its covered change
 * go (a new link starts clean anyway, mg_upd_conn()); the binding stays. */
static inline void mg_upd_link_ended(struct mg_upd *u)
{
	u->prompting = u->here = u->refuse_next = u->covered = false;
}

/* A press on connection gen: true if it answered the prompt. */
static inline bool mg_upd_press(struct mg_upd *u, uint32_t gen, int64_t now_ms, bool real,
				bool injected_counts)
{
	mg_upd_conn(u, gen);
	mg_upd_expire(u, now_ms);
	if (!u->prompting || !(real || injected_counts)) {
		return false;
	}
	u->prompting = false;
	if (u->armed_upload) {
		/* Bound to the upload whose write started the prompt, on any
		 * link for MG_UPDATE_CONFIRM_MS; this link carries it. */
		u->bound = true;
		u->blen = u->alen;
		memcpy(u->bsha, u->asha, 32);
		u->bound_at = now_ms;
		u->here = true;
	} else {
		/* Only the change that started it, on this link. */
		u->covered = u->akey_len > 0;
		u->ckey_len = u->akey_len;
		memcpy(u->ckey, u->akey, u->akey_len);
		u->covered_at = now_ms;
	}
	return true;
}

static inline bool mg_upd_prompting(struct mg_upd *u, uint32_t gen, int64_t now_ms)
{
	mg_upd_conn(u, gen);
	mg_upd_expire(u, now_ms);
	return u->prompting;
}

/* Which builds take the update prompt: a release build, neither automation
 * nor bench mode; every bench or automation build takes update writes with
 * no press. */
static inline bool mg_ota_release_build(bool automation, bool bench_mode)
{
	return !automation && !bench_mode;
}

/* Something a press allowed is live on this link (the Nordic UART's
 * `status`: update admitted). */
static inline bool mg_upd_allowed(struct mg_upd *u, uint32_t gen)
{
	mg_upd_conn(u, gen);
	return u->here || u->covered;
}

/* A press's admission still counts for this link's reachability
 * (mg_reach_update_live()'s admitted): the bound upload it carries, or the
 * change the press covered, within MG_UPDATE_CONFIRM_MS of the press. Past
 * that an idle link holds nothing by it (a transfer still moving counts by
 * its chunks), so a link left idle after a press can't keep the single
 * connection for good. */
static inline bool mg_upd_admitted_live(struct mg_upd *u, uint32_t gen, int64_t now_ms)
{
	mg_upd_conn(u, gen);
	return (u->here && mg_upd_bound_live(u, now_ms)) ||
	       (u->covered && now_ms - u->covered_at < MG_UPDATE_CONFIRM_MS);
}

static inline bool mg_ota_window_open_now(uint32_t window_gen, int64_t until_ms, uint32_t conn_gen,
					  int64_t now_ms)
{
	return window_gen != 0 && window_gen == conn_gen && now_ms < until_ms;
}

static inline bool mg_ota_smp_allowed(uint16_t group, uint8_t id, uint8_t op, bool proven,
				      bool safe, bool button_safe, bool release, bool window)
{
	bool update_write = op == MG_SMP_OP_WRITE &&
			    (group == MG_SMP_GROUP_IMAGE ||
			     (group == MG_SMP_GROUP_OS && id == MG_SMP_OS_RESET));

	/* The one physical confirm is for a release build on a link without the
	 * proof, set up or not (a gadget with no device token, not set up or
	 * after Forget, asks the same); the wired arm, the button's SAFE and
	 * bench and automation builds update with none. */
	if (!update_write || window || !release || (safe && button_safe)) {
		return true;
	}
	return proven;
}

/* A SAFE the gadget entered by itself (boot loop, crash guard) on a release
 * build has no way to prompt: an update write there is refused outright
 * (MGMT_ERR_EACCESSDENIED, rc 11), never asked, set up or not; reads stay
 * open, and the way back is the button's SAFE or SWD. The wired console's
 * arm still opens it. */
static inline bool mg_ota_smp_closed(uint16_t group, uint8_t id, uint8_t op, bool safe,
				     bool button_safe, bool release, bool window)
{
	bool update_write = op == MG_SMP_OP_WRITE &&
			    (group == MG_SMP_GROUP_IMAGE ||
			     (group == MG_SMP_GROUP_OS && id == MG_SMP_OS_RESET));

	return update_write && release && safe && !button_safe && !window;
}

/*
 * SMP over Bluetooth without CONFIG_MG_DFU (CONFIG_MG_SMP_GATE_BOARD,
 * mg_smp_gate.c), where a board may keep any of MCUmgr's groups: on a set-up
 * gadget every SMP command, read or write, in any group, needs this
 * connection's token proof (in SAFE mode: SAFE asked with the button, as
 * above), except the OS group's harmless status, which reads nothing stored:
 * echo, task and memory pool statistics, MCUmgr's parameters, OS and
 * bootloader info. A gadget that isn't set up stays open (Kconfig keeps the
 * groups that would expose or change stored data or files off: settings, fs,
 * shell). This covers mg_ota_smp_allowed() above: none of the harmless ones
 * is an update write.
 */
#define MG_SMP_OP_READ            0 /* MGMT_OP_READ */
#define MG_SMP_OS_ECHO            0 /* OS_MGMT_ID_ECHO */
#define MG_SMP_OS_TASKSTAT        2 /* OS_MGMT_ID_TASKSTAT */
#define MG_SMP_OS_MPSTAT          3 /* OS_MGMT_ID_MPSTAT */
#define MG_SMP_OS_MCUMGR_PARAMS   6 /* OS_MGMT_ID_MCUMGR_PARAMS */
#define MG_SMP_OS_INFO            7 /* OS_MGMT_ID_INFO */
#define MG_SMP_OS_BOOTLOADER_INFO 8 /* OS_MGMT_ID_BOOTLOADER_INFO */

static inline bool mg_smp_harmless(uint16_t group, uint8_t id, uint8_t op)
{
	if (group != MG_SMP_GROUP_OS) {
		return false;
	}
	if (id == MG_SMP_OS_ECHO) {
		return true; /* echo answers what it was sent, read or write */
	}
	return op == MG_SMP_OP_READ &&
	       (id == MG_SMP_OS_TASKSTAT || id == MG_SMP_OS_MPSTAT ||
		id == MG_SMP_OS_MCUMGR_PARAMS || id == MG_SMP_OS_INFO ||
		id == MG_SMP_OS_BOOTLOADER_INFO);
}

static inline bool mg_smp_gate_allowed(uint16_t group, uint8_t id, uint8_t op, bool set_up,
				       bool proven, bool safe, bool button_safe)
{
	if (!set_up || mg_smp_harmless(group, id, op)) {
		return true;
	}
	return safe ? button_safe : proven;
}

/*
 * The carry-on: once a connection's upload passed the check above, the rest
 * of that transfer runs on that connection without being asked again, even if
 * the proof lapses meanwhile (the client challenges again, say): its chunks,
 * a retry from offset 0 with the same size and sha, and, once the last chunk
 * is in, the image state write and the OS reset. A new upload (another size or sha), an erase, and
 * anything on another connection are asked again; a link drop ends it.
 */
#define MG_SMP_IMG_STATE  0 /* IMG_MGMT_ID_STATE */
#define MG_SMP_IMG_UPLOAD 1 /* IMG_MGMT_ID_UPLOAD */

/* A request the check refused: whether it may go on as part of the allowed
 * transfer on this connection (an upload is then checked chunk by chunk,
 * mg_ota_smp_same_transfer). staged: that transfer's last chunk is in. */
static inline bool mg_ota_smp_carries(uint16_t group, uint8_t id, uint8_t op, bool same_conn,
				      bool staged)
{
	if (!same_conn || op != MG_SMP_OP_WRITE) {
		return false;
	}
	if (group == MG_SMP_GROUP_IMAGE) {
		return id == MG_SMP_IMG_UPLOAD || (id == MG_SMP_IMG_STATE && staged);
	}
	return group == MG_SMP_GROUP_OS && id == MG_SMP_OS_RESET && staged;
}

/* An upload chunk belongs to the allowed transfer: its size and sha (at
 * offset 0 the request's own, later the upload img_mgmt has under way) are
 * that transfer's. Without a sha there is nothing to tie it to. */
static inline bool mg_ota_smp_same_transfer(size_t xfer_size, const uint8_t *xfer_sha,
					    size_t xfer_sha_len, size_t size, const uint8_t *sha,
					    size_t sha_len)
{
	if (xfer_sha_len == 0 || sha_len != xfer_sha_len || size != xfer_size) {
		return false;
	}
	for (size_t i = 0; i < sha_len; i++) {
		if (sha[i] != xfer_sha[i]) {
			return false;
		}
	}
	return true;
}

/* The trial window: an image still on test after MG_OTA_TRIAL_MS of
 * counted time restarts on its own, so MCUboot reverts it; a broken trial
 * image (a dead TX path, say) would otherwise only drop links for good,
 * since peer losses never restart it while it's on test. Time counts only
 * with no link or with a token-proven one: a stranger holding the single
 * link can't run the clock down and make a good update revert while the
 * owner can't reach it. MG_OTA_TRIAL_CAP_MS of uptime reverts an
 * unconfirmed trial regardless. Local failure (a crash, the watchdog, never
 * healthy) reverts at once through MCUboot, as before. */
#define MG_OTA_TRIAL_MS     (30 * 60 * 1000)
#define MG_OTA_TRIAL_CAP_MS (4 * 60 * 60 * 1000)

/* One tick of the trial clock: `step_ms` counts only if no unproven peer held
 * the link at any point in it (`unproven_seen`: latched at every connection
 * and at the tick if the link is up unproven; one sample per tick would let
 * a peer that dropped just before each tick run the clock down). */
static inline int64_t mg_ota_trial_count(int64_t counted_ms, int64_t step_ms, bool unproven_seen)
{
	return unproven_seen ? counted_ms : counted_ms + step_ms;
}

static inline bool mg_ota_trial_expired(bool confirmed, int64_t counted_ms, int64_t uptime_ms)
{
	return !confirmed && (counted_ms >= MG_OTA_TRIAL_MS || uptime_ms >= MG_OTA_TRIAL_CAP_MS);
}

/* An accepted upload chunk within MG_OTA_TRIAL_UPLOAD_IDLE_MS defers the
 * 30 min restart (a finished or abandoned upload sends no "stopped" event);
 * the 4 h cap is never deferred. */
#define MG_OTA_TRIAL_UPLOAD_IDLE_MS 60000

static inline bool mg_ota_trial_upload_moving(int64_t last_chunk_ms, int64_t now_ms)
{
	return last_chunk_ms > 0 && now_ms - last_chunk_ms < MG_OTA_TRIAL_UPLOAD_IDLE_MS;
}

/* MCUboot's swap type at a trial tick. A trial image that just swapped in
 * reads REVERT (good magic, copy done, image_ok unset): the trial goes on. A
 * replacement staged in slot1 (TEST or PERM) would be installed by a restart,
 * not reverted to: the owner's update now, so the timer stands down. */
enum mg_ota_swap { MG_OTA_SWAP_NONE, MG_OTA_SWAP_TEST, MG_OTA_SWAP_PERM, MG_OTA_SWAP_REVERT,
		   MG_OTA_SWAP_OTHER };

enum mg_ota_trial_act { MG_OTA_TRIAL_GO_ON, MG_OTA_TRIAL_DONE, MG_OTA_TRIAL_REVERT };

/* Whether an unproven peer may have held the link at some point in the tick
 * just ended: a connection or disconnection in it (latched), a link up
 * unproven now, or one up unproven at the tick before (it held the start of
 * this interval even if it's gone now). */
static inline bool mg_ota_trial_unproven(bool edge_seen, bool link_up, bool proven,
					 bool prev_unproven)
{
	return edge_seen || (link_up && !proven) || prev_unproven;
}

/* The whole tick (trial_fn calls only this; host test_trial_tick drives it). */
static inline enum mg_ota_trial_act mg_ota_trial_tick(int64_t *counted_ms, int64_t step_ms,
						      int64_t now_ms, bool confirmed,
						      enum mg_ota_swap swap, bool unproven_seen,
						      int64_t last_chunk_ms)
{
	if (confirmed || swap == MG_OTA_SWAP_TEST || swap == MG_OTA_SWAP_PERM) {
		return MG_OTA_TRIAL_DONE;
	}
	*counted_ms = mg_ota_trial_count(*counted_ms, step_ms, unproven_seen);
	if (now_ms >= MG_OTA_TRIAL_CAP_MS) {
		return MG_OTA_TRIAL_REVERT;
	}
	if (*counted_ms >= MG_OTA_TRIAL_MS && !mg_ota_trial_upload_moving(last_chunk_ms, now_ms)) {
		return MG_OTA_TRIAL_REVERT;
	}
	return MG_OTA_TRIAL_GO_ON;
}

/* An updated image keeps itself (and a client's earlier confirm takes
 * effect) once the boot is healthy (board-ready included, mg_health.c) AND
 * either a link passed the gate with a completed ATT exchange through the
 * gadget's own path (mg_reach_policy.h, mg_reach_att_counts()) or any
 * central's update-path read (an SMP image list / state read: the web
 * updater's post-install check, the Keep signal) was answered by this image
 * after the boot. Never on advertising or a timer alone: an image no central
 * can talk to stays on test and the next reset reverts it. */
static inline bool mg_ota_confirm_ready(bool boot_healthy, bool link_proven, bool update_read)
{
	return boot_healthy && (link_proven || update_read);
}

/* In SAFE mode of either kind a client's confirm of the running image is
 * refused: a SAFE boot isn't the image working, and the button's SAFE can run
 * a test image just swapped in, which a confirm there would keep without its
 * normal boot ever getting healthy. A test or confirm of the other slot's
 * image still passes (the update path SAFE is for). */
static inline enum mg_ota_set_next mg_ota_set_next(bool healthy, bool active, bool confirm,
						   bool safe)
{
	if (active && confirm && safe) {
		return MG_OTA_SET_NEXT_REFUSE;
	}
	return !healthy && active && confirm ? MG_OTA_SET_NEXT_LATCH : MG_OTA_SET_NEXT_PASS;
}

/* Whether to confirm the running test image now. The bench revert build
 * (CONFIG_MG_BENCH_NO_CONFIRM) keeps it only when a client confirmed it.
 * Nothing is confirmed in SAFE mode (that boot isn't the image working;
 * a test image never gets there anyway: MCUboot swaps it back at the first
 * unhealthy reset). */
static inline bool mg_ota_should_confirm(bool healthy, bool safe, bool confirmed,
					 bool client_confirmed, bool bench_no_confirm)
{
	if (!healthy || safe || confirmed) {
		return false;
	}
	return client_confirmed || !bench_no_confirm;
}

/*
 * SMP over BLE: a request spans several ATT writes, which Zephyr's transport
 * reassembles by the length in the SMP header, with no timeout. A write lost
 * on the way (the iOS app writes without response and doesn't wait for
 * canSendWriteWithoutResponse) leaves the partial request waiting, and the
 * client's retry is then appended to it: every retry fails until the link
 * drops. A request's writes come back to back (milliseconds apart) and
 * every client waits seconds before it retries (Android 5 s, iOS 30 s), so
 * a write that comes more than CONFIG_MG_SMP_RX_GAP_MS after the previous
 * one starts afresh: the partial request (expected > 0 bytes still missing)
 * is dropped first. last_ms 0: no write yet.
 */
static inline bool mg_smp_rx_stale(int64_t now_ms, int64_t last_ms, int expected, uint32_t gap_ms)
{
	return gap_ms != 0 && last_ms != 0 && expected > 0 && now_ms - last_ms > (int64_t)gap_ms;
}

/* A request's source is the connection its first fragment came on: a partly
 * received request (expected >= 0) whose connection isn't the link any more
 * is dropped before the next write is collected, so another connection's
 * bytes never complete it and lend it their proof. */
static inline bool mg_smp_rx_orphan(int expected, uint32_t first_gen, uint32_t cur_gen)
{
	return expected >= 0 && first_gen != cur_gen;
}

/* Slot1 shared with the clips (CONFIG_MG_QUEUE_IN_SLOT1): an upload holds it
 * from its first chunk until a reset, or until the client erases the slot
 * (the upload abandoned). img_mgmt reports nothing between the last chunk
 * and the state write that stages the image, and a stopped upload resumes
 * from what is on flash, so neither ends the hold; once staged, the pending
 * swap keeps the slot too. */
enum mg_ota_slot_evt {
	MG_OTA_SLOT_STARTED, /* an upload's first chunk */
	MG_OTA_SLOT_STOPPED, /* an upload stopped (an error; it may resume) */
	MG_OTA_SLOT_PENDING, /* its last chunk is in, not staged yet */
	MG_OTA_SLOT_ERASED,  /* the client erased the slot */
};

static inline bool mg_ota_slot_held(bool held, enum mg_ota_slot_evt e)
{
	return e == MG_OTA_SLOT_STARTED ? true : e == MG_OTA_SLOT_ERASED ? false : held;
}

#endif
