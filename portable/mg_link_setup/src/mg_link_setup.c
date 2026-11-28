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
 * Muse Link setup v5, device side, portable. Follows the reference firmware
 * message for message (esp32/main/ble_server.c dispatch and statuses,
 * link_pairing.c session, zephyr/src/mg_setup.c), with the Muse Gadget setup
 * contract v2 changes: hello is accepted on a set-up gadget, provision_v2 on
 * a confirmed session replaces the stored set through one atomic commit,
 * sdk_token comes from the platform.
 */

#include "mg_link_setup.h"

#include <string.h>

#include "mg_token_proof.h"
#include "mgls_crypto.h"
#include "mgls_util.h"

#define CHUNK_MAGIC 0xFE
#define CHUNK_HDR   3
#define MAX_NOTIFY  160 /* ble_server.c MAX_NOTIFY_CHUNK */
#define SINGLE_MAX  248 /* an unframed write (mg_setup.c) */
/* The tag goes after a ciphertext decoded in place, then a NUL. */
#define RX_MSG_MAX  (MGLS_RX_CAP - 16 - 1)

#define VERSION        5
#define MODEL          "hatch_link"
#define SUITE          "p256-hkdf-sha256-aes-gcm-v1"
#define AUTH           "none"
#define POLICY         "confirm_press"
#define RECORD_LABEL   "hatch-link ble setup v1"
#define SID_LABEL      "hatch-link session id v1"
#define TRANSCRIPT_CAP (548 + 4 * MGLS_ID_MAX + 1)

#define M2D 0
#define D2M 1

enum phase {
	PH_IDLE,
	PH_WAIT_FINISHED,
	PH_CONFIRM_REQUIRED,
	PH_READY,
	PH_PROVISIONING,
	PH_DONE, /* auth_ok sent on this session */
};

#define F_CONNECTED  0x01u
#define F_ARMED      0x02u /* confirm_required went out: the press confirms */
#define F_PT_BLOCKED 0x04u /* plaintext ignored after pairing_ready (until disconnect) */
#define F_PRESS      0x08u /* a press waits for the next poll */
#define F_DISC       0x10u /* a disconnect is due at disc_at */
#define F_DEADLINE   0x20u /* the phase deadline runs */
#define F_RESETUP    0x40u /* this session sets up a gadget that is set up (no ota_url; may_prompt()) */
#define F_SHOWN      0x80u /* the prompt is up: from its arm until it's confirmed or its session ends, across a takeover */

enum rx_state {
	RXS_IDLE,
	RXS_ASSEMBLING,
	RXS_COMPLETE,
	RXS_OVERFLOW,
};

enum tx_kind {
	TXK_NONE,
	TXK_MSG,   /* chunked, at tx + 3 */
	TXK_PLAIN, /* a bare plaintext status */
};

/* What happens once a transmission is over (sent or failed). */
enum after {
	AFTER_NONE,
	AFTER_ARM,          /* sent: the press is armed at the next poll; failed: disconnect */
	AFTER_DISC_IF_FAIL, /* failed: disconnect */
	AFTER_DISC0,
	AFTER_DISC100,
	AFTER_DISC300,
	AFTER_DISC500,
};

/* Every status the device sends. The first six may go out as plaintext
 * (and so be what a read of TX returns). */
enum status {
	ST_IDLE,
	ST_ENCRYPTED,
	ST_ENC_REQUIRED,
	ST_INVALID_HELLO,
	ST_UNAVAILABLE,
	ST_DECRYPT,
	ST_PLAINTEXT_LAST = ST_DECRYPT,
	ST_INVALID_COMMAND,
	ST_UNKNOWN_ACTION,
	ST_CONFIRM_REQUIRED,
	ST_PAIRING_CONFIRMED,
	ST_CONFIRM_TIMEOUT,
	ST_AUTH_OK,
	ST_STORAGE,
	ST_MISSING_CREDENTIALS,
	ST_WIFI_UNSUPPORTED,
	ST_IN_PROGRESS,
	ST_CONFIRM_REQUIRED_ERR,
	ST_UNPAIRED,
};

static const char *const STATUS[] = {
	"idle",
	"encrypted_status",
	"error_encryption_required",
	"error_pairing_invalid_hello",
	"error_pairing_unavailable",
	"error_pairing_decrypt",
	"error_invalid_command",
	"error_unknown_action",
	"confirm_required",
	"pairing_confirmed",
	"pairing_confirm_timeout",
	"auth_ok",
	"error_storage",
	"error_missing_credentials",
	"error_wifi_unsupported",
	"error_operation_in_progress",
	"error_pairing_confirm_required",
	"unpaired",
};

/* hello's working memory, at the end of the RX buffer behind the message. */
struct hello_scratch {
	mglsc_p256_t key;
	uint8_t mpub[65], dpub[65];
	uint8_t mnonce[16], dnonce[16];
	uint8_t ss[32], th[32], salt_in[64], tmp[32], secret[32];
	char mpub_b64[88], dpub_b64[88], mn_b64[24], dn_b64[24], th_b64[44];
	char transcript[TRANSCRIPT_CAP];
};

#define SCRATCH_SIZE MGLS__ALIGN4(sizeof(struct hello_scratch))

typedef char mgls_check_scratch[(SCRATCH_SIZE + 512 <= MGLS_RX_CAP) ? 1 : -1];
typedef char mgls_check_counter[(MGLS_RX_CAP < 65536 && MGLS_TX_CAP < 65536) ? 1 : -1];

#if MGLS_ENABLE_LOG
static void mgls_log_(const mgls_t *s, const char *fmt, long a, long b)
{
	if (s->cfg->log) {
		s->cfg->log(s->cfg->ctx, fmt, a, b);
	}
}
#define LOG2(s, fmt, a, b) mgls_log_(s, "mgls: " fmt, (long)(a), (long)(b))
#else
#define LOG2(s, fmt, a, b) ((void)(s))
#endif
#define LOG1(s, fmt, a) LOG2(s, fmt, a, 0)
#define LOG(s, fmt)     LOG2(s, fmt, 0, 0)

#if defined(MGLS_TEST_HOOKS)
static bool test_eph_set;
static uint8_t test_eph[32];

void mgls_test_set_ephemeral(const uint8_t priv[32])
{
	memcpy(test_eph, priv, 32);
	test_eph_set = true;
}
#endif

static void event(mgls_t *s, enum mgls_event ev)
{
	if (s->cfg->event) {
		s->cfg->event(s->cfg->ctx, ev);
	}
}

static bool due(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

/* ---- Identity ------------------------------------------------------------ */

/* Two hex digits; upper: 0 lowercase, 1 uppercase. */
static void hex_byte(char *out, uint8_t b, int upper)
{
	static const char hex[] = "0123456789abcdef0123456789ABCDEF";

	out[0] = hex[upper * 16 + (b >> 4)];
	out[1] = hex[upper * 16 + (b & 15)];
}

static void derive_identity(mgls_t *s)
{
	mgls_workspace_t *ws = s->ws;
	const uint8_t *a = s->cfg->bd_addr;

	if (ws->id_ready) {
		return;
	}
	ws->id_ready = 1;
	if (a == NULL) {
		return;
	}
	for (int i = 0; i < 6; i++) {
		hex_byte(&ws->id_mac[i * 3], a[i], 0);
		ws->id_mac[i * 3 + 2] = i < 5 ? ':' : '\0';
	}
	memcpy(ws->id_node, "homelink-", 9);
	for (int i = 0; i < 3; i++) {
		hex_byte(&ws->id_node[9 + i * 2], a[3 + i], 0);
	}
	ws->id_node[15] = '\0';
	memcpy(ws->id_dev, "hatch-link:", 11);
	memcpy(&ws->id_dev[11], ws->id_mac, 18);
}

static const char *id_node(const mgls_t *s)
{
	return s->cfg->node_id ? s->cfg->node_id : s->ws->id_node;
}

static const char *id_mac(const mgls_t *s)
{
	return s->cfg->mac ? s->cfg->mac : s->ws->id_mac;
}

static const char *id_device(const mgls_t *s)
{
	return s->cfg->device_id ? s->cfg->device_id : s->ws->id_dev;
}

static const char *fw_version(const mgls_t *s)
{
	return s->cfg->firmware_version ? s->cfg->firmware_version : "";
}

/* ---- Workspace ----------------------------------------------------------- */

static bool attach(mgls_t *s)
{
	void *p;

	if (s->ws != NULL) {
		return true;
	}
	p = s->cfg->borrow ? s->cfg->borrow(s->cfg->ctx, sizeof(mgls_workspace_t)) : NULL;
	if (p == NULL || ((uintptr_t)p & 3u) != 0) {
		if (p != NULL && s->cfg->release) {
			s->cfg->release(s->cfg->ctx, p);
		}
		return false;
	}
	memset(p, 0, sizeof(mgls_workspace_t));
	s->ws = p;
	derive_identity(s);
	return true;
}

static void detach(mgls_t *s)
{
	mgls_workspace_t *ws = s->ws;

	if (ws == NULL) {
		return;
	}
	mglsc_wipe(ws, sizeof(*ws));
	s->ws = NULL;
	if (s->cfg->release) {
		s->cfg->release(s->cfg->ctx, ws);
	}
}

/* ---- Session ------------------------------------------------------------- */

static void advance_gen(mgls_t *s)
{
	if (++s->gen == 0) {
		s->gen = 1; /* 0 means "any session" */
	}
}

static bool keys_available(const mgls_t *s)
{
	return s->phase != PH_IDLE && s->ws != NULL;
}

static bool confirmed(const mgls_t *s)
{
	return s->phase == PH_READY || s->phase == PH_PROVISIONING;
}

static void set_deadline(mgls_t *s, uint32_t ms)
{
	s->deadline = s->now + ms;
	s->flags |= F_DEADLINE;
}

/* Forgets the session's keys and phase (link_pairing.c reset_locked()). takeover: a valid new hello on the prompt's
 * own connection takes over the prompt that is up: it stays up (no session_ended, a press made meanwhile kept for the
 * new session). Otherwise a prompt that is up ends with it, in any phase (session_ended). */
static void wipe_session(mgls_t *s, bool takeover)
{
	bool active = (s->phase != PH_IDLE && s->phase != PH_DONE) || (s->flags & F_SHOWN) != 0;
	bool keep = takeover && (s->flags & F_SHOWN);

	if (s->ws != NULL) {
		mglsc_wipe(s->ws->rx_key, sizeof(s->ws->rx_key));
		mglsc_wipe(s->ws->tx_key, sizeof(s->ws->tx_key));
		mglsc_wipe(s->ws->sid_b64, sizeof(s->ws->sid_b64));
		s->ws->rx_ctr = 0;
		s->ws->tx_ctr = 0;
	}
	s->phase = PH_IDLE;
	s->flags &= (uint8_t)~(F_ARMED | F_DEADLINE | F_RESETUP | (keep ? 0u : (F_PRESS | F_SHOWN)));
	if (active && !keep) {
		event(s, MGLS_EV_SESSION_ENDED);
	}
}

static void clear_session(mgls_t *s)
{
	wipe_session(s, false);
}

static void reset_session(mgls_t *s)
{
	advance_gen(s);
	clear_session(s);
}

/* A disconnect `ms` from now: of the session `gen` (0: whatever runs then). */
static void disconnect_later(mgls_t *s, uint32_t ms, uint8_t gen)
{
	s->disc_gen = gen;
	s->disc_at = s->now + ms;
	s->flags |= F_DISC;
}

/* ---- Sending ------------------------------------------------------------- */

static void arm(mgls_t *s)
{
	if (s->phase != PH_CONFIRM_REQUIRED || !(s->flags & F_CONNECTED)) {
		return;
	}
	s->flags &= (uint8_t)~F_DEADLINE; /* no timer on the prompt: a press, a dismiss or the phone ends it */
	if (s->flags & F_SHOWN) {
		s->flags |= F_ARMED; /* a takeover's: the prompt never left the screen */
		LOG(s, "prompt taken over");
		return;
	}
	s->flags |= F_ARMED | F_SHOWN;
	if (s->flags & F_RESETUP) {
		LOG(s, "re-setup: press to confirm");
	} else {
		LOG(s, "press to confirm");
	}
	event(s, MGLS_EV_CONFIRM_ARMED);
}

static void finish_after(mgls_t *s, uint8_t after, bool ok)
{
	switch (after) {
	case AFTER_ARM:
		if (ok) {
			/* It went out during this call, after its poll's time: armed by the next mgls_poll() (the mark
			 * stays while nothing else is queued), at that poll's time. */
			s->ws->tx_after = AFTER_ARM;
		} else {
			disconnect_later(s, 0, s->gen);
		}
		break;
	case AFTER_DISC_IF_FAIL:
		if (!ok) {
			disconnect_later(s, 0, s->gen);
		}
		break;
	case AFTER_DISC0:
		disconnect_later(s, 0, s->disc_gen);
		break;
	case AFTER_DISC100:
		disconnect_later(s, 100, s->disc_gen);
		break;
	case AFTER_DISC300:
		disconnect_later(s, 300, s->disc_gen);
		break;
	case AFTER_DISC500:
		disconnect_later(s, 500, s->disc_gen);
		break;
	default:
		break;
	}
}

/* One notification to the connection the core follows (notify_to with its id, else notify). */
static int notify(mgls_t *s, const uint8_t *data, size_t len)
{
	if (s->cfg->notify_to != NULL) {
		return s->cfg->notify_to(s->cfg->ctx, s->conn, data, len);
	}
	return s->cfg->notify != NULL ? s->cfg->notify(s->cfg->ctx, data, len) : -1;
}

/* Sends what is queued, a notification at a time, until the platform has no
 * buffer (MGLS_AGAIN: resumed at the next poll) or it has all gone out. */
static void tx_pump(mgls_t *s)
{
	mgls_workspace_t *ws = s->ws;

	while (ws != NULL && ws->tx_kind != TXK_NONE) {
		int r;

		if (!(s->flags & F_CONNECTED)) {
			r = -1;
		} else if (ws->tx_kind == TXK_PLAIN) {
			const char *st = STATUS[ws->tx_status];

			r = notify(s, (const uint8_t *)st, strlen(st));
		} else {
			/* The chunk's header goes in the 3 bytes in front of its
			 * payload: the previous chunk's tail, already sent. */
			size_t off = (size_t)ws->tx_idx * ws->tx_usable;
			size_t n = ws->tx_len - off;
			uint8_t *p = &ws->tx[off];

			if (n > ws->tx_usable) {
				n = ws->tx_usable;
			}
			p[0] = CHUNK_MAGIC;
			p[1] = ws->tx_idx;
			p[2] = ws->tx_chunks;
			r = notify(s, p, n + CHUNK_HDR);
		}
		if (s->ws != ws) {
			return; /* the callback ended the connection (mgls_disconnected) */
		}
		if (r == MGLS_AGAIN) {
			return;
		}
		if (r == MGLS_OK && ws->tx_kind == TXK_MSG && ++ws->tx_idx < ws->tx_chunks) {
			continue;
		}
		uint8_t after = ws->tx_after;

		if (r != MGLS_OK) {
			LOG1(s, "notify failed (%ld)", r);
		}
		ws->tx_kind = TXK_NONE;
		ws->tx_after = AFTER_NONE;
		mglsc_wipe(ws->tx, sizeof(ws->tx));
		finish_after(s, after, r == MGLS_OK);
	}
}

/* Queues the message at tx + 3: [0xFE, index, total, fragment], each at
 * most 160 bytes and MTU - 3 (ble_server.c). */
static bool queue_msg(mgls_t *s, size_t len, uint8_t after)
{
	mgls_workspace_t *ws = s->ws;
	size_t max = s->mtu > 3 ? (size_t)s->mtu - 3 : 0;
	size_t usable, chunks;

	if (max > MAX_NOTIFY) {
		max = MAX_NOTIFY;
	}
	if (max <= CHUNK_HDR) {
		finish_after(s, after, false);
		return false;
	}
	usable = max - CHUNK_HDR;
	chunks = (len + usable - 1) / usable;
	if (chunks == 0) {
		chunks = 1;
	}
	if (chunks > 255) {
		LOG1(s, "TX %ld B: too long", len);
		finish_after(s, after, false);
		return false;
	}
	ws->tx_kind = TXK_MSG;
	ws->tx_len = (uint16_t)len;
	ws->tx_usable = (uint16_t)usable;
	ws->tx_idx = 0;
	ws->tx_chunks = (uint8_t)chunks;
	ws->tx_after = after;
	tx_pump(s);
	return true;
}

/* "label|sid|m2d|counter" (link_pairing.c build_aad). */
static int record_aad(const mgls_t *s, uint8_t dir, uint32_t ctr, char *out, size_t cap)
{
	mgj_w w;

	mgj_begin(&w, out, cap);
	mgj_cstr(&w, RECORD_LABEL "|");
	mgj_cstr(&w, s->ws->sid_b64);
	mgj_cstr(&w, dir == M2D ? "|m2d|" : "|d2m|");
	mgj_u32(&w, ctr);
	return mgj_finish(&w);
}

/* [dir, 0, 0, 0, counter big-endian (8)]. */
static void record_nonce(uint8_t dir, uint32_t ctr, uint8_t out[12])
{
	memset(out, 0, 12);
	out[0] = dir;
	out[8] = (uint8_t)(ctr >> 24);
	out[9] = (uint8_t)(ctr >> 16);
	out[10] = (uint8_t)(ctr >> 8);
	out[11] = (uint8_t)ctr;
}

/* The staging area for a record's plaintext: the end of the TX buffer. */
static uint8_t *enc_pt(mgls_t *s)
{
	return &s->ws->tx[MGLS_TX_CAP - MGLS__ENC_PT_CAP - 16];
}

/* Seals the n plaintext bytes staged at enc_pt() into a pairing_encrypted
 * envelope at tx + 3 and queues it. */
static bool queue_record(mgls_t *s, size_t n, uint8_t after)
{
	mgls_workspace_t *ws = s->ws;
	uint8_t *pt = enc_pt(s);
	uint8_t nonce[12];
	char aad[80];
	int al = record_aad(s, D2M, ws->tx_ctr, aad, sizeof(aad));
	mgj_w w;
	int len, b;

	record_nonce(D2M, ws->tx_ctr, nonce);
	if (al < 0 ||
	    mglsc_gcm_seal(ws->tx_key, nonce, (const uint8_t *)aad, (size_t)al, pt, n)) {
		mglsc_wipe(pt, n + 16);
		finish_after(s, after, false);
		return false;
	}
	mgj_begin(&w, (char *)&ws->tx[CHUNK_HDR], (size_t)(pt - &ws->tx[CHUNK_HDR]));
	mgj_cstr(&w, "{\"type\":\"pairing_encrypted\",\"session_id\":\"");
	mgj_cstr(&w, ws->sid_b64);
	mgj_cstr(&w, "\",\"counter\":\"");
	mgj_u32(&w, ws->tx_ctr);
	mgj_cstr(&w, "\",\"ciphertext\":\"");
	b = w.err ? -1 : mgls_b64_encode(pt, n, &w.buf[w.len], w.cap - w.len);
	if (b < 0) {
		w.err = true;
	} else {
		w.len += (size_t)b;
	}
	mgj_cstr(&w, "\",\"tag\":\"");
	b = w.err ? -1 : mgls_b64_encode(&pt[n], 16, &w.buf[w.len], w.cap - w.len);
	if (b < 0) {
		w.err = true;
	} else {
		w.len += (size_t)b;
	}
	mgj_cstr(&w, "\"}");
	len = mgj_finish(&w);
	mglsc_wipe(pt, n + 16);
	if (len < 0) {
		finish_after(s, after, false);
		return false;
	}
	/* The counter is spent once the record exists, sent or not. */
	ws->tx_ctr++;
	return queue_msg(s, (size_t)len, after);
}

static bool plaintext_allowed(enum status st)
{
	return st >= ST_ENC_REQUIRED && st <= ST_PLAINTEXT_LAST;
}

/*
 * ble_server.c send_status(): encrypted while a session has keys (gen 0:
 * any session; else only that one); otherwise plaintext, and then only the
 * four errors a client may see before a session, and never once
 * pairing_ready went out on this connection.
 */
/* A bare plaintext status (what a read of TX returns from then on); TX is free. */
static void send_plain(mgls_t *s, enum status st, uint8_t after)
{
	mgls_workspace_t *ws = s->ws;

	s->last_status = (uint8_t)st;
	ws->tx_kind = TXK_PLAIN;
	ws->tx_status = (uint8_t)st;
	ws->tx_after = after;
	tx_pump(s);
}

static bool send_status(mgls_t *s, enum status st, uint8_t gen, uint8_t after)
{
	mgls_workspace_t *ws = s->ws;

	if (ws == NULL || ws->tx_kind != TXK_NONE) {
		LOG1(s, "status %ld dropped: TX busy", st);
		finish_after(s, after, false);
		return false;
	}
	if (keys_available(s) && (gen == 0 || gen == s->gen)) {
		mgj_w w;
		const char *tok = NULL;
		size_t tl = 0;
		int n;

		mgj_begin(&w, (char *)enc_pt(s), MGLS__ENC_PT_CAP);
		mgj_kstr(&w, "type", "status");
		mgj_kstr(&w, "status", STATUS[st]);
		if (st == ST_PAIRING_CONFIRMED) {
			if (s->cfg->sdk_token) {
				tok = s->cfg->sdk_token(s->cfg->ctx, &tl);
			}
			if (tok != NULL && tl > MGLS_SDK_TOKEN_MAX) {
				LOG2(s, "SDK token too long (%ld > %ld)", tl,
				     MGLS_SDK_TOKEN_MAX);
				tok = NULL;
			}
			if (tok != NULL && tl > 0) {
				mgj_kstrn(&w, "sdk_token", tok, tl);
				LOG1(s, "sdk_token sent (%ld chars)", tl);
			} else {
				LOG(s, "no SDK token");
				event(s, MGLS_EV_NO_SDK_TOKEN);
			}
		}
		n = mgj_end(&w);
		s->last_status = ST_ENCRYPTED;
		if (n < 0) {
			mglsc_wipe(enc_pt(s), MGLS__ENC_PT_CAP);
			finish_after(s, after, false);
			return false;
		}
		return queue_record(s, (size_t)n, after);
	}
	if (gen != 0 || (s->flags & F_PT_BLOCKED) || !plaintext_allowed(st)) {
		LOG1(s, "plaintext status %ld suppressed", st);
		finish_after(s, after, false);
		return false;
	}
	send_plain(s, st, after);
	return true;
}

/* The answer to a pairing_client_hello refused before anything of it (error_operation_in_progress: the re-setup
 * prompt's rate; error_pairing_unavailable: no set_up hook, or a one-key board outside setup mode) in plaintext (the
 * client's new hello has no keys), even after pairing_ready on this connection (a repeat comes after one); a session
 * that runs keeps its keys and phase. */
static void send_hello_refusal(mgls_t *s, enum status st)
{
	if (s->ws->tx_kind != TXK_NONE) {
		LOG1(s, "hello refusal %ld dropped: TX busy", st);
		return;
	}
	send_plain(s, st, AFTER_NONE);
}

/* An encrypted message other than a status (the empty Wi-Fi scan). */
static void send_encrypted_json(mgls_t *s, const char *json)
{
	size_t n = strlen(json);

	if (!keys_available(s) || s->ws->tx_kind != TXK_NONE || n > MGLS__ENC_PT_CAP) {
		return;
	}
	memcpy(enc_pt(s), json, n);
	(void)queue_record(s, n, AFTER_NONE);
}

/* ---- Table-driven messages ------------------------------------------------ */

/* The values device_info, pairing_ready and the transcript are made of: the
 * first ones change per gadget or session, the rest are literals. RAW marks
 * a JSON number. */
enum val {
	V_DEVICE,
	V_NODE,
	V_MAC,
	V_FW,
	V_DPUB,
	V_DNONCE,
	V_TH,
	V_SID,
	V_MPUB,
	V_MNONCE,
	V_DYN_COUNT,
	V_MODEL = V_DYN_COUNT,
	V_AUTH,
	V_POLICY,
	V_EMPTY,
	V_SUITE,
	V_MOBILE,
	V_LINK,
	V_T_READY,
	V_T_INFO,
	V_5,
	V_0,
	V_60,
	V_1,
};

#define RAW 0x80u

static const char *const LIT[] = {
	MODEL, AUTH, POLICY, "", SUITE, "mobile", "link", "pairing_ready", "device_info",
	"5",   "0",  "60",   "1",
};

struct kv {
	const char *key;
	uint8_t v;
};

static const struct kv DEVICE_INFO[] = {
	{"type", V_T_INFO},
	{"node_id", V_NODE},
	{"version", V_FW},
	{"device_id", V_DEVICE},
	{"mac", V_MAC},
	{"model", V_MODEL},
	{"pairing_protocol", V_5 | RAW},
	{"pairing_auth", V_AUTH},
	{"pairing_auth_epoch", V_0 | RAW},
	{"pairing_policy", V_POLICY},
	{"build_sha", V_EMPTY},
	{"wifi", V_AUTH}, /* "none" */
	{"mgcommands", V_1 | RAW}, /* last: dropped when mgcommands is 0 */
};

static const struct kv PAIRING_READY[] = {
	{"type", V_T_READY},
	{"version", V_5 | RAW},
	{"device_id", V_DEVICE},
	{"node_id", V_NODE},
	{"mac", V_MAC},
	{"model", V_MODEL},
	{"firmware_version", V_FW},
	{"pairing_auth", V_AUTH},
	{"pairing_auth_epoch", V_0 | RAW},
	{"pairing_policy", V_POLICY},
	{"device_pub", V_DPUB},
	{"device_nonce", V_DNONCE},
	{"transcript_hash", V_TH},
	{"session_id", V_SID},
};

/* pairing_transcript.c: "hatch-link-pairing-v5" then "\nkey=value" lines. */
static const struct kv TRANSCRIPT[] = {
	{"version", V_5},
	{"initiator_role", V_MOBILE},
	{"responder_role", V_LINK},
	{"device_id", V_DEVICE},
	{"node_id", V_NODE},
	{"mac", V_MAC},
	{"model", V_MODEL},
	{"firmware_version", V_FW},
	{"selected_cipher_suite", V_SUITE},
	{"pairing_auth", V_AUTH},
	{"pairing_auth_epoch", V_0},
	{"pairing_policy", V_POLICY},
	{"confirm_timeout_seconds", V_60},
	{"mobile_pub", V_MPUB},
	{"device_pub", V_DPUB},
	{"mobile_nonce", V_MNONCE},
	{"device_nonce", V_DNONCE},
};

/* json: {"key":"value",...}; else the transcript's lines. */
static void emit(mgj_w *w, const struct kv *t, size_t n, const char *const dyn[], bool json)
{
	for (size_t i = 0; i < n; i++) {
		unsigned int v = t[i].v & ~RAW;
		const char *val = v < V_DYN_COUNT ? dyn[v] : LIT[v - V_DYN_COUNT];

		if (!json) {
			mgj_cstr(w, "\n");
			mgj_cstr(w, t[i].key);
			mgj_cstr(w, "=");
			mgj_cstr(w, val != NULL ? val : "");
		} else if (t[i].v & RAW) {
			mgj_kraw(w, t[i].key, val);
		} else {
			mgj_kstr(w, t[i].key, val);
		}
	}
}

/* ---- Handshake ----------------------------------------------------------- */

static struct hello_scratch *scratch(mgls_t *s)
{
	uintptr_t p = (uintptr_t)&s->ws->rx[MGLS_RX_CAP - SCRATCH_SIZE];

	return (struct hello_scratch *)(p & ~(uintptr_t)3);
}

static bool nonempty(const char *v)
{
	return v != NULL && v[0] != '\0';
}

static void hello_values(mgls_t *s, struct hello_scratch *h, const char *dyn[V_DYN_COUNT])
{
	dyn[V_DEVICE] = id_device(s);
	dyn[V_NODE] = id_node(s);
	dyn[V_MAC] = id_mac(s);
	dyn[V_FW] = fw_version(s);
	dyn[V_DPUB] = h->dpub_b64;
	dyn[V_DNONCE] = h->dn_b64;
	dyn[V_TH] = h->th_b64;
	dyn[V_SID] = s->ws->sid_b64;
	dyn[V_MPUB] = h->mpub_b64;
	dyn[V_MNONCE] = h->mn_b64;
}

/* session_secret = HKDF(salt SHA256(mobile_nonce || device_nonce || th),
 * ikm ss, info RECORD_LABEL); rx / tx = HKDF-Expand(secret, "mobile->device"
 * / "device->mobile"); session_id = SHA256(SID_LABEL || th || ss)[0..16). */
static int derive_keys(mgls_t *s, struct hello_scratch *h)
{
	mgls_workspace_t *ws = s->ws;
	uint8_t *sid_in = (uint8_t *)h->transcript; /* done with it */
	const size_t ll = sizeof(SID_LABEL) - 1;
	int err;

	memcpy(h->salt_in, h->mnonce, 16);
	memcpy(&h->salt_in[16], h->dnonce, 16);
	memcpy(&h->salt_in[32], h->th, 32);
	memcpy(sid_in, SID_LABEL, ll);
	memcpy(&sid_in[ll], h->th, 32);
	memcpy(&sid_in[ll + 32], h->ss, 32);
	err = mglsc_sha256(h->salt_in, sizeof(h->salt_in), h->tmp);
	err = err ? err
		  : mglsc_hkdf32(h->tmp, 32, h->ss, 32, (const uint8_t *)RECORD_LABEL,
				 sizeof(RECORD_LABEL) - 1, h->secret);
	err = err ? err
		  : mglsc_hkdf_expand32(h->secret, (const uint8_t *)"mobile->device", 14,
					ws->rx_key);
	err = err ? err
		  : mglsc_hkdf_expand32(h->secret, (const uint8_t *)"device->mobile", 14,
					ws->tx_key);
	err = err ? err : mglsc_sha256(sid_in, ll + 64, h->tmp);
	if (!err && mgls_b64_encode(h->tmp, 16, ws->sid_b64, sizeof(ws->sid_b64)) < 0) {
		err = -1;
	}
	return err;
}

static enum status handle_hello(mgls_t *s, const char *base)
{
	mgls_workspace_t *ws = s->ws;
	const mgls_jfield_t *f = ws->f;
	const size_t n = ws->nfields;
	const char *auth = mgj_str(base, f, n, "pairing_auth");
	const char *policy = mgj_str(base, f, n, "pairing_policy");
	const char *mpub = mgj_str(base, f, n, "mobile_pub");
	const char *mnonce = mgj_str(base, f, n, "mobile_nonce");
	struct hello_scratch *h = scratch(s);
	const mgls_jfield_t *ver = mgj_get(base, f, n, "version");
	size_t dl;
	int err;
	mgj_w w;
	int len;

	/* The integer 5: in JSON that can only be written "5". */
	if (ver == NULL || ver->type != MGJ_NUMBER || ver->len != 1 || base[ver->val] != '5' ||
	    !nonempty(auth) || strcmp(auth, AUTH) != 0 || !nonempty(policy) ||
	    strcmp(policy, POLICY) != 0 || !nonempty(mpub) || !nonempty(mnonce)) {
		return ST_INVALID_HELLO;
	}
	/* The client's key and nonce, before anything changes: a malformed hello leaves the session that runs as it
	 * is (dispatch() ends a prompt it tried to take over). */
	if ((size_t)((const char *)h - (const char *)ws->rx) < ws->rx_len) {
		return ST_INVALID_HELLO; /* the message reaches into the scratch */
	}
	memset(h, 0, sizeof(*h));
	if (mgls_b64_decode(mpub, strlen(mpub), h->mpub, sizeof(h->mpub), &dl) ||
	    dl != sizeof(h->mpub) || h->mpub[0] != 0x04 ||
	    mgls_b64_decode(mnonce, strlen(mnonce), h->mnonce, sizeof(h->mnonce), &dl) ||
	    dl != sizeof(h->mnonce)) {
		mglsc_wipe(h, sizeof(*h));
		return ST_INVALID_HELLO;
	}
	/* A config mgls_init() refused, or no set_up hook (the core can't tell a set-up gadget from a new one): it fails
	 * closed, the hello refused before anything changes (no prompt, no confirm; the session that runs goes on
	 * untouched). */
	if (s->refused || s->cfg->set_up == NULL) {
		LOG(s, "config without its hooks: hello refused");
		return ST_UNAVAILABLE;
	}
	/* A new re-setup prompt (none is up: a hello on the prompt's connection while one is up takes it over, never
	 * refused), before anything changes: a one-key board outside setup mode, then the prompt's rate. */
	if (!(s->flags & F_SHOWN) && s->cfg->set_up(s->cfg->ctx)) {
		if (s->cfg->setup_mode != NULL && !s->cfg->setup_mode(s->cfg->ctx)) {
			LOG(s, "re-setup outside setup mode: error_pairing_unavailable");
			return ST_UNAVAILABLE;
		}
		if (s->cfg->may_prompt != NULL &&
		    !s->cfg->may_prompt(s->cfg->ctx, s->prompted ? s->now - s->prompt_at : MGLS_NO_PROMPT)) {
			LOG(s, "re-setup prompt again too soon: error_operation_in_progress");
			return ST_IN_PROGRESS;
		}
	}
	/* Every hello starts over (on the prompt's connection it takes the prompt over). A set-up gadget takes it too
	 * (contract 7). From here a failure (keygen, random, the key agreement, the transcript) ends the session and any
	 * prompt it took over (reset_session(): session_ended, no press kept). */
	advance_gen(s);
	wipe_session(s, true);
#if defined(MGLS_TEST_HOOKS)
	if (test_eph_set) {
		test_eph_set = false;
		err = mglsc_p256_import(&h->key, test_eph, h->dpub);
	} else
#endif
	{
		err = mglsc_p256_keygen(&h->key, h->dpub);
	}
	err = err ? err
		  : (s->cfg->random ? s->cfg->random(s->cfg->ctx, h->dnonce, sizeof(h->dnonce))
				    : -1);
	/* The client's point is checked by the key agreement. */
	err = err ? err : mglsc_p256_ecdh(&h->key, h->mpub, h->ss);
	mglsc_p256_free(&h->key);
	if (!err && (mgls_b64_encode(h->mpub, 65, h->mpub_b64, sizeof(h->mpub_b64)) < 0 ||
		     mgls_b64_encode(h->dpub, 65, h->dpub_b64, sizeof(h->dpub_b64)) < 0 ||
		     mgls_b64_encode(h->mnonce, 16, h->mn_b64, sizeof(h->mn_b64)) < 0 ||
		     mgls_b64_encode(h->dnonce, 16, h->dn_b64, sizeof(h->dn_b64)) < 0)) {
		err = -1;
	}
	const char *dyn[V_DYN_COUNT];

	hello_values(s, h, dyn);
	if (!err) {
		mgj_begin(&w, h->transcript, sizeof(h->transcript));
		mgj_cstr(&w, "hatch-link-pairing-v5");
		emit(&w, TRANSCRIPT, sizeof(TRANSCRIPT) / sizeof(TRANSCRIPT[0]), dyn, false);
		len = mgj_finish(&w);
	} else {
		len = -1;
	}
	err = len < 0 || mglsc_sha256((const uint8_t *)h->transcript, (size_t)len, h->th) ||
	      mgls_b64_encode(h->th, 32, h->th_b64, sizeof(h->th_b64)) < 0 || derive_keys(s, h);
	if (err) {
		mglsc_wipe(h, sizeof(*h));
		reset_session(s);
		return ST_INVALID_HELLO;
	}

	mgj_begin(&w, (char *)&ws->tx[CHUNK_HDR], MGLS_TX_CAP - CHUNK_HDR);
	emit(&w, PAIRING_READY, sizeof(PAIRING_READY) / sizeof(PAIRING_READY[0]), dyn, true);
	len = mgj_end(&w);
	mglsc_wipe(h, sizeof(*h));
	if (len < 0) {
		reset_session(s);
		return ST_INVALID_HELLO;
	}
	ws->rx_ctr = 0;
	ws->tx_ctr = 0;
	s->phase = PH_WAIT_FINISHED;
	set_deadline(s, MGLS_CLIENT_FINISHED_MS);
	s->flags |= F_PT_BLOCKED;
	if (s->cfg->set_up(s->cfg->ctx)) {
		s->flags |= F_RESETUP;
	}
	(void)queue_msg(s, (size_t)len, AFTER_NONE);
	LOG(s, "pairing_ready sent");
	event(s, MGLS_EV_SESSION_STARTED);
	return ST_IDLE;
}

/* Decrypts a pairing_encrypted record in place: the ciphertext's base64
 * text becomes the ciphertext, the tag goes after it, then the plaintext,
 * NUL-terminated. */
static enum status decrypt_record(mgls_t *s, char *base, char **out, size_t *out_len)
{
	mgls_workspace_t *ws = s->ws;
	const mgls_jfield_t *f = ws->f;
	const size_t n = ws->nfields;
	const char *sid = mgj_str(base, f, n, "session_id");
	const char *ctr_s = mgj_str(base, f, n, "counter");
	const char *ct_s = mgj_str(base, f, n, "ciphertext");
	const char *tag_s = mgj_str(base, f, n, "tag");
	uint8_t tag[16], nonce[12];
	char aad[80];
	uint32_t ctr;
	size_t cl, tl;
	int al;

	if (!keys_available(s) || !nonempty(sid) || strcmp(sid, ws->sid_b64) != 0 ||
	    !nonempty(ctr_s) || !mgls_parse_u32(ctr_s, &ctr) || ctr != ws->rx_ctr ||
	    !nonempty(ct_s) || !nonempty(tag_s)) {
		reset_session(s);
		return ST_DECRYPT;
	}
	uint8_t *ct = (uint8_t *)(uintptr_t)ct_s;
	size_t room = (size_t)(&ws->rx[RX_MSG_MAX] - ct);

	if (mgls_b64_decode(tag_s, strlen(tag_s), tag, sizeof(tag), &tl) || tl != 16 ||
	    mgls_b64_decode(ct_s, strlen(ct_s), ct, room, &cl)) {
		reset_session(s);
		return ST_DECRYPT;
	}
	memcpy(&ct[cl], tag, 16);
	al = record_aad(s, M2D, ws->rx_ctr, aad, sizeof(aad));
	record_nonce(M2D, ws->rx_ctr, nonce);
	if (al < 0 || mglsc_gcm_open(ws->rx_key, nonce, (const uint8_t *)aad, (size_t)al, ct, cl)) {
		mglsc_wipe(ct, cl + 16);
		reset_session(s);
		return ST_DECRYPT;
	}
	mglsc_wipe(&ct[cl], 16);
	ct[cl] = '\0';
	ws->rx_ctr++;
	*out = (char *)ct;
	*out_len = cl;
	return ST_IDLE;
}

static void handle_client_finished(mgls_t *s, const char *base)
{
	mgls_workspace_t *ws = s->ws;
	const char *a = mgj_str(base, ws->f, ws->nfields, "action");
	bool exact = ws->nfields == 1 && a != NULL && strcmp(a, "pairing_client_finished") == 0;

	if (!exact || s->phase != PH_WAIT_FINISHED || ws->rx_ctr != 1) {
		(void)send_status(s, ST_DECRYPT, 0, AFTER_NONE);
		reset_session(s);
		disconnect_later(s, 300, 0);
		return;
	}
	advance_gen(s);
	s->phase = PH_CONFIRM_REQUIRED;
	s->flags &= (uint8_t)~F_DEADLINE; /* the prompt has no timer */
	/* The prompt goes out before the press is armed (at the next poll, finish_after()), so an early press
	 * can't make pairing_confirmed overtake it. */
	(void)send_status(s, ST_CONFIRM_REQUIRED, s->gen, AFTER_ARM);
}

static void confirm(mgls_t *s)
{
	advance_gen(s);
	s->phase = PH_READY;
	s->flags &= (uint8_t)~(F_ARMED | F_SHOWN);
	set_deadline(s, MGLS_CONFIRMED_IDLE_MS);
	LOG(s, "confirmed");
	event(s, MGLS_EV_CONFIRMED);
	(void)send_status(s, ST_PAIRING_CONFIRMED, s->gen, AFTER_DISC_IF_FAIL);
}

/* ---- Provisioning: token-only (wifi "none") ------------------------------ */

static bool absent_or_empty(const char *base, const mgls_jfield_t *f, size_t n, const char *key)
{
	const mgls_jfield_t *fl = mgj_get(base, f, n, key);

	return fl == NULL || fl->type == MGJ_NULL || (fl->type == MGJ_STRING && fl->len == 0);
}

/* A re-setup never takes an ota_url (it is never stored or followed): logged once, in every build. */
static void resetup_ota_url(mgls_t *s, const char *base)
{
	if ((s->flags & F_RESETUP) && !absent_or_empty(base, s->ws->f, s->ws->nfields, "ota_url")) {
		LOG(s, "re-setup: ota_url ignored");
	}
}

static void provision_failed(mgls_t *s, enum status st, uint8_t gen)
{
	LOG1(s, "provisioning failed (status %ld)", st);
	s->disc_gen = gen;
	(void)send_status(s, st, gen, AFTER_DISC500);
}

/* {"node_id":"","access_token":"","refresh_token":"","access_expires_at":N}: the device session's credentials
 * document around node_id and the pair, with room for the longest access_expires_at (20 digits) a board that keeps
 * one adds to it. */
#define CREDS_DOC_FIXED \
	(sizeof("{\"node_id\":\"\",\"access_token\":\"\",\"refresh_token\":\"\",\"access_expires_at\":}") - 1u + 20u)

/* Printable ASCII without a quote or backslash: the only token bytes the device session hands back. */
static bool token_chars(const char *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)t[i];

		if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') {
			return false;
		}
	}
	return true;
}

/* Each token within its limit, and a pair the device session can hand back: its credentials document (with any
 * access_expires_at) within MGLS_CREDENTIALS_MAX, every byte a token character. False: provision_v2 answers
 * error_storage. */
static bool pair_ok(mgls_t *s, const char *base, const mgls_jfield_t *at, const mgls_jfield_t *rt)
{
	LOG2(s, "provision_v2: access %ld B, refresh %ld B", at->len, rt->len);
	if (at->len > MGLS_ACCESS_TOKEN_MAX || rt->len > MGLS_REFRESH_TOKEN_MAX) {
		LOG2(s, "token too long (max %ld / %ld B)",
		     MGLS_ACCESS_TOKEN_MAX, MGLS_REFRESH_TOKEN_MAX);
		return false;
	}
	if (CREDS_DOC_FIXED + strlen(id_node(s)) + at->len + rt->len > MGLS_CREDENTIALS_MAX) {
		LOG1(s, "pair too long for the session's credentials (max %ld B with node_id)", MGLS_CREDENTIALS_MAX);
		return false;
	}
	if (!token_chars(base + at->val, at->len) || !token_chars(base + rt->val, rt->len)) {
		LOG(s, "a token byte the session can't hand back");
		return false;
	}
	return true;
}

static void handle_provision(mgls_t *s, const char *base)
{
	mgls_workspace_t *ws = s->ws;
	const mgls_jfield_t *f = ws->f;
	const size_t n = ws->nfields;
	const mgls_jfield_t *at = mgj_get(base, f, n, "access_token");
	const mgls_jfield_t *rt = mgj_get(base, f, n, "refresh_token");
	const char *tt = mgj_str(base, f, n, "token_type");
	mgls_credentials_t cred;
	uint8_t k[32];
	uint8_t gen;
	int err;

	if (at == NULL || at->type != MGJ_STRING || at->len == 0 || rt == NULL ||
	    rt->type != MGJ_STRING || rt->len == 0 || tt == NULL || strcmp(tt, "device") != 0) {
		(void)send_status(s, ST_MISSING_CREDENTIALS, 0, AFTER_NONE);
		return;
	}
	/* wifi "none": no Wi-Fi to join; nothing is stored. */
	if (!absent_or_empty(base, f, n, "ssid") || !absent_or_empty(base, f, n, "password")) {
		(void)send_status(s, ST_WIFI_UNSUPPORTED, 0, AFTER_NONE);
		return;
	}
	/* link_pairing_mark_provisioning_active() */
	if (s->phase == PH_READY) {
		advance_gen(s);
		s->phase = PH_PROVISIONING;
		set_deadline(s, MGLS_PROVISIONING_MS);
	}
	if (s->phase != PH_PROVISIONING) {
		s->disc_gen = s->gen;
		(void)send_status(s, ST_IN_PROGRESS, 0, AFTER_DISC0);
		return;
	}
	gen = s->gen;
	if (!pair_ok(s, base, at, rt)) {
		provision_failed(s, ST_STORAGE, gen);
		return;
	}
	resetup_ota_url(s, base); /* token-only takes no ota_url anyway */
	const char *node = id_node(s);

	/* K (mgcommands.h, Token proof): the token proof's own derivation. */
	err = mgtp_derive_key(base + at->val, at->len, node, strlen(node), k);
	if (!err) {
		cred.access_token = base + at->val;
		cred.access_len = at->len;
		cred.refresh_token = base + rt->val;
		cred.refresh_len = rt->len;
		cred.k = k;
		cred.flags = MGLS_CRED_SETUP_COMPLETE | MGLS_CRED_WIFI_SKIPPED;
		err = s->cfg->commit ? s->cfg->commit(s->cfg->ctx, &cred) : -1;
	}
	mglsc_wipe(k, sizeof(k));
	if (err) {
		LOG1(s, "commit failed (%ld): old set kept", err);
		event(s, MGLS_EV_STORAGE_FAILED);
		provision_failed(s, ST_STORAGE, gen);
		return;
	}
	/* Durable: only now auth_ok. The session is consumed (PH_DONE: a
	 * further provision_v2 gets error_pairing_confirm_required), and a
	 * disconnect an earlier failed attempt on it asked for no longer
	 * applies (new gen). */
	s->phase = PH_DONE;
	s->flags &= (uint8_t)~F_DEADLINE;
	advance_gen(s);
	LOG(s, "setup stored");
	event(s, MGLS_EV_PROVISIONED);
	(void)send_status(s, ST_AUTH_OK, s->gen, AFTER_NONE);
}

static void handle_unpair(mgls_t *s)
{
	if (s->cfg->erase == NULL || s->cfg->erase(s->cfg->ctx) != 0) {
		(void)send_status(s, ST_STORAGE, 0, AFTER_NONE);
		return;
	}
	LOG(s, "unpaired");
	event(s, MGLS_EV_UNPAIRED);
	s->disc_gen = 0;
	(void)send_status(s, ST_UNPAIRED, 0, AFTER_DISC300);
	reset_session(s);
}

/* ---- Dispatch (ble_server.c dispatch_command_ex) ------------------------- */

static bool sensitive(const char *a)
{
	static const char *const list[] = {"provision", "provision_v2", "wifi_scan", "ota",
					   "device.ota", "unpair",       "set_wifi",  "set_auth"};

	for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
		if (strcmp(a, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void send_device_info(mgls_t *s)
{
	const char *dyn[V_DYN_COUNT];
	size_t nf = sizeof(DEVICE_INFO) / sizeof(DEVICE_INFO[0]);
	mgj_w w;
	int n;

	memset(dyn, 0, sizeof(dyn));
	dyn[V_DEVICE] = id_device(s);
	dyn[V_NODE] = id_node(s);
	dyn[V_MAC] = id_mac(s);
	dyn[V_FW] = fw_version(s);
	mgj_begin(&w, (char *)&s->ws->tx[CHUNK_HDR], MGLS_TX_CAP - CHUNK_HDR);
	emit(&w, DEVICE_INFO, s->cfg->mgcommands ? nf : nf - 1, dyn, true);
	n = mgj_end(&w);
	if (n > 0) {
		(void)queue_msg(s, (size_t)n, AFTER_NONE);
	}
}

static void dispatch(mgls_t *s, char *base, size_t len, bool decrypted)
{
	mgls_workspace_t *ws = s->ws;
	const char *act;

	if (mgj_parse(base, len, ws->f, MGLS_JSON_MAX_FIELDS, &ws->nfields)) {
		LOG1(s, "RX invalid JSON (%ld B)", len);
		(void)send_status(s, ST_INVALID_COMMAND, 0, AFTER_NONE);
		return;
	}
	act = mgj_str(base, ws->f, ws->nfields, "action");
	if (act == NULL) {
		act = "";
	}

	if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
		enum status err = handle_hello(s, base);

		if (err == ST_IN_PROGRESS || err == ST_UNAVAILABLE) {
			send_hello_refusal(s, err);
		} else if (err != ST_IDLE) {
			(void)send_status(s, err, 0, AFTER_NONE);
			if (s->flags & F_SHOWN) {
				/* A takeover refused before it changed anything: the prompt it tried to take over ends
				 * (session_ended), so no press is kept for a hello that never ran. */
				LOG(s, "invalid hello on the prompt's connection: prompt ended");
				reset_session(s);
			}
		}
	} else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {
		char *pt = NULL;
		size_t pl = 0;
		enum status err = decrypt_record(s, base, &pt, &pl);

		if (err != ST_IDLE) {
			(void)send_status(s, err, 0, AFTER_NONE);
			disconnect_later(s, 300, 0);
		} else {
			dispatch(s, pt, pl, true);
		}
	} else if (!decrypted && (s->flags & F_PT_BLOCKED)) {
		LOG(s, "plaintext ignored");
	} else if (!decrypted && sensitive(act)) {
		(void)send_status(s, ST_ENC_REQUIRED, 0, AFTER_NONE);
	} else if (decrypted && strcmp(act, "pairing_client_finished") == 0) {
		handle_client_finished(s, base);
	} else if (decrypted && sensitive(act) && !confirmed(s)) {
		(void)send_status(s, ST_CONFIRM_REQUIRED_ERR, 0, AFTER_NONE);
	} else if (decrypted && strcmp(act, "wifi_scan") == 0) {
		/* No Wi-Fi radio: an empty list (apps skip the scan for wifi "none"). */
		send_encrypted_json(s, "{\"type\":\"wifi_scan_result\",\"networks\":[]}");
	} else if (decrypted && strcmp(act, "unpair") == 0) {
		handle_unpair(s);
	} else if (decrypted && strcmp(act, "provision_v2") != 0) {
		(void)send_status(s, ST_UNKNOWN_ACTION, 0, AFTER_NONE);
	} else if (decrypted) {
		handle_provision(s, base);
	} else if (strcmp(act, "get_device_info") == 0) {
		send_device_info(s);
	} else if (strcmp(act, "get_version") == 0) {
		/* The reference answers "version:<v>", a plaintext status outside
		 * the allowed four, so it never goes out. */
		LOG(s, "get_version ignored");
	} else {
		(void)send_status(s, ST_UNKNOWN_ACTION, 0, AFTER_NONE);
	}
}

static void process_rx(mgls_t *s)
{
	mgls_workspace_t *ws = s->ws;

	if (ws->rx_state == RXS_OVERFLOW) {
		ws->rx_state = RXS_IDLE;
		mglsc_wipe(ws->rx, sizeof(ws->rx));
		LOG1(s, "RX over %ld B refused", RX_MSG_MAX);
		if (confirmed(s)) {
			/* A provision_v2 too large to store. */
			provision_failed(s, ST_STORAGE, s->gen);
		} else if (keys_available(s)) {
			(void)send_status(s, ST_INVALID_COMMAND, 0, AFTER_NONE);
			reset_session(s);
			disconnect_later(s, 300, 0);
		}
		return;
	}
	ws->rx_state = RXS_IDLE;
	dispatch(s, (char *)ws->rx, ws->rx_len, false);
	if (s->ws != ws) {
		return; /* a callback ended the connection (mgls_disconnected()): the workspace went back, wiped */
	}
	ws->rx_len = 0;
	mglsc_wipe(ws->f, sizeof(ws->f));
	mglsc_wipe(ws->rx, sizeof(ws->rx));
}

/* ---- Timeouts ------------------------------------------------------------ */

static void check_timers(mgls_t *s)
{
	if ((s->flags & F_DISC) && due(s->now, s->disc_at)) {
		s->flags &= (uint8_t)~F_DISC;
		/* ble_server_disconnect_pairing_session(): only if that session is
		 * still the current one. */
		if (s->disc_gen == 0 || s->disc_gen == s->gen) {
			if (s->disc_gen != 0) {
				reset_session(s);
			}
			if ((s->flags & F_CONNECTED) && s->cfg->disconnect) {
				LOG(s, "disconnecting");
				s->cfg->disconnect(s->cfg->ctx);
			}
		}
	}
	if (!(s->flags & F_DEADLINE) || !due(s->now, s->deadline)) {
		return;
	}
	s->flags &= (uint8_t)~F_DEADLINE;
	/* From the press on (waiting for it, for the provision, or after a failed commit) a deadline ends with the
	 * sealed status and a drop, so the app never waits on a dead session. What is stored stays as it is. */
	if (s->phase == PH_CONFIRM_REQUIRED || s->phase == PH_READY || s->phase == PH_PROVISIONING ||
	    (s->flags & F_SHOWN)) {
		LOG1(s, "ended (%ld: 0 the prompt dismissed, 1 no provision)",
		     s->phase == PH_CONFIRM_REQUIRED || (s->flags & F_SHOWN) ? 0 : 1);
		event(s, MGLS_EV_CONFIRM_TIMEOUT);
		if (s->ws != NULL && s->ws->tx_kind == TXK_NONE) {
			(void)send_status(s, ST_CONFIRM_TIMEOUT, s->gen, AFTER_DISC100);
			/* The record is sealed: the session can go now. The
			 * disconnect belongs to the next gen, so a new hello in
			 * the meantime keeps its connection. */
			reset_session(s);
			s->disc_gen = s->gen;
		} else {
			reset_session(s);
			disconnect_later(s, 0, 0);
		}
	} else if (s->phase != PH_IDLE) {
		LOG(s, "session timed out");
		clear_session(s);
	}
}

/* ---- Public API ---------------------------------------------------------- */

#if !MGLS_RELEASE
bool mgls_test_default_set_up(void *ctx)
{
	(void)ctx;
	return false;
}

bool mgls_test_default_may_prompt(void *ctx, uint32_t since_ms)
{
	(void)ctx;
	(void)since_ms;
	return true;
}
#endif

int mgls_init(mgls_t *s, const mgls_config_t *cfg)
{
	memset(s, 0, sizeof(*s));
	s->cfg = cfg;
	s->mtu = 23;
	s->gen = 1;
	(void)mglsc_init(cfg->random, cfg->ctx);
	if (cfg->bd_addr == NULL && (cfg->node_id == NULL || cfg->mac == NULL ||
				     cfg->device_id == NULL)) {
		LOG(s, "no identity");
	}
	/* Fail closed: without set_up() a re-setup would pass for a first setup (the talk key confirming it); in a
	 * release build without may_prompt() the re-setup prompt would have no rate. */
	if (cfg->set_up == NULL || (MGLS_RELEASE && cfg->may_prompt == NULL)) {
		LOG(s, "config refused: set_up (and in a release build may_prompt) required");
		s->refused = 1;
		return MGLS_E_CONFIG;
	}
	return MGLS_OK;
}

/* The core follows connection conn from now (it connected, or wrote while the one followed had no setup running). */
static void follow(mgls_t *s, uint16_t conn, uint32_t now_ms)
{
	s->now = now_ms;
	detach(s);
	reset_session(s);
	s->conn = conn;
	s->flags = F_CONNECTED;
	s->mtu = 23;
	s->last_status = ST_IDLE;
	s->prompted = 0; /* the prompt's rate is per connection */
}

/* A setup runs on the connection followed: from its pairing_ready until its session ends, a prompt included, and
 * while anything of it still goes out (auth_ok held back by the stack after PH_DONE). */
static bool setup_runs(const mgls_t *s)
{
	return (s->flags & F_SHOWN) || (s->phase != PH_IDLE && s->phase != PH_DONE) ||
	       (s->ws != NULL && s->ws->tx_kind != TXK_NONE);
}

void mgls_conn_connected(mgls_t *s, uint16_t conn, uint32_t now_ms)
{
	if ((s->flags & F_CONNECTED) && conn != s->conn) {
		return; /* another connection: it takes the core only at a write while no setup runs */
	}
	follow(s, conn, now_ms);
}

void mgls_connected(mgls_t *s, uint32_t now_ms)
{
	mgls_conn_connected(s, 0, now_ms);
}

void mgls_conn_disconnected(mgls_t *s, uint16_t conn)
{
	if (conn != s->conn) {
		return;
	}
	reset_session(s);
	detach(s);
	s->flags = 0;
	s->mtu = 23;
	s->prompted = 0;
}

void mgls_disconnected(mgls_t *s)
{
	mgls_conn_disconnected(s, 0);
}

void mgls_conn_mtu_changed(mgls_t *s, uint16_t conn, uint16_t att_mtu)
{
	if (conn == s->conn) {
		s->mtu = att_mtu < 23 ? 23 : att_mtu;
	}
}

void mgls_mtu_changed(mgls_t *s, uint16_t att_mtu)
{
	mgls_conn_mtu_changed(s, 0, att_mtu);
}

uint16_t mgls_conn(const mgls_t *s)
{
	return s->conn;
}

static void rx_followed(mgls_t *s, const uint8_t *data, size_t len);

void mgls_conn_rx(mgls_t *s, uint16_t conn, const uint8_t *data, size_t len)
{
	static const char BUSY[] = "error_operation_in_progress";

	if (data == NULL || len == 0) {
		return;
	}
	if ((s->flags & F_CONNECTED) && conn != s->conn && setup_runs(s)) {
		/* A setup of another connection runs (its prompt never goes to this one): busy, in plaintext, at a
		 * message's first write; nothing changes. */
		if (s->cfg->notify_to != NULL && (data[0] != CHUNK_MAGIC || (len >= CHUNK_HDR && data[1] == 0))) {
			(void)s->cfg->notify_to(s->cfg->ctx, conn, (const uint8_t *)BUSY, sizeof(BUSY) - 1);
		}
		LOG(s, "a write from another connection while a setup runs: error_operation_in_progress");
		return;
	}
	if (!(s->flags & F_CONNECTED) || conn != s->conn) {
		follow(s, conn, s->now);
	}
	rx_followed(s, data, len);
}

void mgls_rx(mgls_t *s, const uint8_t *data, size_t len)
{
	if (!(s->flags & F_CONNECTED) || s->conn != 0) {
		return;
	}
	rx_followed(s, data, len);
}

static void rx_followed(mgls_t *s, const uint8_t *data, size_t len)
{
	mgls_workspace_t *ws;

	if (data == NULL || len == 0) {
		return;
	}
	if (!attach(s)) {
		LOG1(s, "RX %ld B dropped: no workspace", len);
		return;
	}
	ws = s->ws;
	if (ws->rx_state == RXS_COMPLETE || ws->rx_state == RXS_OVERFLOW) {
		LOG1(s, "RX %ld B dropped: busy", len);
		return;
	}
	if (len < CHUNK_HDR || data[0] != CHUNK_MAGIC) {
		size_t n = len < SINGLE_MAX ? len : SINGLE_MAX;

		memcpy(ws->rx, data, n);
		ws->rx_len = (uint16_t)n;
		ws->rx_total = 0;
		ws->rx_state = RXS_COMPLETE;
		return;
	}
	uint8_t idx = data[1], total = data[2];
	size_t flen = len - CHUNK_HDR;

	if (total == 0) {
		ws->rx_state = RXS_IDLE;
		ws->rx_total = 0;
		return;
	}
	if (idx == 0 || ws->rx_total != total || ws->rx_state != RXS_ASSEMBLING) {
		ws->rx_state = RXS_ASSEMBLING;
		ws->rx_total = total;
		ws->rx_next = 0;
		ws->rx_len = 0;
	}
	if (idx != ws->rx_next || idx >= ws->rx_total) {
		ws->rx_state = RXS_IDLE;
		ws->rx_total = 0;
		return;
	}
	if ((size_t)ws->rx_len + flen > RX_MSG_MAX) {
		ws->rx_state = RXS_OVERFLOW;
		ws->rx_total = 0;
		return;
	}
	memcpy(&ws->rx[ws->rx_len], &data[CHUNK_HDR], flen);
	ws->rx_len = (uint16_t)(ws->rx_len + flen);
	ws->rx_next++;
	if (ws->rx_next == ws->rx_total) {
		ws->rx_total = 0;
		ws->rx_state = RXS_COMPLETE;
	}
}

void mgls_poll(mgls_t *s, uint32_t now_ms)
{
	mgls_workspace_t *ws = s->ws;
	/* confirm_required went out during an earlier call: this poll's time is the first the core gets that is
	 * certainly after it left, so the prompt is up from now. */
	bool arm_due = ws != NULL && ws->tx_kind == TXK_NONE && ws->tx_after == AFTER_ARM;

	s->now = now_ms;
	tx_pump(s);
	check_timers(s);
	ws = s->ws;
	if (arm_due && ws != NULL && ws->tx_kind == TXK_NONE && ws->tx_after == AFTER_ARM) {
		ws->tx_after = AFTER_NONE;
		arm(s);
	}
	if (ws != NULL && ws->tx_kind == TXK_NONE && (s->flags & F_PRESS)) {
		if (s->phase == PH_CONFIRM_REQUIRED && (s->flags & F_ARMED) && (s->flags & F_CONNECTED)) {
			s->flags &= (uint8_t)~F_PRESS;
			confirm(s);
		} else if (!(s->flags & F_SHOWN)) {
			s->flags &= (uint8_t)~F_PRESS; /* nothing to confirm; while a takeover's hello runs it waits */
		}
	}
	ws = s->ws;
	/* Not while an arm waits for the next poll: an answer queued now would overwrite its mark (tx_after). */
	if (ws != NULL && ws->tx_kind == TXK_NONE && ws->tx_after != AFTER_ARM &&
	    (ws->rx_state == RXS_COMPLETE || ws->rx_state == RXS_OVERFLOW)) {
		process_rx(s);
	}
	tx_pump(s);
	ws = s->ws;
	if (ws != NULL && s->phase == PH_IDLE && ws->tx_kind == TXK_NONE &&
	    (ws->rx_state == RXS_IDLE)) {
		detach(s);
	}
}

bool mgls_idle(const mgls_t *s)
{
	const mgls_workspace_t *ws = s->ws;

	if (s->flags & (F_DISC | F_DEADLINE | F_PRESS)) {
		return false;
	}
	return ws == NULL || (ws->tx_kind == TXK_NONE && ws->tx_after != AFTER_ARM && /* an arm needs the next poll */
			      (ws->rx_state == RXS_IDLE || ws->rx_state == RXS_ASSEMBLING));
}

enum mgls_press mgls_press(mgls_t *s)
{
	return mgls_press_key(s, MGLS_KEY_TALK);
}

/* The gadget holds a set (set_up()), so a prompt that is up is a re-setup's whatever its session's phase. No hook:
 * taken as set up (fail closed; no hello is taken then anyway). */
static bool holds_set(const mgls_t *s)
{
	return s->cfg->set_up == NULL || s->cfg->set_up(s->cfg->ctx);
}

enum mgls_press mgls_press_key(mgls_t *s, enum mgls_key key)
{
	if ((s->flags & F_SHOWN) && key != MGLS_KEY_OTHER && holds_set(s)) {
		/* A re-setup is never confirmed by the talk key; it dismisses the prompt (no talk turn), so whoever raised
		 * it never gets the owner's next talk press. */
		return mgls_dismiss(s) ? MGLS_PRESS_DISMISSED : MGLS_PRESS_IGNORED;
	}
	if (s->flags & F_SHOWN) {
		s->flags |= F_PRESS; /* one press confirms a first setup, any key; after a takeover, the newest hello */
		return MGLS_PRESS_CONFIRMING;
	}
	if (s->phase != PH_CONFIRM_REQUIRED) {
		return MGLS_PRESS_NONE;
	}
	LOG(s, "press ignored: the prompt isn't up yet");
	return MGLS_PRESS_IGNORED;
}

bool mgls_dismiss(mgls_t *s)
{
	if (!(s->flags & F_SHOWN)) {
		return false;
	}
	if ((s->flags & F_PRESS) && s->phase == PH_CONFIRM_REQUIRED && (s->flags & F_ARMED)) {
		return false; /* the press came first: it confirms at the next poll */
	}
	if (holds_set(s)) {
		s->prompt_at = s->now; /* may_prompt(): a new prompt on this connection within its rate is refused */
		s->prompted = 1;
	}
	LOG(s, "prompt dismissed");
	s->flags &= (uint8_t)~F_PRESS; /* a press kept for a takeover's hello goes with the prompt */
	set_deadline(s, 0); /* the next mgls_poll() ends it, in any phase: the sealed status (if it has keys), a drop */
	return true;
}

bool mgls_confirm_gesture(mgls_t *s)
{
	if (!mgls_confirm_armed(s)) {
		return false;
	}
	s->flags |= F_PRESS;
	return true;
}

#if defined(MGLS_ENABLE_SIMULATED_PRESS)
enum mgls_press mgls_simulated_press(mgls_t *s)
{
	LOG(s, "simulated press");
	return mgls_press_key(s, MGLS_KEY_OTHER); /* the bench's confirm, never the talk key's dismiss */
}
#endif

bool mgls_confirm_armed(const mgls_t *s)
{
	return (s->flags & F_SHOWN) != 0;
}

bool mgls_session_active(const mgls_t *s)
{
	return s->phase != PH_IDLE && s->phase != PH_DONE;
}

size_t mgls_read_status(const mgls_t *s, char *out, size_t cap)
{
	const char *st = STATUS[s->last_status];
	size_t n = strlen(st);

	if (n > cap) {
		n = cap;
	}
	memcpy(out, st, n);
	return n;
}

void mgls_abort(mgls_t *s)
{
	reset_session(s);
	detach(s);
	s->flags &= (uint8_t)~(F_DISC | F_PRESS);
}

uint8_t mgls_adv_flags(bool set_up)
{
	return (uint8_t)(MGLS_ADV_FLAG_GADGET | (set_up ? MGLS_ADV_FLAG_SET_UP : 0u));
}

size_t mgls_adv_build(uint8_t *out, size_t cap, bool set_up)
{
	static const uint8_t uuid[16] = {MGLS_SERVICE_UUID_LE};
	size_t n = 0;

	if (cap < 26) {
		return 0;
	}
	out[n++] = 2;
	out[n++] = 0x01; /* Flags */
	out[n++] = 0x06; /* LE General Discoverable, BR/EDR not supported */
	out[n++] = 17;
	out[n++] = 0x07; /* Complete List of 128-bit Service UUIDs */
	memcpy(&out[n], uuid, 16);
	n += 16;
	out[n++] = 4;
	out[n++] = 0xFF; /* Manufacturer Specific Data */
	out[n++] = 0xFF;
	out[n++] = 0xFF; /* company 0xFFFF */
	out[n++] = mgls_adv_flags(set_up);
	return n;
}

size_t mgls_format_name(char *out, size_t cap, const char *model, const uint8_t a[6])
{
	const size_t pl = sizeof(MGLS_NAME_PREFIX) - 1;
	size_t ml = strlen(model);
	size_t n = pl + ml + 1 + 6;

	if (n + 1 > cap) {
		return 0;
	}
	memcpy(out, MGLS_NAME_PREFIX, pl);
	memcpy(&out[pl], model, ml);
	out[pl + ml] = '-';
	for (int i = 0; i < 3; i++) {
		hex_byte(&out[pl + ml + 1 + i * 2], a[3 + i], 1);
	}
	out[n] = '\0';
	return n;
}
