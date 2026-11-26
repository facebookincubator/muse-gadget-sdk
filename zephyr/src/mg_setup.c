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

#include "mg_setup.h"
#include "mg_session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_MG_AUTOMATION) || defined(CONFIG_MG_BENCH_MODE)
#include <zephyr/sys/printk.h>
#endif

#include "mg_app.h"
#include "mg_button.h"
#include "mg_connparam.h"
#include "mg_core.h"
#include "mg_identity.h"
#include "mg_input_policy.h"
#include "mg_json.h"
#include "mg_led.h"
#include "mg_psa.h"
#include "mg_resetup_policy.h"
#include "mg_setup_crypto.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_VM_SESSION)
#include "mg_vm_session.h"
#endif
#include "pairing_transcript.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_audio.h"
#include "mg_queue.h"
#endif

LOG_MODULE_REGISTER(mg_setup, CONFIG_MG_LOG_LEVEL);

/* The offline clips are the owner's: once a re-setup's commit or Forget has
 * marked them (mg_setup_store.h), the queue erases its whole store in the
 * background and the mark goes when every byte is blank. Until then the
 * queue stays closed (mg_core_clips_ready()); a power loss, a failed erase
 * or a firmware update taking the store meanwhile keeps the mark, and the
 * erase starts over at the next boot or when the store comes back. */
#if defined(CONFIG_MG_AUDIO_QUEUE)
static atomic_t wipe_result;

static void clips_wiped_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	int err = (int)atomic_get(&wipe_result);

	if (mg_queue_wiping()) {
		return; /* another owner change started it over */
	}
	if (err == -EAGAIN) {
		LOG_INF("offline clips' erase waits for the store (firmware update)");
		return;
	}
	if (err) {
		LOG_WRN("offline clips not erased yet (%d): at the next boot", err);
		return;
	}
	if (mg_setup_store_clips_due()) {
		(void)mg_setup_store_clips_gone();
		LOG_INF("offline clips erased");
	}
}

static K_WORK_DEFINE(clips_wiped_work, clips_wiped_fn);

static void clips_wiped(int err)
{
	atomic_set(&wipe_result, err);
	k_work_submit_to_queue(mg_app_wq(), &clips_wiped_work);
}
#endif

static void forget_clips(const char *why)
{
#if defined(CONFIG_MG_AUDIO_QUEUE)
	/* A capture still held for the proof is the earlier owner's: discarded
	 * unsent, never saved as a clip after the erase. */
	if (mg_audio_output_held()) {
		mg_audio_gate_fail();
	}
	if (!mg_setup_store_clips_due()) {
		return;
	}
	enum mg_audio_activity a = mg_audio_activity();

	if (a == MG_AUDIO_RECORDING || a == MG_AUDIO_CLIP) {
		mg_audio_stop();
	}
	int err = mg_queue_forget(clips_wiped);

	if (err == -EAGAIN) {
		LOG_INF("offline clips erased once the store is back (%s)", why);
	} else if (err) {
		LOG_WRN("offline clips not erased yet (%s, %d): at the next boot", why, err);
	} else {
		LOG_INF("offline clips going (%s)", why);
	}
#else
	ARG_UNUSED(why);
#endif
}

/* Forget: the clips' mark first, so they go even after a power loss; with
 * no mark stored the setup stays. */
static int erase_setup(void)
{
#if defined(CONFIG_MG_AUDIO_QUEUE)
	int err = mg_setup_store_mark_clips();

	if (err) {
		LOG_ERR("offline clips' mark not stored (%d): the setup stays", err);
		return err;
	}
#endif
	return mg_setup_store_erase();
}

/* Framing (ble_server.c). */
#define CHUNK_MAGIC  0xFE
#define CHUNK_HDR    3
#define MAX_NOTIFY   160
/* An encrypted command's ciphertext is at most 4096 base64 characters
 * (3 KB, link_pairing.c); the envelope around it fits in the rest. */
#define RX_MAX       4352
#define PT_MAX       3072
#define TX_MAX       1024
/* The device encrypts only statuses and an empty scan result. */
#define ENC_PT_MAX   384
#define SINGLE_MAX   248
#define STATUS_MAX   64

#if defined(CONFIG_MG_SDK_TOKEN_SLOT)
/* The SDK token from the image's token slot (src/mg_token_slot.c), which an
 * updater fills in before it signs and sends the image; the slot's marker
 * means none. Defined in another file so nothing here can fold its
 * contents at build time. */
extern const char mg_sdk_token_slot[MG_SDK_TOKEN_SLOT_LEN];
#define SDK_TOKEN_MAX MG_SDK_TOKEN_SLOT_LEN

static const char *sdk_token(void)
{
	const char *p = mg_sdk_token_slot;

	/* No token: empty, the marker still in place (the whole marker, so a
	 * token may start with any letter), or no NUL in the slot (malformed).
	 * The marker is compared in two pieces: the image must hold it exactly
	 * once, in the slot, for the updater to find (AGENTS.md, SDK token
	 * slot), so its whole text can't appear here. */
	BUILD_ASSERT(sizeof(MG_SDK_TOKEN_SLOT_MARKER) - 1 == 7 + 14, "marker pieces");
	if (p[0] == '\0' ||
	    (memcmp(p, "MG-SDK-", 7) == 0 && memcmp(p + 7, "TOKEN-SLOT-v1:", 14) == 0) ||
	    memchr(p, '\0', MG_SDK_TOKEN_SLOT_LEN) == NULL) {
		return "";
	}
	return p;
}
#elif defined(CONFIG_MG_SDK_TOKEN_BOARD)
/* The board's loader holds it (mg_app.h, mg_board_sdk_token()), under the
 * slot's rules: at most MG_SDK_TOKEN_SLOT_LEN - 1 characters and a NUL, else
 * no token (never read past that bound). */
#define SDK_TOKEN_MAX MG_SDK_TOKEN_SLOT_LEN

static const char *sdk_token(void)
{
	const char *t = mg_board_sdk_token();

	return t && strnlen(t, SDK_TOKEN_MAX) < SDK_TOKEN_MAX ? t : "";
}
#else
#define SDK_TOKEN_MAX sizeof(CONFIG_MG_SDK_TOKEN)

static const char *sdk_token(void)
{
	return CONFIG_MG_SDK_TOKEN;
}
#endif
#define SDK_TOKEN sdk_token()

enum phase {
	PH_IDLE,
	PH_WAIT_FINISHED,
	PH_CONFIRM_REQUIRED,
	PH_READY,
	PH_PROVISIONING,
	PH_DONE, /* provisioned on this session: auth_ok sent */
};

/* get_device_info's device_key_auth (CONFIG_MG_SETUP_DEVICE_KEY_AUTH; the
 * automation console can change it for a test). */
static int device_key_auth = IS_ENABLED(CONFIG_MG_SETUP_DEVICE_KEY_AUTH) ? 1 : 0;

static struct {
	enum phase phase;
	uint32_t gen; /* changes with every phase and reset (link_pairing.c) */
	bool armed;   /* confirm_required went out: one press confirms */
	int64_t deadline;
	uint64_t rx_ctr, tx_ctr;
	struct mg_setup_keys k;
	bool plaintext_blocked;
	bool connected;
} s;

/* Reassembly of chunked writes (one message at a time). */
static struct {
	size_t len;
	uint8_t total, next, count;
	bool active;
} rx;

/* + the tag, which goes after the ciphertext decoded in place. */
static char rxbuf[RX_MAX + 1 + MG_SETUP_TAG_LEN];
static char single[SINGLE_MAX + 1 + MG_SETUP_TAG_LEN];
static uint8_t cbuf[ENC_PT_MAX + MG_SETUP_TAG_LEN]; /* a record being sealed */
static char txbuf[TX_MAX];
static char jbuf[192 + SDK_TOKEN_MAX];

static char last_status[STATUS_MAX] = "idle";
static K_MUTEX_DEFINE(status_lock);

static struct k_work_delayable phase_work;      /* client_finished / confirm / confirmed-idle timeouts */
static uint32_t phase_work_gen;
static void start_phase_timer(int ms);
static struct k_work_delayable disconnect_work; /* a disconnect a little later */
static uint32_t disconnect_gen;                 /* 0: unconditional */

#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
/* Tests only: a fixed ephemeral key and nonce, never in firmware. */
static bool test_eph;
static uint8_t test_priv[32];
static uint8_t test_nonce[MG_SETUP_NONCE_LEN];

void mg_setup_test_set_ephemeral(const uint8_t priv[32], const uint8_t nonce[16])
{
	memcpy(test_priv, priv, 32);
	memcpy(test_nonce, nonce, 16);
	test_eph = true;
}
#endif

/* ------------------------------------------------------------------------
 * Session (link_pairing.c)
 * ---------------------------------------------------------------------- */

static void advance_gen(void)
{
	if (++s.gen == 0) {
		++s.gen; /* zero means "no session" */
	}
}

/* The re-setup prompt's rate (mg_resetup_policy.h): prompt_conn, the
 * connection the prompt is up on (0: none); dismissed_ms, when it was last
 * dismissed; takeover, a hello took the prompt over and its handshake is
 * under way (the light and the button stay the prompt's). */
static uint32_t prompt_conn;
static uint32_t dismissed_conn;
static int64_t dismissed_ms;
static bool takeover;

static void clear_session(void)
{
	mg_psa_wipe(&s.k, sizeof(s.k));
	s.deadline = 0;
	s.rx_ctr = 0;
	s.tx_ctr = 0;
	s.phase = PH_IDLE;
	mg_connparam_hold(MG_CONNPARAM_HOLD_SETUP, false); /* setup over */
	if (s.armed || takeover) {
		mg_led_set(MG_LED_PAIR_PENDING, false);
	}
	s.armed = false;
	takeover = false;
}

static void reset_session(void)
{
	advance_gen();
	clear_session();
	(void)k_work_cancel_delayable(&phase_work);
}

static bool expired(void)
{
	if (s.phase == PH_IDLE || s.deadline == 0 || k_uptime_get() <= s.deadline) {
		return false;
	}
	LOG_WRN("setup session timed out");
	clear_session();
	return true;
}

static bool keys_available(void)
{
	return s.phase != PH_IDLE;
}

static bool confirmed(void)
{
	return s.phase == PH_READY || s.phase == PH_PROVISIONING;
}

static void set_deadline(int ms)
{
	s.deadline = k_uptime_get() + ms;
}

/* ------------------------------------------------------------------------
 * Sending (ble_server.c)
 * ---------------------------------------------------------------------- */

static bool notify(const uint8_t *buf, size_t len)
{
	if (!s.connected) {
		return false;
	}
	int err = mg_transport_notify(MG_ATT_SETUP, buf, len, K_MSEC(500));

	if (err && err != -ENOTCONN) {
		/* Counted with the console's drops, and logged; the
		 * message it belonged to is incomplete (the client times out or
		 * retries). */
		mg_session_count_drop("a Link setup notification");
	}
	return err == 0;
}

static void set_last_status(const char *st, size_t n)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	n = MIN(n, sizeof(last_status) - 1);
	memcpy(last_status, st, n);
	last_status[n] = '\0';
	k_mutex_unlock(&status_lock);
}

size_t mg_setup_read_status(char *out, size_t cap)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	size_t n = MIN(strlen(last_status), cap);

	memcpy(out, last_status, n);
	k_mutex_unlock(&status_lock);
	return n;
}

/* [0xFE, index, total, fragment], each at most 160 bytes and MTU - 3. */
static bool send_chunked(const char *data, size_t total_len)
{
	uint8_t pkt[MAX_NOTIFY];
	size_t max = mg_transport_att_mtu();

	if (max <= 3) {
		return false;
	}
	max = MIN(max - 3, (size_t)MAX_NOTIFY);
	if (max <= CHUNK_HDR) {
		return false;
	}
	size_t usable = max - CHUNK_HDR;
	size_t chunks = MAX((total_len + usable - 1) / usable, (size_t)1);

	if (chunks > 255) {
		LOG_WRN("TX message exceeds the framing limit");
		return false;
	}
	for (size_t i = 0; i < chunks; i++) {
		size_t off = i * usable;
		size_t n = MIN(usable, total_len - off);

		pkt[0] = CHUNK_MAGIC;
		pkt[1] = (uint8_t)i;
		pkt[2] = (uint8_t)chunks;
		memcpy(&pkt[CHUNK_HDR], &data[off], n);
		if (!notify(pkt, n + CHUNK_HDR)) {
			return false;
		}
	}
	return true;
}

static bool plaintext_status_allowed(const char *st)
{
	return strcmp(st, "error_encryption_required") == 0 ||
	       strcmp(st, "error_pairing_invalid_hello") == 0 ||
	       strcmp(st, "error_pairing_unavailable") == 0 ||
	       strcmp(st, "error_pairing_decrypt") == 0;
}

static void u64_dec(uint64_t v, char out[21])
{
	char tmp[21];
	size_t i = sizeof(tmp);

	do {
		tmp[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	memcpy(out, &tmp[i], sizeof(tmp) - i);
	out[sizeof(tmp) - i] = '\0';
}

/* Encrypts plain into a pairing_encrypted envelope in txbuf and sends it.
 * gen 0: the current session; otherwise only that phase's. */
static bool send_encrypted(const char *plain, uint32_t gen)
{
	size_t n = strlen(plain);
	char ctr[21], tag_b64[24];
	struct mg_jw w;
	int len;

	if ((gen != 0 && gen != s.gen) || expired() || !keys_available()) {
		return false;
	}
	if (n > ENC_PT_MAX) {
		return false;
	}
	if (mg_setup_seal(s.k.tx, MG_SETUP_D2M, s.tx_ctr, s.k.session_id_b64,
			  (const uint8_t *)plain, n, cbuf)) {
		return false;
	}
	static char ct_b64[(ENC_PT_MAX + 2) / 3 * 4 + 1];

	if (mg_b64url_encode(cbuf, n, ct_b64, sizeof(ct_b64)) < 0 ||
	    mg_b64url_encode(&cbuf[n], MG_SETUP_TAG_LEN, tag_b64, sizeof(tag_b64)) < 0) {
		return false;
	}
	u64_dec(s.tx_ctr, ctr);
	mg_jw_begin(&w, txbuf, sizeof(txbuf));
	mg_jw_str(&w, "type", "pairing_encrypted");
	mg_jw_str(&w, "session_id", s.k.session_id_b64);
	mg_jw_str(&w, "counter", ctr);
	mg_jw_str(&w, "ciphertext", ct_b64);
	mg_jw_str(&w, "tag", tag_b64);
	len = mg_jw_end(&w);
	mg_psa_wipe(cbuf, n + MG_SETUP_TAG_LEN);
	if (len < 0) {
		return false;
	}
	/* The counter is spent once the record exists, sent or not. */
	s.tx_ctr++;
	return send_chunked(txbuf, (size_t)len);
}

/* ble_server.c send_status(): encrypted while a session has keys; otherwise
 * plaintext, and then only the four errors a client may see before one. */
static bool send_status(const char *st, uint32_t gen)
{
	struct mg_jw w;

	if (keys_available() && !expired() && (gen == 0 || gen == s.gen)) {
		mg_jw_begin(&w, jbuf, sizeof(jbuf));
		mg_jw_str(&w, "type", "status");
		mg_jw_str(&w, "status", st);
		if (SDK_TOKEN[0] != '\0' && strcmp(st, "pairing_confirmed") == 0) {
			mg_jw_str(&w, "sdk_token", SDK_TOKEN);
			LOG_INF("pairing_confirmed carries the SDK token (%u chars)",
				(unsigned int)strlen(SDK_TOKEN));
		} else if (strcmp(st, "pairing_confirmed") == 0) {
			LOG_WRN("pairing_confirmed without an SDK token: the Muse app can't finish "
				"setup (set CONFIG_MG_SDK_TOKEN)");
		}
		bool sent = mg_jw_end(&w) > 0 && send_encrypted(jbuf, gen);

		mg_psa_wipe(jbuf, sizeof(jbuf));
		set_last_status("encrypted_status", 16);
		if (sent) {
			LOG_INF("status: %s", st);
		}
		return sent;
	}
	if (gen != 0 || s.plaintext_blocked || !plaintext_status_allowed(st)) {
		LOG_WRN("plaintext status suppressed: %s", st);
		return false;
	}
	set_last_status(st, strlen(st));
	bool sent = notify((const uint8_t *)st, strlen(st));

	if (sent) {
		LOG_INF("status: %s", st);
	}
	return sent;
}

static uint32_t disconnect_link; /* the link it was asked on (mg_transport_conn_gen()) */

static void disconnect_later(int ms, uint32_t gen)
{
	disconnect_gen = gen;
	disconnect_link = mg_transport_conn_gen();
	k_work_reschedule_for_queue(mg_app_wq(), &disconnect_work, K_MSEC(ms));
}

static void disconnect_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (disconnect_gen != 0) {
		/* ble_server_disconnect_pairing_session(): only if that session is
		 * still the current one. */
		if (disconnect_gen != s.gen) {
			return;
		}
		reset_session();
	}
	if (s.connected && disconnect_link == mg_transport_conn_gen()) {
		/* Only the link it was asked on: a delayed drop never takes a
		 * link formed meanwhile. */
		LOG_INF("disconnecting the setup client");
		mg_transport_disconnect();
	}
}

/* ------------------------------------------------------------------------
 * Handshake
 * ---------------------------------------------------------------------- */

static bool json_nonempty(const struct mg_json_obj *o, const char *key, const char **out)
{
	const char *v = mg_json_str(o, key);

	if (v == NULL || v[0] == '\0') {
		return false;
	}
	*out = v;
	return true;
}

static const char *handle_hello(const struct mg_json_obj *o, int *reply_len)
{
	const struct mg_json_field *ver = mg_json_get(o, "version");
	const char *auth, *policy, *mpub_s, *mnonce_s;
	uint8_t mpub[MG_SETUP_P256_PUB_LEN], dpub[MG_SETUP_P256_PUB_LEN];
	uint8_t mnonce[MG_SETUP_NONCE_LEN], dnonce[MG_SETUP_NONCE_LEN];
	uint8_t ss[32], th[32];
	char mpub_b64[96], dpub_b64[96], mn_b64[32], dn_b64[32], th_b64[48];
	psa_key_id_t key = PSA_KEY_ID_NULL;
	size_t n;
	int err;

	if (ver == NULL || ver->type != MG_JSON_NUMBER || !ver->num_is_int || ver->num != 5 ||
	    !json_nonempty(o, "pairing_auth", &auth) || strcmp(auth, PAIRING_COMMUNITY_AUTH) != 0 ||
	    !json_nonempty(o, "pairing_policy", &policy) ||
	    strcmp(policy, PAIRING_POLICY_BUTTON) != 0 || !json_nonempty(o, "mobile_pub", &mpub_s) ||
	    !json_nonempty(o, "mobile_nonce", &mnonce_s)) {
		return "error_pairing_invalid_hello";
	}
	/* The hello's own key and nonce are checked before anything is wiped: a
	 * hello that fails them changes nothing (a prompt that is up stays its
	 * session's). */
	if (mg_b64url_decode(mpub_s, strlen(mpub_s), mpub, sizeof(mpub), &n) || n != sizeof(mpub) ||
	    mpub[0] != 0x04 ||
	    mg_b64url_decode(mnonce_s, strlen(mnonce_s), mnonce, sizeof(mnonce), &n) ||
	    n != sizeof(mnonce)) {
		return "error_pairing_invalid_hello";
	}
	/* Every hello starts over. */
	reset_session();
#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
	if (test_eph) {
		test_eph = false;
		err = mg_setup_p256_import(&key, test_priv, dpub);
		memcpy(dnonce, test_nonce, sizeof(dnonce));
	} else
#endif
	{
		err = mg_setup_p256_generate(&key, dpub);
		err = err ?: mg_psa_random(dnonce, sizeof(dnonce));
	}
	/* The client's point is checked by the key agreement. */
	err = err ?: mg_setup_ecdh(key, mpub, ss);
	if (key != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(key);
	}
	if (err || mg_b64url_encode(mpub, sizeof(mpub), mpub_b64, sizeof(mpub_b64)) < 0 ||
	    mg_b64url_encode(dpub, sizeof(dpub), dpub_b64, sizeof(dpub_b64)) < 0 ||
	    mg_b64url_encode(mnonce, sizeof(mnonce), mn_b64, sizeof(mn_b64)) < 0 ||
	    mg_b64url_encode(dnonce, sizeof(dnonce), dn_b64, sizeof(dn_b64)) < 0) {
		mg_psa_wipe(ss, sizeof(ss));
		reset_session();
		return "error_pairing_invalid_hello";
	}
	const pairing_transcript_fields_t fields = {
		.device_id = mg_identity_device_id(),
		.node_id = mg_identity_offer_node_id(),
		.mac = mg_identity_mac(),
		.firmware_version = mg_app_version(),
		.mobile_pub = mpub_b64,
		.device_pub = dpub_b64,
		.mobile_nonce = mn_b64,
		.device_nonce = dn_b64,
	};
	char *transcript = pairing_transcript_build(true, 0, PAIRING_POLICY_BUTTON, &fields);

	err = transcript == NULL ||
	      mg_psa_sha256((const uint8_t *)transcript, strlen(transcript), th) ||
	      mg_b64url_encode(th, sizeof(th), th_b64, sizeof(th_b64)) < 0 ||
	      mg_setup_derive(ss, mnonce, dnonce, th, &s.k, NULL);
	free(transcript);
	mg_psa_wipe(ss, sizeof(ss));
	if (err) {
		reset_session();
		return "error_pairing_invalid_hello";
	}

	struct mg_jw w;

	mg_jw_begin(&w, txbuf, sizeof(txbuf));
	mg_jw_str(&w, "type", "pairing_ready");
	mg_jw_int(&w, "version", PAIRING_VERSION);
	mg_jw_str(&w, "device_id", mg_identity_device_id());
	mg_jw_str(&w, "node_id", mg_identity_offer_node_id());
	mg_jw_str(&w, "mac", mg_identity_mac());
	mg_jw_str(&w, "model", PAIRING_MODEL);
	mg_jw_str(&w, "firmware_version", mg_app_version());
	mg_jw_str(&w, "pairing_auth", PAIRING_COMMUNITY_AUTH);
	mg_jw_int(&w, "pairing_auth_epoch", 0);
	mg_jw_str(&w, "pairing_policy", PAIRING_POLICY_BUTTON);
	mg_jw_str(&w, "device_pub", dpub_b64);
	mg_jw_str(&w, "device_nonce", dn_b64);
	mg_jw_str(&w, "transcript_hash", th_b64);
	mg_jw_str(&w, "session_id", s.k.session_id_b64);
	*reply_len = mg_jw_end(&w);
	if (*reply_len < 0) {
		reset_session();
		return "error_pairing_invalid_hello";
	}
	set_deadline(MG_SETUP_CLIENT_FINISHED_MS);
	s.rx_ctr = 0;
	s.tx_ctr = 0;
	s.phase = PH_WAIT_FINISHED;
	/* Link setup is activity, fast until it ends. */
	mg_connparam_hold(MG_CONNPARAM_HOLD_SETUP, true);
	/* A client that goes quiet doesn't keep the keys and the hold. */
	start_phase_timer(MG_SETUP_CLIENT_FINISHED_MS);
	LOG_INF("pairing_ready sent; waiting for client_finished");
	return NULL;
}

static bool parse_u64(const char *str, uint64_t *out)
{
	uint64_t v = 0;

	if (str == NULL || *str == '\0') {
		return false;
	}
	for (const char *p = str; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		uint64_t d = (uint64_t)(*p - '0');

		if (v > (UINT64_MAX - d) / 10) {
			return false;
		}
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

/* Decrypts a pairing_encrypted record in place: the ciphertext's base64
 * text in rxbuf becomes the ciphertext, then the tag after it, then the
 * plaintext, NUL-terminated; *out points at it. (The fields the tag may
 * overwrite were all checked first; the buffers have room for a tag past
 * the end of the message.) */
static const char *decrypt_record(const struct mg_json_obj *o, char **out, size_t *out_len)
{
	const char *sid, *ctr_s, *ct_s, *tag_s;
	uint8_t tag[MG_SETUP_TAG_LEN];
	uint64_t ctr;
	size_t cl, tl;

	if (expired() || !keys_available()) {
		reset_session();
		return "error_pairing_decrypt";
	}
	if (!json_nonempty(o, "session_id", &sid) || strcmp(sid, s.k.session_id_b64) != 0 ||
	    !json_nonempty(o, "counter", &ctr_s) || !parse_u64(ctr_s, &ctr) || ctr != s.rx_ctr ||
	    !json_nonempty(o, "ciphertext", &ct_s) || !json_nonempty(o, "tag", &tag_s)) {
		reset_session();
		return "error_pairing_decrypt";
	}
	uint8_t *ct = (uint8_t *)ct_s;

	if (mg_b64url_decode(tag_s, strlen(tag_s), tag, sizeof(tag), &tl) ||
	    tl != MG_SETUP_TAG_LEN ||
	    mg_b64url_decode(ct_s, strlen(ct_s), ct, PT_MAX, &cl)) {
		reset_session();
		return "error_pairing_decrypt";
	}
	memcpy(&ct[cl], tag, MG_SETUP_TAG_LEN);
	if (mg_setup_open(s.k.rx, MG_SETUP_M2D, s.rx_ctr, s.k.session_id_b64, ct,
			  cl + MG_SETUP_TAG_LEN, ct)) {
		mg_psa_wipe(ct, cl + MG_SETUP_TAG_LEN);
		reset_session();
		return "error_pairing_decrypt";
	}
	mg_psa_wipe(&ct[cl], MG_SETUP_TAG_LEN);
	ct[cl] = '\0';
	s.rx_ctr++;
	*out = (char *)ct;
	*out_len = cl;
	return NULL;
}

static void phase_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (phase_work_gen != s.gen) {
		return;
	}
	if (s.phase == PH_WAIT_FINISHED) {
		/* As a late client_finished finds it: the session gone, nothing sent. */
		LOG_WRN("setup session timed out waiting for client_finished");
		clear_session();
		return;
	}
	if (s.phase == PH_READY) {
		LOG_WRN("confirmed setup session timed out before provisioning");
	} else {
		return;
	}
	(void)send_status("pairing_confirm_timeout", s.gen);
	disconnect_later(100, s.gen);
}

static void start_phase_timer(int ms)
{
	phase_work_gen = s.gen;
	k_work_reschedule_for_queue(mg_app_wq(), &phase_work, K_MSEC(ms));
}

static bool exact_client_finished(const struct mg_json_obj *o)
{
	const char *a = mg_json_str(o, "action");

	return o->n == 1 && a != NULL && strcmp(a, "pairing_client_finished") == 0;
}

/* A re-setup is never confirmed by the talk key. On a board whose only key
 * is the talk key (the XIAO) a set-up gadget takes
 * a re-setup only in setup mode: the button held at boot into the normal
 * firmware (main.c: released before the SAFE cue), Link setup open under
 * the normal identity, for MG_SETUP_MODE_MS or until a re-setup is
 * confirmed; neither kind of SAFE is setup mode. Outside it the hello gets
 * error_pairing_unavailable in plaintext and nothing shows. Test and bench
 * builds are always in setup mode (scripted flows, no hand). Weak so
 * the host suite can drive it. */
#define MG_RESETUP_REFUSED "error_pairing_unavailable"

static bool setup_mode_on;
static int64_t setup_mode_until;

void mg_setup_mode_begin(void)
{
	setup_mode_on = true;
	setup_mode_until = k_uptime_get() + MG_SETUP_MODE_MS;
	LOG_INF("setup: setup mode for %d min (a re-setup can be confirmed)",
		MG_SETUP_MODE_MS / 60000);
}

__attribute__((weak)) bool mg_setup_mode(void)
{
	return mg_setup_mode_now(IS_ENABLED(CONFIG_MG_AUTOMATION) || IS_ENABLED(CONFIG_MG_BENCH_MODE),
				 setup_mode_on, setup_mode_until, k_uptime_get());
}

static bool resetup_exempt(void)
{
	return IS_ENABLED(CONFIG_MG_AUTOMATION) || IS_ENABLED(CONFIG_MG_BENCH_MODE);
}


static void handle_client_finished(const struct mg_json_obj *o)
{
	bool ok = exact_client_finished(o) && !expired() && s.phase == PH_WAIT_FINISHED &&
		  s.rx_ctr == 1;

	if (!ok) {
		(void)send_status("error_pairing_decrypt", 0);
		reset_session();
		disconnect_later(300, 0);
		return;
	}
	advance_gen();
	s.phase = PH_CONFIRM_REQUIRED;
	/* No timer on the prompt: it stays until the press, the link's end, or
	 * the phone giving up (a new hello). */
	s.deadline = 0;
	(void)k_work_cancel_delayable(&phase_work);
	/* The prompt goes out before the button is armed, so an early press
	 * can't make pairing_confirmed overtake it. */
	if (!send_status("confirm_required", s.gen)) {
		disconnect_later(0, s.gen);
		return;
	}
	s.armed = true;
	/* A press already queued or down now is from before the prompt: it
	 * never confirms it (mg_input_after_arm(), mg_setup_button_at()). */
	mg_button_flush_injected();
	takeover = false;
	prompt_conn = mg_transport_conn_gen();
	mg_led_set(MG_LED_PAIR_PENDING, true);
	LOG_INF("setup: press the button to confirm");
#if defined(CONFIG_MG_AUTOMATION) || defined(CONFIG_MG_BENCH_MODE)
	/* For a test rig on the bench console, which answers ">pair.confirm"
	 * (or, in bench mode, the Nordic UART's `pair confirm`). */
	printk("@pair.pending\n");
#endif
}

static void confirm_now(void)
{
	advance_gen();
	s.phase = PH_READY;
	s.armed = false;
	prompt_conn = 0;
	if (mg_setup_store_complete()) {
		setup_mode_on = false; /* setup mode ends with its re-setup's confirm */
	}
	mg_led_set(MG_LED_PAIR_PENDING, false);
	set_deadline(MG_SETUP_CONFIRMED_IDLE_MS);
	start_phase_timer(MG_SETUP_CONFIRMED_IDLE_MS);
	LOG_INF("setup confirmed on the device");
	if (!send_status("pairing_confirmed", s.gen)) {
		disconnect_later(0, s.gen);
	}
}

/* One press confirms a first setup and a re-setup alike, whatever made it
 * (the button, or scripted input): no hold, no press time; but only a press
 * that began after the prompt was armed (after_arm). While the prompt waits,
 * a press is never push-to-talk. */
bool mg_setup_button_at(bool real, bool after_arm)
{
	if (takeover) {
		/* A hello took the prompt over: the button stays the prompt's
		 * while that hello's handshake runs. */
		LOG_INF("press ignored: the prompt's new hello is still being answered");
		return true;
	}
	if (expired() || s.phase != PH_CONFIRM_REQUIRED) {
		return false;
	}
	if (!s.armed || !s.connected) {
		LOG_INF("press ignored: no confirmation armed");
		return true;
	}
	if (!real && !IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
		/* On a release build only the button confirms a setup or a
		 * re-setup, set up or not, in setup mode or not: an injected
		 * press (the USB console's d) is taken by the prompt, never a
		 * talk press, and confirms nothing. */
		LOG_INF("setup: an injected press never confirms (release build)");
		return true;
	}
	if (mg_setup_store_complete() && !mg_setup_mode()) {
		/* A talk press never confirms a re-setup; it only dismisses the
		 * prompt. */
		LOG_INF("setup: a talk press dismisses the re-setup prompt");
		mg_setup_dismiss();
		return true;
	}
	if (!after_arm) {
		/* Queued or down when the prompt armed (a debounced press that
		 * began before it): it confirms nothing, set up or not, in setup
		 * mode or not; the prompt takes it (and its release), and the
		 * next press confirms. */
		LOG_INF("setup: a press from before the prompt doesn't confirm it");
		return true;
	}
	confirm_now();
	return true;
}

bool mg_setup_button(bool real)
{
	/* A scripted confirm (the bench console, the Nordic UART) comes while
	 * the prompt is up, after it by construction. */
	return mg_setup_button_at(real, true);
}

/* The back gesture (a board with one): the prompt closes ("Not now"; it
 * ends as the phone giving up does), and a hello on its connection within
 * 10 s of it shows nothing on a set-up release build (mg_resetup_policy.h). */
void mg_setup_dismiss(void)
{
	/* The prompt is up while it waits for its press and while a takeover's
	 * hello is answered: a dismiss ends it in either. */
	if (expired() || !(mg_setup_confirm_pending() || takeover) || !s.connected) {
		return;
	}
	bool handshake = takeover;

	LOG_INF("setup: the prompt dismissed");
	dismissed_ms = k_uptime_get();
	dismissed_conn = prompt_conn;
	s.armed = false;
	prompt_conn = 0;
	mg_led_set(MG_LED_PAIR_PENDING, false);
	(void)send_status("pairing_confirm_timeout", s.gen);
	if (handshake) {
		/* The takeover's hello never finishes into a new prompt. */
		reset_session();
		disconnect_later(100, 0);
		return;
	}
	disconnect_later(100, s.gen);
}

/* ------------------------------------------------------------------------
 * Provisioning: token-only (wifi "none")
 * ---------------------------------------------------------------------- */

/* Absent, null or an empty string. */
static bool absent_or_empty(const struct mg_json_obj *o, const char *key)
{
	const struct mg_json_field *f = mg_json_get(o, key);

	return f == NULL || f->type == MG_JSON_NULL ||
	       (f->type == MG_JSON_STRING && f->str_len == 0);
}

/* A member that is there: NULL if absent, null or "". */
static const struct mg_json_field *present(const struct mg_json_obj *o, const char *key)
{
	return absent_or_empty(o, key) ? NULL : mg_json_get(o, key);
}

/*
 * provision_v2's optional device key (protocols/README.md, Gadget setup over
 * Muse Link) into *key: absent (device_key and device_key_id both absent,
 * null or "") is 0 with key->key NULL, and vm_id and vm_kind then mean
 * nothing. Otherwise both in standard padded base64 of exactly 32 and 16
 * bytes, an optional vm_id of 1..MG_SETUP_VM_ID_MAX printable ASCII bytes and
 * an optional vm_kind "svm" or "cvm"; anything else is -EINVAL
 * (error_invalid_device_key). The gadget's device session runs on the phone,
 * which can complete the attestation handshake (cvm_supported), so a "cvm" binding
 * is kept like any other.
 */
static int parse_device_key(const struct mg_json_obj *o, uint8_t dk[MG_SETUP_DEVICE_KEY_LEN],
			    uint8_t kid[MG_SETUP_KEY_ID_LEN], struct mg_setup_device_key *key)
{
	const struct mg_json_field *k = present(o, "device_key");
	const struct mg_json_field *id = present(o, "device_key_id");
	const struct mg_json_field *vm = present(o, "vm_id");
	const struct mg_json_field *kind = present(o, "vm_kind");

	memset(key, 0, sizeof(*key));
	if (k == NULL && id == NULL) {
		return 0;
	}
	if (k == NULL || k->type != MG_JSON_STRING || id == NULL || id->type != MG_JSON_STRING ||
	    mg_b64_decode_exact(k->str, k->str_len, dk, MG_SETUP_DEVICE_KEY_LEN) ||
	    mg_b64_decode_exact(id->str, id->str_len, kid, MG_SETUP_KEY_ID_LEN)) {
		return -EINVAL;
	}
	if (vm != NULL) {
		if (vm->type != MG_JSON_STRING || vm->str_len > MG_SETUP_VM_ID_MAX) {
			return -EINVAL;
		}
		for (size_t i = 0; i < vm->str_len; i++) {
			if (vm->str[i] < 0x21 || vm->str[i] > 0x7e) {
				return -EINVAL;
			}
		}
		key->vm_id = vm->str;
		key->vm_id_len = vm->str_len;
	}
	if (kind != NULL) {
		if (kind->type != MG_JSON_STRING ||
		    (strcmp(kind->str, "svm") != 0 && strcmp(kind->str, "cvm") != 0)) {
			return -EINVAL;
		}
		key->vm_kind = kind->str[0] == 's' ? MG_SETUP_VM_KIND_PLAIN : MG_SETUP_VM_KIND_ATTESTED;
	}
	key->key = dk;
	key->key_id = kid;
	return 0;
}

static void provision_failed(const char *status, uint32_t gen)
{
	LOG_WRN("provisioning failed (%s); %s", status,
		mg_setup_store_complete() ? "the old setup stays" : "nothing kept");
	(void)send_status(status, gen);
	disconnect_later(500, gen);
}

static void handle_provision(const struct mg_json_obj *o)
{
	const struct mg_json_field *at = mg_json_get(o, "access_token");
	const struct mg_json_field *rt = mg_json_get(o, "refresh_token");
	const char *tt = mg_json_str(o, "token_type");
	uint8_t k[MG_SETUP_K_LEN], dk[MG_SETUP_DEVICE_KEY_LEN], kid[MG_SETUP_KEY_ID_LEN];
	struct mg_setup_device_key key;
	bool have_dk;
	uint32_t gen;

	if (at == NULL || at->type != MG_JSON_STRING || at->str_len == 0 || rt == NULL ||
	    rt->type != MG_JSON_STRING || rt->str_len == 0 || tt == NULL ||
	    strcmp(tt, "device") != 0) {
		(void)send_status("error_missing_credentials", 0);
		return;
	}
	/* wifi "none": no Wi-Fi to join; nothing is stored. */
	if (!absent_or_empty(o, "ssid") || !absent_or_empty(o, "password")) {
		(void)send_status("error_wifi_unsupported", 0);
		return;
	}
	/* The device key the device session hands the app (protocols/README.md,
	 * Device session): optional, but both or neither, and well formed. A
	 * key that can't be kept is refused before anything is stored (the app
	 * revokes it), and the session may try again. */
	if (parse_device_key(o, dk, kid, &key)) {
		mg_psa_wipe(dk, sizeof(dk));
		mg_psa_wipe(kid, sizeof(kid));
		(void)send_status("error_invalid_device_key", 0);
		return;
	}
	have_dk = key.key != NULL;
	/* link_pairing_mark_provisioning_active() */
	if (!expired() && s.phase == PH_READY) {
		advance_gen();
		s.phase = PH_PROVISIONING;
		set_deadline(MG_SETUP_PROVISIONING_MS);
		(void)k_work_cancel_delayable(&phase_work);
	}
	if (s.phase != PH_PROVISIONING) {
		mg_psa_wipe(dk, sizeof(dk));
		mg_psa_wipe(kid, sizeof(kid));
		(void)send_status("error_operation_in_progress", 0);
		disconnect_later(0, s.gen);
		return;
	}
	gen = s.gen;
	bool again = mg_setup_store_complete();

	if (again && !absent_or_empty(o, "ota_url")) {
		/* Setting a set-up gadget up again never takes an update. */
		LOG_WRN("setup again: ota_url ignored");
	}

	LOG_INF("token-only provisioning%s: access %u B, refresh %u B%s",
		again ? " (re-provisioning a set-up gadget)" : "", (unsigned int)at->str_len,
		(unsigned int)rt->str_len, have_dk ? ", device key" : "");
	if (at->str_len > MG_SETUP_TOKEN_MAX || rt->str_len > MG_SETUP_TOKEN_MAX) {
		LOG_ERR("token longer than %d bytes", MG_SETUP_TOKEN_MAX);
		mg_psa_wipe(dk, sizeof(dk));
		mg_psa_wipe(kid, sizeof(kid));
		provision_failed("error_storage", gen);
		return;
	}
	/* The store rolls back what it wrote: a set-up gadget keeps its old
	 * tokens, K and device key, one that wasn't keeps nothing. */
	/* K and the stored set take the offered node id (gadget-). */
	if (mg_token_proof_derive_key(at->str, at->str_len, mg_identity_offer_node_id(), k) ||
	    mg_setup_store_commit_device_key(at->str, at->str_len, rt->str, rt->str_len, k,
					     have_dk ? &key : NULL)) {
		mg_psa_wipe(k, sizeof(k));
		mg_psa_wipe(dk, sizeof(dk));
		mg_psa_wipe(kid, sizeof(kid));
		provision_failed("error_storage", gen);
		return;
	}
	mg_psa_wipe(k, sizeof(k));
	mg_psa_wipe(dk, sizeof(dk));
	mg_psa_wipe(kid, sizeof(kid));
	forget_clips("set up again");
	/* The old tokens, K and device key are gone: no proof made with them
	 * stands, and no device session opened with them goes on. */
	mg_token_proof_connection_reset();
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_connection_reset();
#endif
	/* Durable: only now auth_ok. */
	s.phase = PH_DONE;
	mg_connparam_hold(MG_CONNPARAM_HOLD_SETUP, false); /* idle 5 s after */
	s.deadline = 0;
	(void)send_status("auth_ok", gen);
	mg_led_event(MG_LED_EVENT_SETUP_DONE); /* setup done, 3 quick blinks */
	LOG_INF("setup %s; Link setup advertising stops", again ? "replaced" : "complete");
	mg_ble_refresh_advertising();
	mg_core_link_refresh();
}

static void handle_unpair(void)
{
	int err = erase_setup();

	forget_clips("unpair");
	if (err) {
		(void)send_status("error_storage", 0);
		return;
	}
	(void)send_status("unpaired", 0);
	reset_session();
	/* A just-reset gadget is in a fast window when setup starts again. */
	mg_transport_adv_boost();
	mg_ble_refresh_advertising();
	mg_core_link_refresh();
	disconnect_later(300, 0);
}

/* ------------------------------------------------------------------------
 * Dispatch (ble_server.c dispatch_command_ex)
 * ---------------------------------------------------------------------- */

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

static void send_device_info(void)
{
	struct mg_jw w;
	int n;

	mg_jw_begin(&w, txbuf, sizeof(txbuf));
	mg_jw_str(&w, "type", "device_info");
	/* What a setup now would give it (gadget-, a set-up gadget too). */
	mg_jw_str(&w, "node_id", mg_identity_offer_node_id());
	mg_jw_str(&w, "version", mg_app_version());
	mg_jw_str(&w, "device_id", mg_identity_device_id());
	mg_jw_str(&w, "mac", mg_identity_mac());
	mg_jw_str(&w, "model", PAIRING_MODEL);
	mg_jw_int(&w, "pairing_protocol", PAIRING_VERSION);
	mg_jw_str(&w, "pairing_auth", PAIRING_COMMUNITY_AUTH);
	mg_jw_int(&w, "pairing_auth_epoch", 0);
	mg_jw_str(&w, "pairing_policy", PAIRING_POLICY_BUTTON);
	mg_jw_str(&w, "build_sha", "");
	mg_jw_str(&w, "wifi", "none");
	/* 0: a BLE-only setup offers no device-key auth, so the app
	 * leaves the gadget keyless and relays its session; provision_v2 still
	 * takes a key if one comes (CONFIG_MG_SETUP_DEVICE_KEY_AUTH, the
	 * automation console's >keyauth=). */
	mg_jw_int(&w, "device_key_auth", device_key_auth);
	mg_jw_bool(&w, "cvm_supported", true);
	mg_jw_int(&w, "mgcommands", 1);
	n = mg_jw_end(&w);
	if (n > 0) {
		(void)send_chunked(txbuf, (size_t)n);
	}
}

static void dispatch(char *data, size_t len, bool decrypted)
{
	/* One for both levels: a record's fields are done with once it opens. */
	static struct mg_json_obj obj;
	struct mg_json_obj *o = &obj;

	if (mg_json_parse(data, len, o)) {
		LOG_WRN("RX invalid JSON (%u bytes)", (unsigned int)len);
		(void)send_status("error_invalid_command", 0);
		return;
	}
	const char *act = mg_json_str(o, "action");

	if (act == NULL) {
		act = "";
	}
	LOG_INF("RX action: %s%s", act, decrypted ? " (encrypted)" : "");

	/* A set-up gadget takes a new setup too (Gadget setup v2,
	 * re-provisioning): the same handshake and press, and its commit
	 * replaces the old tokens and K. */
	enum mg_resetup_hello rh = MG_RESETUP_SHOW;

	if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && mg_setup_store_complete() &&
	    !mg_setup_mode()) {
		/* A set-up gadget whose only key is the talk key re-sets up
		 * only in setup mode; nothing shows. */
		static const char refused[] = MG_RESETUP_REFUSED;

		LOG_WRN("setup: a re-setup hello outside setup mode (hold the button at boot): "
			"refused");
		set_last_status(refused, sizeof(refused) - 1);
		(void)notify((const uint8_t *)refused, sizeof(refused) - 1);
		return;
	}
	if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
		if (prompt_conn != 0 && !mg_setup_confirm_pending() && !takeover) {
			prompt_conn = 0; /* that prompt has ended (its link, an error) */
		}
		rh = mg_resetup_hello(resetup_exempt(), mg_setup_store_complete(), prompt_conn,
				      mg_transport_conn_gen(), dismissed_conn, dismissed_ms, k_uptime_get());
	}
	if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && rh == MG_RESETUP_BUSY) {
		/* Another connection's hello while the prompt is up, or one
		 * within 10 s of a dismiss: nothing shows. */
		static const char busy[] = "error_operation_in_progress";

		LOG_WRN("setup: a re-setup hello %s: not shown",
			prompt_conn ? "from another connection while the prompt is up"
				    : "within 10 s of a dismiss");
		/* A hello's answer, in plaintext; the setup under way is left
		 * as it is. */
		set_last_status(busy, sizeof(busy) - 1);
		(void)notify((const uint8_t *)busy, sizeof(busy) - 1);
	} else if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
		int n = 0;
		uint32_t was = s.gen;
		const char *err = handle_hello(o, &n);

		if (err && s.gen != was) {
			/* The hello started over (its session reset) and failed:
			 * its error goes out in plaintext, as a first hello's
			 * does (a takeover's too, mg-session-v1's "a takeover
			 * hello that fails validation ends the prompt"). */
			s.plaintext_blocked = false;
		}

		if (rh == MG_RESETUP_TAKEOVER && err == NULL) {
			/* The prompt up on this connection is now this hello's:
			 * its light stays (no new flash), the button stays the
			 * prompt's until this hello's client_finished re-arms it. */
			LOG_INF("setup: a hello while the prompt is up: it takes the prompt over");
			takeover = true;
			mg_led_set(MG_LED_PAIR_PENDING, true);
		} else if (rh == MG_RESETUP_TAKEOVER) {
			/* A takeover hello that fails ends the prompt, so no
			 * later hello finds a press waiting: one failing after
			 * the wipe already has (clear_session(): unarmed, its
			 * light off); one refused before it, once answered
			 * under the session's keys, below. */
			prompt_conn = 0;
		}

		if (err) {
			(void)send_status(err, 0);
			if (rh == MG_RESETUP_TAKEOVER) {
				reset_session();
			}
		} else {
			s.plaintext_blocked = true;
			(void)send_chunked(txbuf, (size_t)n);
		}
	} else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {
		char *pt = NULL;
		size_t pl = 0;
		const char *err = decrypt_record(o, &pt, &pl);

		if (err) {
			(void)send_status(err, 0);
			disconnect_later(300, 0);
		} else {
			dispatch(pt, pl, true);
			mg_psa_wipe(pt, pl);
		}
	} else if (!decrypted && s.plaintext_blocked) {
		LOG_WRN("plaintext command suppressed after pairing_ready: %s", act);
	} else if (!decrypted && sensitive(act)) {
		(void)send_status("error_encryption_required", 0);
	} else if (decrypted && strcmp(act, "pairing_client_finished") == 0) {
		handle_client_finished(o);
	} else if (decrypted && sensitive(act) && (expired() || !confirmed())) {
		(void)send_status("error_pairing_confirm_required", 0);
	} else if (decrypted && strcmp(act, "wifi_scan") == 0) {
		/* No Wi-Fi radio: an empty list (apps skip the scan for wifi "none"). */
		(void)send_encrypted("{\"type\":\"wifi_scan_result\",\"networks\":[]}", 0);
	} else if (decrypted && strcmp(act, "unpair") == 0) {
		handle_unpair();
	} else if (decrypted && strcmp(act, "provision_v2") != 0) {
		(void)send_status("error_unknown_action", 0);
	} else if (decrypted) {
		handle_provision(o);
	} else if (strcmp(act, "get_device_info") == 0) {
		send_device_info();
	} else if (strcmp(act, "get_version") == 0) {
		char v[48] = "version:";

		strncat(v, mg_app_version(), sizeof(v) - strlen(v) - 1);
		(void)send_status(v, 0);
	} else {
		(void)send_status("error_unknown_action", 0);
	}
	if (decrypted) {
		mg_psa_wipe(o, sizeof(*o));
	}
}

/* ------------------------------------------------------------------------
 * RX framing
 * ---------------------------------------------------------------------- */

static void rx_reset(void)
{
	if (rx.active) {
		mg_psa_wipe(rxbuf, rx.len);
	}
	memset(&rx, 0, sizeof(rx));
}

void mg_setup_rx(const uint8_t *data, size_t len)
{
	if (!s.connected || len == 0) {
		return;
	}
	if (len < CHUNK_HDR || data[0] != CHUNK_MAGIC) {
		size_t n = MIN(len, (size_t)SINGLE_MAX);

		memcpy(single, data, n);
		single[n] = '\0';
		dispatch(single, n, false);
		mg_psa_wipe(single, sizeof(single));
		return;
	}
	uint8_t idx = data[1], total = data[2];
	const uint8_t *frag = &data[CHUNK_HDR];
	size_t flen = len - CHUNK_HDR;

	if (total == 0) {
		rx_reset();
		return;
	}
	if (idx == 0 || rx.total != total) {
		rx_reset();
		rx.total = total;
		rx.active = true;
	}
	if (idx != rx.next || idx >= rx.total || rx.len + flen > RX_MAX) {
		rx_reset();
		return;
	}
	memcpy(&rxbuf[rx.len], frag, flen);
	rx.len += flen;
	rx.count++;
	rx.next = idx + 1;
	if (rx.count < rx.total) {
		return;
	}
	size_t n = rx.len;

	rxbuf[n] = '\0';
	memset(&rx, 0, sizeof(rx));
	LOG_DBG("RX reassembled (%u bytes)", (unsigned int)n);
	dispatch(rxbuf, n, false);
	mg_psa_wipe(rxbuf, sizeof(rxbuf));
}

/* ------------------------------------------------------------------------
 * Connection, reset, init
 * ---------------------------------------------------------------------- */

void mg_setup_connected(void)
{
	rx_reset();
	reset_session();
	s.plaintext_blocked = false;
	s.connected = true;
	set_last_status("idle", 4);
}

void mg_setup_disconnected(void)
{
	s.connected = false;
	rx_reset();
	reset_session();
	s.plaintext_blocked = false;
	(void)k_work_cancel_delayable(&disconnect_work);
}

void mg_setup_set_device_key_auth(int v)
{
	device_key_auth = v ? 1 : 0;
	LOG_INF("setup: device_key_auth %d in get_device_info", device_key_auth);
}

bool mg_setup_confirm_pending(void)
{
	return s.phase == PH_CONFIRM_REQUIRED && s.armed;
}

bool mg_setup_advertise_link(void)
{
	return !mg_setup_store_complete();
}

int mg_setup_factory_reset(const char *why)
{
	return mg_setup_erase(why, true);
}

int mg_setup_erase(const char *why, bool end_session)
{
	LOG_INF("%s: erasing the setup", why);
	int err = erase_setup();

	forget_clips(why);

	reset_session();
	mg_token_proof_connection_reset();
#if defined(CONFIG_MG_VM_SESSION)
	/* No credential, no device session: nothing it queued goes out (the
	 * sealed clear ends it itself, after its answer). */
	if (end_session) {
		mg_vm_session_connection_reset();
	}
#endif
	/* A just-reset gadget is in a fast window when setup starts again. */
	mg_transport_adv_boost();
	mg_ble_refresh_advertising();
	mg_core_link_refresh();
	return err;
}

void mg_setup_clips_resume(void)
{
	if (!mg_setup_store_clips_due()) {
		return;
	}
#if defined(CONFIG_MG_AUDIO_QUEUE)
	bool mounted = mg_queue_present(), wiping = mg_queue_wiping();
#else
	bool mounted = false, wiping = false;
#endif
	switch (mg_clips_owed_step(IS_ENABLED(CONFIG_MG_AUDIO_QUEUE), mounted, wiping)) {
	case MG_CLIPS_OWED_ERASE:
		forget_clips("owed from before");
		break;
	case MG_CLIPS_OWED_KEEP:
		if (!mounted) {
			/* A failed mount, not a board without a store: the mark
			 * stays until the erase has run. */
			LOG_WRN("offline clips' erase owed: the clip store isn't mounted, at a "
				"later boot");
		}
		break;
	case MG_CLIPS_OWED_GONE:
		/* No clip store on this build: nothing to erase. */
		(void)mg_setup_store_clips_gone();
		break;
	}
}

void mg_setup_init(void)
{
	memset(&s, 0, sizeof(s));
	memset(&rx, 0, sizeof(rx));
	prompt_conn = 0;
	dismissed_ms = 0;
	dismissed_conn = 0;
	takeover = false;
	setup_mode_on = false;
	k_work_init_delayable(&phase_work, phase_fn);
	k_work_init_delayable(&disconnect_work, disconnect_fn);
	if (mg_psa_init()) {
		LOG_ERR("PSA crypto init failed");
	}
	LOG_INF("Link setup: node %s, %s", mg_identity_node_id(),
		mg_setup_store_complete() ? "set up" : "waiting for setup");
	if (SDK_TOKEN[0] == '\0') {
		LOG_WRN("no SDK token (CONFIG_MG_SDK_TOKEN): the Muse app can't set this "
			"gadget up");
	}
}
