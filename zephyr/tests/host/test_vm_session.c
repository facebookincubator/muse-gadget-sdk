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
 * The device session (mg_vm_session.c, mg_vm_link.c) against
 * protocols/test-vectors/mg-session-v1.json, every write of every exchange
 * answered with exactly the vector's notifications, with the real token
 * proof gating it and the real store holding the credentials; then what
 * the vectors don't reach: the store's token update and its failures,
 * retries when the notification queue is full, a lost client write, the
 * send queue, a message larger than the window, the token proof ending the
 * session, link.invoke's checks and busy answers, /link-control framing
 * and failures, the heartbeat, and the board's commands.
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "check.h"
#include "host_fakes.h"
#include "mg_envelope.h"
#include "mg_identity.h"
#include "mg_led.h"
#include "mg_psa.h"
#include "mg_session_seal.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_vm_link.h"
#include "mg_vm_session.h"
#include "session_vectors.h"
#include "sealed_data_vectors.h"

/* ---- fakes for mg_vm_commands.c ---- */

static int battery_mv = -1;
static int led_command = -1;
static uint32_t led_ms;

uint8_t mg_app_battery_percent(void)
{
	return 87;
}

int mg_app_battery_mv(void)
{
	return battery_mv;
}

void mg_led_command(enum mg_led_command c, uint32_t duration_ms)
{
	led_command = c;
	led_ms = duration_ms;
}

/* ---- the client's side ---- */

static uint8_t K[32];
/* The proof the vectors' sealed exchanges were made under (session_seal). */
#define client_nonce sv_seal_client_nonce
#define device_nonce sv_seal_device_nonce

/* Every session is sealed (an unsealed hello is insecure). The harness seals what it sends on data and
 * credentials_update with its own counter (hs_cctr, from 0 at each sealed
 * hello) and opens the device's sealed data whatever its counter. */
static mgseal_keys_t hs_keys;
static uint32_t hs_cctr;
static bool hs_sealed;
/* The last token proof's nonces (the next sealed hello's keys). */
static const uint8_t *hs_cn = sv_seal_client_nonce, *hs_dn = sv_seal_device_nonce;

static void put32(uint32_t v, uint8_t *b)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* A write on Control: [mg_command_session, ...]. */
static void write(const uint8_t *b, size_t n)
{
	CHECK(n >= 1 && b[0] == mg_command_session);
	CHECK(n <= (size_t)host_mtu - 3);
	mg_vm_session_rx(&b[1], n - 1);
}

/* An unsealed hello (refused insecure on every link). */
static void hello_plain(uint32_t max, uint32_t window)
{
	uint8_t m[11] = {mg_command_session, mg_session_hello, MG_SESSION_VERSION};

	put32(max, &m[3]);
	put32(window, &m[7]);
	hs_sealed = false;
	write(m, sizeof(m));
}

/* The client's hello: sealed, with the legacy credentials_saved answer. */
static void hello(uint32_t max, uint32_t window)
{
	uint8_t m[12] = {mg_command_session, mg_session_hello, MG_SESSION_VERSION};

	put32(max, &m[3]);
	put32(window, &m[7]);
	m[11] = mg_session_hello_flag_sealed;
	/* The device's keys: its stored K and the node id it registers. */
	uint8_t k[32];
	const char *node = mg_vm_link_node_id();

	if (mg_setup_store_get_k(k) != 0) {
		memcpy(k, K, sizeof(k));
	}
	CHECK(mgseal_keys(&hs_keys, k, hs_cn, hs_dn, node, strlen(node)) == MGSEAL_OK);
	hs_cctr = 0;
	hs_sealed = true;
	write(m, sizeof(m));
}

static void state(uint8_t st, uint16_t epoch)
{
	const uint8_t m[] = {mg_command_session, mg_session_state, st, (uint8_t)epoch,
			     (uint8_t)(epoch >> 8)};

	write(m, sizeof(m));
}

static void credit(uint32_t n)
{
	uint8_t m[6] = {mg_command_session, mg_session_credit};

	put32(n, &m[2]);
	write(m, sizeof(m));
}

/* Sends msg as sub fragments with extra flags (mg_session_flag_sealed). */
static void send_frag_flags(uint8_t sub, uint8_t flags, const uint8_t *msg, size_t len);

/* Sends msg as sub (data or credentials_update) fragments, each filled but
 * the last. */
static void send_frag(uint8_t sub, const uint8_t *msg, size_t len)
{
	static uint8_t buf[MG_SESSION_MAX_MESSAGE + MGSEAL_OVERHEAD];

	if (hs_sealed && (sub == mg_session_data || sub == mg_session_credentials_update) &&
	    len + MGSEAL_OVERHEAD <= sizeof(buf)) {
		memcpy(&buf[4], msg, len);
		CHECK(mgseal_seal(&hs_keys, MGSEAL_DIR_CLIENT, hs_cctr++, buf, len) == MGSEAL_OK);
		send_frag_flags(sub, mg_session_flag_sealed, buf, len + MGSEAL_OVERHEAD);
		return;
	}
	send_frag_flags(sub, 0, msg, len);
}

static void send_frag_flags(uint8_t sub, uint8_t flags, const uint8_t *msg, size_t len)
{
	uint8_t f[260];
	size_t off = 0;

	while (off < len) {
		bool first = off == 0;
		size_t h = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
		size_t n = MIN(host_mtu - 3 - h, len - off);

		f[0] = mg_command_session;
		f[1] = sub;
		f[2] = flags | (first ? mg_session_flag_first : 0) |
		       (off + n == len ? mg_session_flag_last : 0);
		if (first) {
			put32((uint32_t)len, &f[3]);
		}
		memcpy(&f[h], &msg[off], n);
		write(f, h + n);
		off += n;
	}
}

/* A VM message on stream 1: a body chunk carrying control messages. */
static size_t vm_chunk(int64_t stream, const uint8_t *data, size_t n, uint8_t *out, size_t cap)
{
	const struct mg_env_frame f = {
		.stream_id = stream, .kind = MG_ENV_BODY_CHUNK, .body = {data, n}};
	int len = mg_env_encode_response_message(&f, out, cap);

	CHECK(len > 0);
	return len > 0 ? (size_t)len : 0;
}

/* One control message: u32-LE length and the JSON. */
static size_t control(const char *json, uint8_t *out)
{
	size_t n = strlen(json);

	put32((uint32_t)n, out);
	memcpy(&out[4], json, n);
	return n + 4;
}

static void vm_send_json(const char *json)
{
	static uint8_t c[4200], m[4300];
	size_t n = control(json, c);

	send_frag(mg_session_data, m, vm_chunk(1, c, n, m, sizeof(m)));
}

/* The device's data messages in host_notes from index *seen on, one at a
 * time: the next whole message into out, its length; 0 if none. */
static size_t next_device_msg(int *seen, uint8_t *out, size_t cap)
{
	size_t len = 0, total = 0;
	bool started = false;

	while (*seen < host_nnotes) {
		const struct host_note *n = &host_notes[(*seen)++];

		if (n->len < 3 || n->data[0] != mg_command_session ||
		    n->data[1] != mg_session_data) {
			continue;
		}
		const uint8_t *p = &n->data[3];
		size_t k = n->len - 3;

		if (n->data[2] & mg_session_flag_first) {
			total = get32(p);
			p += 4;
			k -= 4;
			len = 0;
			started = true;
		} else if (!started) {
			continue; /* the rest of a message sent before */
		}
		if (len + k > cap) {
			CHECK(0); /* a clean failure, not a write past the buffer */
			return 0;  /* as "no message": the caller's checks fail too */
		}
		memcpy(&out[len], p, k);
		len += k;
		if (n->data[2] & mg_session_flag_last) {
			CHECK(len == total);
			if (hs_sealed && (n->data[2] & mg_session_flag_sealed) &&
			    len > MGSEAL_OVERHEAD) {
				/* Opened under the counter it carries (the tag
				 * proves it): the harness reads messages out of
				 * order. */
				size_t plen = 0;

				uint32_t ctr = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 |
					       (uint32_t)out[2] << 8 | out[3]; /* big-endian */

				CHECK(mgseal_open(&hs_keys, MGSEAL_DIR_DEVICE, ctr, out, len, &plen) ==
				      MGSEAL_OK);
				memmove(out, &out[4], plen);
				return plen;
			}
			return len;
		}
	}
	return 0;
}

/* The control JSON in a device message (a body chunk on stream 1), or NULL. */
static const char *control_json(const uint8_t *msg, size_t len)
{
	static char json[4200];
	struct mg_env_bytes payload;
	struct mg_env_frame f;
	uint8_t service = 9;

	if (mg_env_decode_request_envelope(msg, len, &service, &payload) || service != 0 ||
	    mg_env_decode_frame(payload.p, payload.n, NULL, 0, &f) || f.kind != MG_ENV_BODY_CHUNK ||
	    f.stream_id != 1 || f.body.n < 4 || get32(f.body.p) != f.body.n - 4 ||
	    f.body.n - 4 >= sizeof(json)) {
		return NULL;
	}
	memcpy(json, &f.body.p[4], f.body.n - 4);
	json[f.body.n - 4] = '\0';
	return json;
}

/* The next control JSON the device sent, or NULL. */
static const char *next_control(int *seen)
{
	static uint8_t msg[4300];
	size_t n = next_device_msg(seen, msg, sizeof(msg));

	return n ? control_json(msg, n) : NULL;
}

static bool last_error(uint16_t code)
{
	const uint8_t e[] = {mg_command_error, mg_command_session, mg_error_data_code,
			     (uint8_t)code, (uint8_t)(code >> 8)};
	const struct host_note *n = host_nnotes ? &host_notes[host_nnotes - 1] : NULL;

	return n && n->len == sizeof(e) && memcmp(n->data, e, sizeof(e)) == 0;
}

/* Any notification is that session error (credit may follow it). */
static bool had_error(uint16_t code)
{
	const uint8_t e[] = {mg_command_error, mg_command_session, mg_error_data_code,
			     (uint8_t)code, (uint8_t)(code >> 8)};

	for (int i = 0; i < host_nnotes; i++) {
		if (host_notes[i].len == sizeof(e) && memcmp(host_notes[i].data, e, sizeof(e)) == 0) {
			return true;
		}
	}
	return false;
}

/* ---- connections ---- */

/* The vectors' credentials: with the device key (test_credentials), and
 * with bound its VM binding too (test_credentials_bound). */
static void store_credentials_bound(bool device_key, bool bound)
{
	struct mg_setup_device_key dk = {.key = sv_device_key, .key_id = sv_key_id};

	if (bound) {
		dk.vm_id = SV_VM_ID;
		dk.vm_id_len = strlen(SV_VM_ID);
		dk.vm_kind = SV_VM_KIND;
	}
	/* The vectors' sealed exchanges run under session_seal's fixed K (as a
	 * setup would have stored it). */
	memcpy(K, sv_seal_k, sizeof(K));
	CHECK(mg_setup_store_commit_device_key(SV_ACCESS, strlen(SV_ACCESS), SV_REFRESH,
					       strlen(SV_REFRESH), K,
					       device_key ? &dk : NULL) == 0);
}

static void store_credentials(bool device_key)
{
	store_credentials_bound(device_key, false);
}

/* The token proof as the app runs it, with the K setup left. */
static void prove_with_maybe(const uint8_t *cn, const uint8_t *dn, bool must_match);

static void prove_with(const uint8_t *cn, const uint8_t *dn)
{
	prove_with_maybe(cn, dn, true);
}

/* must_match false: a gadget without a setup answers not_found. */
static void prove_with_maybe(const uint8_t *cn, const uint8_t *dn, bool must_match)
{
	uint8_t ch[17] = {mg_token_proof_challenge}, conf[33] = {mg_token_proof_confirm};

	memcpy(&ch[1], cn, 16);
	mg_token_proof_test_set_nonce(dn);
	int before = host_nnotes;

	mg_token_proof_rx(ch, sizeof(ch));
	/* No response (not_found without a setup): the client stops there. */
	bool answered = false;

	for (int k = before; k < host_nnotes; k++) {
		answered |= host_notes[k].data[0] == mg_command_token_proof &&
			    host_notes[k].data[1] == mg_token_proof_response;
	}
	CHECK(!must_match || answered);
	if (answered) {
		CHECK(mg_token_proof_mac(K, false, cn, dn, &conf[1]) == 0);
		mg_token_proof_rx(conf, sizeof(conf));
	}
	CHECK(!must_match || mg_token_proof_matched());
	hs_cn = cn;
	hs_dn = dn;
}

static void prove(void)
{
	host_notes_clear();
	prove_with(client_nonce, device_nonce);
}

/* A new connection on the same store. */
static void reconnect(uint16_t mtu, bool proof, bool encrypted)
{
	host_mtu = mtu;
	host_encrypted = encrypted;
	host_notify_fail = 0;
	mg_token_proof_connection_reset();
	mg_vm_session_connection_reset();
	if (proof) {
		prove();
	}
	host_notes_clear();
}

static void connect_fresh_bound(uint16_t mtu, bool token, bool proof, bool encrypted, bool bound)
{
	host_settings_clear();
	(void)mg_setup_store_load();
	if (token) {
		store_credentials_bound(true, bound);
	}
	/* No proof can match without a token (the vector's "no token" device
	 * says proof true): not_found comes first either way. */
	reconnect(mtu, proof && token, encrypted);
}

static void connect_fresh(uint16_t mtu, bool token, bool proof, bool encrypted)
{
	connect_fresh_bound(mtu, token, proof, encrypted, false);
}

/* The gadget re-asks only while it has an
 * active task. The vectors and the re-ask tests describe a gadget that has
 * one, given here as a board's wait (the board hook,
 * which the app's weak default leaves false); test_keepalive turns it
 * off. */
static bool test_wait = true;

bool mg_vm_wait_active(void)
{
	return test_wait;
}

static void use_vector_commands(void)
{
	mg_vm_link_test_clear();
	mg_vm_link_set_identity(&sv_identity);
	for (int i = 0; i < SV_COMMAND_COUNT; i++) {
		CHECK(mg_vm_link_add_command(&sv_commands[i]) == 0);
	}
}

/* hello, open, the /link-control response and the register reply. */
static void open_session(uint16_t mtu, uint32_t client_window)
{
	static uint8_t m[256];
	const struct mg_env_frame ok = {.stream_id = 1, .kind = MG_ENV_RESPONSE, .status = 200};
	int n;

	connect_fresh(mtu, true, true, true);
	hello(65536, client_window);
	state(mg_session_state_open, 1);
	n = mg_env_encode_response_message(&ok, m, sizeof(m));
	send_frag(mg_session_data, m, (size_t)n);
	vm_send_json("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\","
		     "\"node_id\":\"" SV_NODE_ID "\"}}");
	CHECK(mg_vm_link_registered());
	host_notes_clear();
}

/* ---- the vectors ---- */

static void dump(const char *what, const uint8_t *b, size_t n)
{
	fprintf(stderr, "      %s ", what);
	for (size_t i = 0; i < n; i++) {
		fprintf(stderr, "%02x", b[i]);
	}
	fprintf(stderr, "\n");
}

/* mg_vm_session.c's retry of what a full notification queue held back. */
#define SV_RETRY_MS 20

static void test_vectors(void)
{
	int steps = 0;

	printf("  mg-session-v1.json, byte for byte\n");
	CHECK(SV_MAX_MESSAGE == CONFIG_MG_VM_SESSION_MAX_MESSAGE);
	CHECK(SV_WINDOW == CONFIG_MG_VM_SESSION_WINDOW);
	use_vector_commands();
	for (int e = 0; e < SV_EXCHANGE_COUNT; e++) {
		const struct sv_exchange *ex = &sv_exchanges[e];
		bool ok = true;

		/* The exchange's task (device flag task): a board's wait here. */
		test_wait = ex->task;
		host_notify_skip = host_notify_fail = 0;
		/* The token proof's answers need a link of MG_MIN_ATT_MTU (the
		 * vectors' device proves outside the link's MTU): proven there,
		 * then the exchange's MTU (the low-MTU hello). */
		connect_fresh_bound(MAX(ex->mtu, MG_MIN_ATT_MTU), ex->holds_token, ex->proof,
				    ex->encrypted, ex->vm_binding);
		host_mtu = ex->mtu;
		if (ex->credentials_wait) {
			/* No room for the credentials when the hello is
			 * answered: its answer goes, their first fragment
			 * doesn't (a full notification queue); they wait for
			 * the next retry. */
			host_notify_skip = 1;
			host_notify_fail = 1;
		}
		for (int i = 0; i < ex->n_steps; i++) {
			const struct sv_step *st = &ex->steps[i];
			bool same = true;

			host_notes_clear();
			if (st->proof_cn) {
				/* A new token proof: its challenge, response,
				 * confirm and result aren't in the vector; its
				 * errors (not_found without a setup) and what else
				 * the device sends are. */
				prove_with_maybe(st->proof_cn, st->proof_dn,
						 mg_setup_store_complete());
				int kept = 0;

				for (int k = 0; k < host_nnotes; k++) {
					const uint8_t *d = host_notes[k].data;

					if (d[0] != mg_command_token_proof) {
						host_notes[kept++] = host_notes[k];
					}
				}
				host_nnotes = kept;
			} else if (st->task == 5) {
				/* The board's stack has room again: the retry
				 * runs (mg_vm_session.c's RETRY_MS). */
				host_notify_skip = host_notify_fail = 0;
				host_advance(SV_RETRY_MS);
			} else if (st->task == 4) {
				mg_vm_session_nudge(); /* the credit keepalive */
			} else if (st->task == 3) {
				/* A long session's stand-in. */
				mg_vm_session_test_set_counters(st->ctr_rx, st->ctr_tx);
			} else if (st->task) {
				/* A task starts (a user action) or ends. */
				test_wait = st->task == 1;
				if (test_wait) {
					mg_vm_session_reason();
				}
			} else {
				write(st->w, st->wn);
			}
			/* What the device does at once runs on the app work queue
			 * (the re-ask at 0 ms): run what's due now. */
			host_advance(0);
			steps++;
			same = host_nnotes == st->n_notify;
			for (int k = 0; same && k < st->n_notify; k++) {
				same = host_notes[k].len == st->notify[k].n &&
				       memcmp(host_notes[k].data, st->notify[k].b, st->notify[k].n) ==
					       0;
			}
			CHECK(same);
			if (!same && ok) {
				ok = false;
				fprintf(stderr, "    %s, step %d (%s)\n", ex->name, i,
					st->note ? st->note : "");
				if (st->w) {
					dump("write ", st->w, st->wn);
				}
				for (int k = 0; k < st->n_notify; k++) {
					dump("want  ", st->notify[k].b, st->notify[k].n);
				}
				for (int k = 0; k < host_nnotes; k++) {
					dump("got   ", host_notes[k].data, host_notes[k].len);
				}
			}
		}
	}
	test_wait = true;
	printf("    %d exchanges, %d steps\n", SV_EXCHANGE_COUNT, steps);
	/* Every exchange and step replayed (20 or more). */
	CHECK(SV_EXCHANGE_COUNT >= 20 && steps == SV_STEP_COUNT);
}

/* ---- the store: credentials_update ---- */

static void update(const char *json)
{
	send_frag(mg_session_credentials_update, (const uint8_t *)json, strlen(json));
}

/* The session's keys while it's sealed (set by the tests that seal), so
 * saved() can open a sealed credentials_saved. */
static const mgseal_keys_t *saved_keys;

static bool saved(uint8_t v)
{
	const uint8_t m[] = {mg_command_session, mg_session_credentials_saved, v};
	uint8_t b[64];
	size_t plen = 0;

	if (host_nnotes != 1) {
		return false;
	}
	if (host_notes[0].len == 3) {
		return memcmp(host_notes[0].data, m, 3) == 0;
	}
	/* Sealed: [session, saved, first|last|sealed, total_len 29, counter ||
	 * ct || tag], plaintext result || hash. */
	if (!saved_keys || host_notes[0].len != MG_SESSION_FIRST_HEADER + MGSEAL_OVERHEAD + 1 +
						       MG_SESSION_SAVED_HASH_LEN ||
	    host_notes[0].data[1] != mg_session_credentials_saved ||
	    host_notes[0].data[2] != (mg_session_flag_first | mg_session_flag_last |
				      mg_session_flag_sealed)) {
		return false;
	}
	size_t n = host_notes[0].len - MG_SESSION_FIRST_HEADER;
	uint32_t ctr;

	memcpy(b, &host_notes[0].data[MG_SESSION_FIRST_HEADER], n);
	ctr = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	return mgseal_open(saved_keys, MGSEAL_DIR_DEVICE, ctr, b, n, &plen) == MGSEAL_OK &&
	       plen == 1 + MG_SESSION_SAVED_HASH_LEN && b[4] == v;
}

static bool stored_pair(const char *at, const char *rt)
{
	char a[64], r[64];
	size_t al, rl;

	return mg_setup_store_get_tokens(a, sizeof(a), &al, r, sizeof(r), &rl) == 0 &&
	       strcmp(a, at) == 0 && strcmp(r, rt) == 0;
}

static void test_token_update(void)
{
	uint8_t k[32];

	printf("  credentials_update: one commit, K kept, failures keep the old pair\n");
	use_vector_commands();
	connect_fresh(100, true, true, true);
	hello(65536, 65536);
	host_notes_clear();
	update("{\"access_token\":\"at-2\",\"refresh_token\":\"rt-2\",\"extra\":[1]}");
	CHECK(saved(1));
	CHECK(stored_pair("at-2", "rt-2"));
	/* The new pair and copies of K and the device key in the other slot,
	 * the old slot gone. */
	CHECK(host_settings_len("mg/setup/at1") == 4 && host_settings_len("mg/setup/at") == -1);
	CHECK(host_settings_len("mg/setup/k1") == 32 && host_settings_len("mg/setup/dk1") == 32 &&
	      host_settings_len("mg/setup/kid1") == 16 && host_settings_len("mg/setup/dk") == -1);
	/* It survives a reboot, and K is what setup derived. */
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_complete());
	CHECK(stored_pair("at-2", "rt-2") && mg_setup_store_has_device_key());
	CHECK(mg_setup_store_get_k(k) == 0);
	CHECK_MEM(k, K, 32);
	/* So the app's proof still matches on the next connection. */
	reconnect(100, true, true);
	hello(65536, 65536);
	update("{\"access_token\":\"at-3\",\"refresh_token\":\"rt-3\"}");
	CHECK(stored_pair("at-3", "rt-3"));
	CHECK(host_settings_len("mg/setup/at") == 4 && host_settings_len("mg/setup/at1") == -1);

	/* A failed write at either token, K, the device key, its id or the
	 * record: saved 0, the old pair kept, before and after a reboot, and no
	 * stray slot left behind. */
	for (int fail = 1; fail <= 6; fail++) {
		host_notes_clear();
		host_settings_fail_save(fail);
		update("{\"access_token\":\"at-x\",\"refresh_token\":\"rt-x\"}");
		CHECK(saved(0));
		CHECK(stored_pair("at-3", "rt-3"));
		CHECK(mg_setup_store_load() == 0 && stored_pair("at-3", "rt-3"));
		CHECK(host_settings_len("mg/setup/at1") == -1 && host_settings_len("mg/setup/k1") == -1);
		CHECK(mg_setup_store_has_device_key());
	}
	/* A power loss after the new pair, before the record: the old pair. */
	settings_save_one("mg/setup/at1", "at-y", 4);
	settings_save_one("mg/setup/rt1", "rt-y", 4);
	CHECK(mg_setup_store_load() == 0 && stored_pair("at-3", "rt-3"));
	CHECK(host_settings_len("mg/setup/at1") == -1 && host_settings_len("mg/setup/rt1") == -1);
	/* Not an object, empty or missing tokens, not JSON: error, nothing
	 * stored. */
	static const char *const bad[] = {
		"[1]",
		"{\"access_token\":\"\",\"refresh_token\":\"r\"}",
		"{\"access_token\":\"a\",\"refresh_token\":5}",
		"{\"access_token\":\"a\",\"refresh_token\":\"r\"",
		"{\"access_token\":\"a\",\"access_token\":\"b\",\"refresh_token\":\"r\"}",
	};
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		reconnect(100, true, true);
		hello(65536, 65536);
		host_notes_clear();
		update(bad[i]);
		CHECK(last_error(mg_error_code_invalid_value) && host_nnotes == 1);
		CHECK(stored_pair("at-3", "rt-3"));
	}
	/* Escapes are stored decoded. */
	host_notes_clear();
	update("{\"access_token\":\"a\\u00e9\",\"refresh_token\":\"r\\/1\"}");
	CHECK(saved(1) && stored_pair("a\xc3\xa9", "r/1"));
	/* A pair whose stored document (node id and device key with it, what
	 * the next credentials send) would be longer than
	 * MG_SESSION_MAX_CREDENTIALS: invalid_value, nothing stored, and the
	 * next hello still sends credentials (mgcommands.h). */
	static char big[MG_SESSION_MAX_CREDENTIALS];
	size_t room = MG_SESSION_MAX_CREDENTIALS - 2 * strlen(SV_ACCESS) - 40;
	int n = snprintf(big, sizeof(big), "{\"access_token\":\"%0*d\",\"refresh_token\":\"%0*d\"}",
			 (int)room / 2, 0, (int)room / 2, 0);

	CHECK(n > 0 && (size_t)n < sizeof(big));
	host_notes_clear();
	update(big);
	CHECK(last_error(mg_error_code_invalid_value) && !saved(0) && !saved(1) &&
	      stored_pair("a\xc3\xa9", "r/1"));
	reconnect(100, true, true);
	hello(65536, 65536);
	CHECK(host_nnotes > 1 && host_notes[0].data[1] == mg_session_hello);
	/* After the setup is erased (the proof's clear): saved 0... */
	reconnect(100, true, true);
	hello(65536, 65536);
	CHECK(mg_setup_store_update_tokens("a", 1, "r", 1) == 0);
	CHECK(mg_setup_store_erase() == 0);
	CHECK(mg_setup_store_update_tokens("a", 1, "r", 1) == -ENOENT);
	/* ...and the session is over: its hello no longer counts. */
	host_notes_clear();
	update("{\"access_token\":\"a\",\"refresh_token\":\"r\"}");
	CHECK(last_error(mg_error_code_invalid_value));
	hello(65536, 65536);
	CHECK(last_error(mg_error_code_not_found));
}

static const char *credentials_sent(void);

/* After a setup with a device key bound to a VM: the refreshed pair keeps
 * the key and its binding, and a failure at any write keeps all of the old
 * set. */
static void test_token_update_bound(void)
{
	uint8_t dk[32], kid[16];
	char vm_id[MG_SETUP_VM_ID_MAX + 1];
	size_t vm_len;
	enum mg_setup_vm_kind kind;

	printf("  credentials_update keeps the device key and its VM binding\n");
	use_vector_commands();
	connect_fresh_bound(100, true, true, true, true);
	hello(65536, 65536);
	host_notes_clear();
	update("{\"access_token\":\"at-2\",\"refresh_token\":\"rt-2\"}");
	CHECK(saved(1) && stored_pair("at-2", "rt-2"));
	CHECK(host_settings_len("mg/setup/vm1") == (int)strlen(SV_VM_ID) &&
	      host_settings_len("mg/setup/vm") == -1 && host_settings_count() == 7);
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_get_device_key(dk, kid) == 0);
	CHECK_MEM(dk, sv_device_key, 32);
	CHECK_MEM(kid, sv_key_id, 16);
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 && kind == SV_VM_KIND);
	CHECK_STR(vm_id, SV_VM_ID);
	for (int fail = 1; fail <= 7; fail++) {
		host_notes_clear();
		host_settings_fail_save(fail);
		update("{\"access_token\":\"at-x\",\"refresh_token\":\"rt-x\"}");
		CHECK(saved(0) && stored_pair("at-2", "rt-2"));
		CHECK(mg_setup_store_load() == 0 && stored_pair("at-2", "rt-2"));
		CHECK(host_settings_count() == 7 && host_settings_len("mg/setup/vm") == -1);
		CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 && kind == SV_VM_KIND);
		CHECK_STR(vm_id, SV_VM_ID);
	}
	/* A power loss after the new set's vm_id, before the record: the old
	 * set, and the leftovers go. */
	settings_save_one("mg/setup/at", "at-y", 4);
	settings_save_one("mg/setup/vm", "vm-y", 4);
	CHECK(mg_setup_store_load() == 0 && stored_pair("at-2", "rt-2"));
	CHECK(host_settings_len("mg/setup/vm") == -1 && host_settings_count() == 7);
	/* The next hello carries the new pair and the same binding. */
	reconnect(247, true, true);
	hello(65536, 65536);
	CHECK(strstr(credentials_sent(), "\"access_token\":\"at-2\"") != NULL);
	CHECK(strstr(credentials_sent(), "\",\"vm_id\":\"" SV_VM_ID "\",\"vm_kind\":\"cvm\"}") !=
	      NULL);
}

/* ---- credentials ---- */

static const char *sealed_credentials(const mgseal_keys_t *keys, uint32_t ctr);

/* The credentials the device sent, opened when sealed (counter 0). */
static const char *credentials_sent(void)
{
	static char json[2100];
	size_t len = 0;

	for (int i = 0; i < host_nnotes; i++) {
		const struct host_note *n = &host_notes[i];

		if (n->len < 3 || n->data[0] != mg_command_session ||
		    n->data[1] != mg_session_credentials) {
			continue;
		}
		if (n->data[2] & mg_session_flag_sealed) {
			const char *j = sealed_credentials(&hs_keys, 0);

			snprintf(json, sizeof(json), "%s", j ? j : "");
			return json;
		}
		size_t h = (n->data[2] & mg_session_flag_first) ? 7 : 3;

		memcpy(&json[len], &n->data[h], n->len - h);
		len += n->len - h;
	}
	json[len] = '\0';
	return json;
}

static void test_credentials(void)
{
	static char big[1100];

	printf("  credentials: without a device key, too long, gating\n");
	use_vector_commands();
	host_settings_clear();
	(void)mg_setup_store_load();
	store_credentials(false);
	reconnect(247, true, true);
	hello(65536, 65536);
	CHECK_STR(credentials_sent(), "{\"node_id\":\"" SV_NODE_ID "\",\"access_token\":\"" SV_ACCESS
				      "\",\"refresh_token\":\"" SV_REFRESH "\"}");

	/* Tokens too long for MG_SESSION_MAX_CREDENTIALS: a setup's commit
	 * refuses them; a stored pair that long (the store's token update
	 * alone doesn't measure the document) gets no hello, an error. */
	memset(big, 'a', sizeof(big) - 1);
	CHECK(mg_setup_store_commit(big, sizeof(big) - 1, big, sizeof(big) - 1, K) == -EMSGSIZE);
	CHECK(mg_setup_store_update_tokens(big, sizeof(big) - 1, big, sizeof(big) - 1) == 0);
	reconnect(247, true, true);
	hello(65536, 65536);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_not_found));
	credit(1);
	CHECK(last_error(mg_error_code_invalid_value));

	/* An unsealed hello is insecure on a plain link and
	 * on an encrypted one alike, asks for no encryption, sends nothing and
	 * leaves the proof's nonces: the sealed hello after it opens. */
	connect_fresh(247, true, true, false);
	hello_plain(65536, 65536);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_insecure));
	host_encrypted = true;
	host_notes_clear();
	hello_plain(65536, 65536);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_insecure) && !mg_vm_session_mem_held());
	host_notes_clear();
	hello(65536, 65536);
	CHECK(host_nnotes > 1 && host_notes[0].data[1] == mg_session_hello);

	/* A new challenge drops the match: hello needs a proof again. */
	host_notes_clear();
	const uint8_t ch[17] = {mg_token_proof_challenge};

	mg_token_proof_rx(ch, sizeof(ch));
	host_notes_clear();
	hello(65536, 65536);
	CHECK(last_error(mg_error_code_proof_required));

	/* Limits: the largest max_message and window = max_message are fine. */
	reconnect(247, true, true);
	hello(MG_SESSION_MAX_MESSAGE, MG_SESSION_MAX_MESSAGE);
	CHECK(host_notes[0].data[1] == mg_session_hello);
	host_notes_clear();
	hello(MG_SESSION_MAX_MESSAGE + 1, 1);
	CHECK(last_error(mg_error_code_invalid_value));
	host_notes_clear();
	hello(65536, 65537);
	CHECK(last_error(mg_error_code_invalid_value));
}

/* ---- sending: credit, the queue, retries ---- */

static const char *const INVOKE = "{\"method\":\"link.invoke\",\"id\":\"i\",\"command\":"
				  "\"device.health\",\"params\":{}}";

static void test_send_queue(void)
{
	static uint8_t big[5000];
	struct mg_vm_iov iov = {big, 0};
	int seen = 0;

	printf("  send queue, client max_message, credit\n");
	use_vector_commands();
	/* Not open. */
	connect_fresh(247, true, true, true);
	iov.n = 10;
	CHECK(mg_vm_session_sendv(&iov, 1) == -ENOTCONN);
	hello(4096, 16);
	state(mg_session_state_open, 7);
	CHECK(mg_vm_session_is_open());
	/* Larger than the client takes; empty. */
	iov.n = 4097;
	CHECK(mg_vm_session_sendv(&iov, 1) == -EMSGSIZE);
	iov.n = 0;
	CHECK(mg_vm_session_sendv(&iov, 1) == -EMSGSIZE);
	/* The queue fills (register is in it: the window is 16 bytes). */
	iov.n = 1000;
	int queued = 0;

	while (mg_vm_session_sendv(&iov, 1) == 0) {
		queued++;
	}
	CHECK(mg_vm_session_sendv(&iov, 1) == -ENOSPC);
	CHECK(queued >= 1 && queued <= CONFIG_MG_VM_SESSION_TX_QUEUE / 1000);
	/* Credit trickles them out in order, never more than given: 5 more
	 * bytes of the registration, in one continuation. */
	host_notes_clear();
	credit(5);
	CHECK(host_nnotes == 1 && host_notes[0].len == MG_SESSION_DATA_HEADER + 5 &&
	      host_notes[0].data[2] == mg_session_flag_sealed);
	host_notes_clear();
	credit(1u << 20);
	size_t total = 0;

	for (int i = 0; i < host_nnotes; i++) {
		total += host_notes[i].len;
		CHECK(host_notes[i].len <= 244);
	}
	CHECK(total > (size_t)queued * 1000);
	/* A state other than open drops the queue; data gets an error. */
	state(mg_session_state_closed, 7);
	CHECK(!mg_vm_session_is_open());
	host_notes_clear();
	credit(100);
	CHECK(host_nnotes == 0);

	/* Several results while credit-starved go out in order once it comes. */
	open_session(247, 64);
	credit(0);
	for (int i = 0; i < 3; i++) {
		char inv[160];

		snprintf(inv, sizeof(inv),
			 "{\"method\":\"link.invoke\",\"id\":\"q%d\",\"command\":\"device.health\"}", i);
		vm_send_json(inv);
	}
	host_notes_clear();
	credit(100000);
	seen = 0;
	for (int i = 0; i < 3; i++) {
		char want[40];
		const char *j = next_control(&seen);

		snprintf(want, sizeof(want), "\"id\":\"q%d\"", i);
		CHECK(j != NULL && strstr(j, want) != NULL);
	}
}

/* Credit the device gave in host_notes. */
static uint64_t credit_given(void)
{
	uint64_t c = 0;

	for (int i = 0; i < host_nnotes; i++) {
		const struct host_note *n = &host_notes[i];

		if (n->len == 6 && n->data[0] == mg_command_session &&
		    n->data[1] == mg_session_credit) {
			c += get32(&n->data[2]);
		}
	}
	return c;
}

/* Sends msg as data the way a client keeping to the device's window does:
 * no more message bytes out than credit allows. false if it got stuck. */
static bool send_windowed(const uint8_t *msg, size_t len, uint64_t *window)
{
	static uint8_t sealed[MG_SESSION_MAX_MESSAGE + MGSEAL_OVERHEAD];
	uint8_t f[260];
	size_t off = 0;
	uint8_t flags = 0;

	if (hs_sealed) {
		CHECK(len + MGSEAL_OVERHEAD <= sizeof(sealed));
		memcpy(&sealed[4], msg, len);
		CHECK(mgseal_seal(&hs_keys, MGSEAL_DIR_CLIENT, hs_cctr++, sealed, len) == MGSEAL_OK);
		msg = sealed;
		len += MGSEAL_OVERHEAD;
		flags = mg_session_flag_sealed;
	}
	while (off < len) {
		bool first = off == 0;
		size_t h = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
		size_t n = MIN(MIN(host_mtu - 3 - h, len - off), *window);

		if (n == 0) {
			return false;
		}
		f[0] = mg_command_session;
		f[1] = mg_session_data;
		f[2] = flags | (first ? mg_session_flag_first : 0) |
		       (off + n == len ? mg_session_flag_last : 0);
		if (first) {
			put32((uint32_t)len, &f[3]);
		}
		memcpy(&f[h], &msg[off], n);
		host_notes_clear();
		write(f, h + n);
		*window += credit_given() - n;
		off += n;
	}
	return true;
}

static void test_window(void)
{
	static uint8_t c[3 * CONFIG_MG_VM_SESSION_WINDOW], m[sizeof(c) + 64];
	uint64_t window = CONFIG_MG_VM_SESSION_WINDOW;
	size_t n;
	int seen;

	printf("  a message larger than the device's window finishes\n");
	use_vector_commands();
	open_session(247, 65536);
	/* A chunk on a stream that isn't open, larger than the window. */
	memset(c, 'x', sizeof(c));
	n = vm_chunk(5, c, 2 * CONFIG_MG_VM_SESSION_WINDOW + 100, m, sizeof(m));
	CHECK(n > CONFIG_MG_VM_SESSION_WINDOW && n <= CONFIG_MG_VM_SESSION_MAX_MESSAGE);
	CHECK(send_windowed(m, n, &window));
	/* Every byte back. */
	CHECK(window == CONFIG_MG_VM_SESSION_WINDOW);
	/* One within the window still comes back whole, once handled. */
	n = control(INVOKE, c);
	n = vm_chunk(1, c, n, m, sizeof(m));
	CHECK(send_windowed(m, n, &window));
	CHECK(window == CONFIG_MG_VM_SESSION_WINDOW);
	seen = 0;
	CHECK(next_control(&seen) != NULL);
	/* Aborted halfway: what was held back comes back too. */
	n = vm_chunk(5, c, 2 * CONFIG_MG_VM_SESSION_WINDOW, m, sizeof(m));
	host_notes_clear();
	n += MGSEAL_OVERHEAD;
	uint32_t ctr = hs_cctr++; /* taken at the first fragment, used by the abort */

	write((const uint8_t[]){mg_command_session, mg_session_data,
				mg_session_flag_first | mg_session_flag_sealed, (uint8_t)n,
				(uint8_t)(n >> 8), (uint8_t)(n >> 16), 0, (uint8_t)(ctr >> 24),
				(uint8_t)(ctr >> 16), (uint8_t)(ctr >> 8), (uint8_t)ctr, 1, 2, 3},
	      14);
	window += credit_given() - 7;
	host_notes_clear();
	write((const uint8_t[]){mg_command_session, mg_session_abort}, 2);
	window += credit_given();
	CHECK(window == CONFIG_MG_VM_SESSION_WINDOW);
}

/* Every notification in order, as one byte string: to compare runs. */
static size_t transcript(uint8_t *out, size_t cap)
{
	size_t n = 0;

	for (int i = 0; i < host_nnotes; i++) {
		if (n + host_notes[i].len + 1 <= cap) {
			out[n++] = (uint8_t)host_notes[i].len;
			memcpy(&out[n], host_notes[i].data, host_notes[i].len);
			n += host_notes[i].len;
		}
	}
	return n;
}

static void test_retry(void)
{
	static uint8_t want[8000], got[8000];
	size_t wn, gn;
	int notes;

	printf("  a full notification queue: hello, credentials, credit, saved and data retried\n");
	use_vector_commands();
	/* The reference run: credit, then the result's fragments. */
	open_session(100, 65536);
	vm_send_json(INVOKE);
	wn = transcript(want, sizeof(want));
	notes = host_nnotes;
	CHECK(notes >= 3);
	/* Whichever of them fails first, the same notifications in the end. */
	for (int skip = 0; skip < notes; skip++) {
		open_session(100, 65536);
		host_notify_skip = skip;
		host_notify_fail = 1 + skip % 3;
		vm_send_json(INVOKE);
		host_advance(1000);
		gn = transcript(got, sizeof(got));
		CHECK(gn == wn && memcmp(got, want, wn) == 0);
	}
	/* The hello and the credentials after it: never the credentials
	 * without the hello before them. */
	connect_fresh(100, true, true, true);
	hello(65536, 65536);
	wn = transcript(want, sizeof(want));
	notes = host_nnotes;
	CHECK(notes > 3);
	for (int skip = 0; skip < notes; skip++) {
		reconnect(100, true, true);
		host_notify_skip = skip;
		host_notify_fail = 2;
		hello(65536, 65536);
		CHECK(host_nnotes == skip);
		host_advance(1000);
		gn = transcript(got, sizeof(got));
		CHECK(gn == wn && memcmp(got, want, wn) == 0);
	}
	/* credentials_saved: sent once it can be, the pair committed once. */
	reconnect(100, true, true);
	hello(65536, 65536);
	host_notes_clear();
	host_notify_fail = 3;
	update("{\"access_token\":\"at-r\",\"refresh_token\":\"rt-r\"}");
	CHECK(host_nnotes == 0 && stored_pair("at-r", "rt-r"));
	host_settings_fail_save(1); /* a second commit would fail */
	host_advance(1000);
	CHECK(saved(1));
	host_settings_fail_save(0);
	/* Without a subscribed client nothing is retried: the next write
	 * flushes. */
	reconnect(100, true, true);
	hello(65536, 65536);
	wn = transcript(want, sizeof(want));
	reconnect(100, true, true);
	host_notify_unsubscribed = true;
	hello(65536, 65536);
	host_advance(1000);
	CHECK(host_nnotes == 0);
	host_notify_unsubscribed = false;
	credit(0);
	gn = transcript(got, sizeof(got));
	CHECK(gn == wn && memcmp(got, want, wn) == 0);
	host_notify_skip = host_notify_fail = 0;
}

static void test_proof_ends_session(void)
{
	const uint8_t ch[17] = {mg_token_proof_challenge}, clear[1] = {mg_token_proof_clear};

	printf("  a new challenge or a proof clear ends the session at once\n");
	use_vector_commands();
	/* Credentials still going out when a new challenge comes: they stop. */
	connect_fresh(100, true, true, true);
	host_notify_skip = 1;
	host_notify_fail = 1;
	hello(65536, 65536);
	CHECK(host_nnotes == 1 && host_notes[0].data[1] == mg_session_hello);
	mg_token_proof_rx(ch, sizeof(ch));
	host_notes_clear();
	host_advance(1000);
	CHECK(host_nnotes == 0);
	/* An open session with results waiting for credit: a clear erases
	 * the setup, and none of them go out. */
	open_session(247, 64);
	credit(0);
	vm_send_json(INVOKE);
	CHECK(mg_vm_session_is_open());
	mg_token_proof_rx(clear, sizeof(clear));
	CHECK(!mg_vm_session_is_open() && !mg_setup_store_complete());
	host_notes_clear();
	host_advance(1000);
	credit(100000);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
}

static void test_write_lost(void)
{
	printf("  a lost client write ends a session's connection\n");
	use_vector_commands();
	connect_fresh(247, true, true, true);
	host_disconnects = 0;
	mg_vm_session_write_lost();
	CHECK(host_disconnects == 0); /* no hello: nothing to lose */
	hello(65536, 65536);
	state(mg_session_state_open, 1);
	mg_vm_session_write_lost();
	CHECK(host_disconnects == 1 && !mg_vm_session_is_open());
	host_notes_clear();
	credit(1);
	CHECK(last_error(mg_error_code_invalid_value)); /* the hello is gone */
}

/* ---- link.invoke and /link-control ---- */

static int fail_handler(const struct mg_json_obj *params, struct mg_jw *w, char *err,
			size_t err_len)
{
	(void)params;
	mg_jw_int(w, "partial", 1);
	return mg_vm_fail(err, err_len, "the %s is %s", "lamp", "out");
}

static int silent_fail_handler(const struct mg_json_obj *params, struct mg_jw *w, char *err,
			       size_t err_len)
{
	(void)params;
	(void)w;
	(void)err;
	(void)err_len;
	return -1;
}

static int big_handler(const struct mg_json_obj *params, struct mg_jw *w, char *err,
		       size_t err_len)
{
	static char s[CONFIG_MG_VM_LINK_CONTROL_MAX];

	(void)params;
	(void)err;
	(void)err_len;
	memset(s, 'x', sizeof(s) - 1);
	mg_jw_str(w, "s", s);
	return 0;
}

static const struct mg_vm_param typed_params[] = {
	{"s", MG_VM_STRING, "a string", true},   {"i", MG_VM_INTEGER, NULL, true},
	{"n", MG_VM_NUMBER, NULL, false},        {"b", MG_VM_BOOLEAN, NULL, false},
	{"o", MG_VM_OBJECT, NULL, false},        {"a", MG_VM_ARRAY, NULL, false},
};

static int echo_handler(const struct mg_json_obj *params, struct mg_jw *w, char *err,
			size_t err_len)
{
	(void)err;
	(void)err_len;
	mg_jw_str(w, "s", mg_json_str(params, "s"));
	mg_jw_int(w, "i", mg_json_get(params, "i")->num);
	return 0;
}

static const struct mg_vm_command extra[] = {
	{"test.fail", "Fails.", NULL, 0, 0, fail_handler},
	{"test.silent", "Fails without saying why.", NULL, 0, 0, silent_fail_handler},
	{"test.big", "Too big to answer.", NULL, 0, 0, big_handler},
	{"test.typed", "Typed params.", typed_params, ARRAY_SIZE(typed_params), 1000, echo_handler},
};

static const char *invoke(const char *json)
{
	int seen = 0;

	host_notes_clear();
	vm_send_json(json);
	return next_control(&seen);
}

static int count_runs;

static int count_handler(const struct mg_json_obj *params, struct mg_jw *w, char *err,
			 size_t err_len)
{
	(void)params;
	(void)err;
	(void)err_len;
	mg_jw_int(w, "run", ++count_runs);
	return 0;
}

static const struct mg_vm_command count_command = {"test.count", "Counts.", NULL, 0, 0,
						   count_handler};

static void test_invoke_busy(void)
{
	char inv[120], want[40];
	int sent = 0, ok = 0, busy = 0, seen = 0;
	const char *j;

	printf("  link.invoke while the send queue is full: busy, not run\n");
	use_vector_commands();
	CHECK(mg_vm_link_add_command(&count_command) == 0);
	open_session(247, 64);
	credit(0);
	count_runs = 0;
	/* The client gives no credit: results pile up until one might not
	 * fit; from then on the command doesn't run. */
	for (sent = 0; sent < 20; sent++) {
		snprintf(inv, sizeof(inv),
			 "{\"method\":\"link.invoke\",\"id\":\"c%d\",\"command\":\"test.count\"}",
			 sent);
		vm_send_json(inv);
	}
	CHECK(count_runs > 0 && count_runs < sent);
	host_notes_clear();
	credit(1u << 20);
	for (int i = 0; i < sent; i++) {
		j = next_control(&seen);
		snprintf(want, sizeof(want), "\"id\":\"c%d\"", i);
		if (j == NULL || strstr(j, want) == NULL) {
			break;
		}
		if (strstr(j, "\"ok\":true") != NULL) {
			CHECK(busy == 0); /* in order: the ones that ran first */
			ok++;
		} else if (strstr(j, "\"error\":\"busy\"") != NULL) {
			busy++;
		}
	}
	CHECK(ok == count_runs && busy == sent - count_runs);
	/* With credit flowing it runs again. */
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"z\",\"command\":\"test.count\"}");
	CHECK(j && strstr(j, "\"ok\":true") != NULL);
}


static void test_invoke(void)
{
	static const struct {
		const char *params;
		const char *want;
	} cases[] = {
		{"{\"s\":\"x\",\"i\":3}", "\"ok\":true,\"payload\":{\"s\":\"x\",\"i\":3}}"},
		{"{\"i\":3}", "\"ok\":false,\"error\":\"missing parameter s\"}"},
		{"{\"s\":\"x\"}", "\"ok\":false,\"error\":\"missing parameter i\"}"},
		{"{\"s\":1,\"i\":3}", "\"ok\":false,\"error\":\"parameter s must be a string\"}"},
		{"{\"s\":\"x\",\"i\":3.5}", "\"error\":\"parameter i must be an integer\"}"},
		{"{\"s\":\"x\",\"i\":3,\"n\":\"1\"}", "\"error\":\"parameter n must be a number\"}"},
		{"{\"s\":\"x\",\"i\":3,\"b\":0}", "\"error\":\"parameter b must be a boolean\"}"},
		{"{\"s\":\"x\",\"i\":3,\"o\":[]}", "\"error\":\"parameter o must be an object\"}"},
		{"{\"s\":\"x\",\"i\":3,\"a\":{}}", "\"error\":\"parameter a must be an array\"}"},
		{"{\"s\":\"x\",\"i\":3,\"n\":1.5,\"b\":true,\"o\":{},\"a\":[],\"zz\":null}",
		 "\"ok\":true"},
		{"[1]", "\"error\":\"params must be an object\"}"},
		{"\"x\"", "\"error\":\"params must be an object\"}"},
	};
	char msg[400];
	const char *j;

	printf("  link.invoke: checks, failures, result size\n");
	use_vector_commands();
	for (size_t i = 0; i < ARRAY_SIZE(extra); i++) {
		CHECK(mg_vm_link_add_command(&extra[i]) == 0);
	}
	open_session(247, 65536);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		snprintf(msg, sizeof(msg),
			 "{\"method\":\"link.invoke\",\"id\":\"t%u\",\"command\":\"test.typed\","
			 "\"params\":%s}",
			 (unsigned int)i, cases[i].params);
		j = invoke(msg);
		CHECK(j != NULL && strstr(j, cases[i].want) != NULL);
		if (j == NULL || strstr(j, cases[i].want) == NULL) {
			fprintf(stderr, "    case %u: %s\n", (unsigned int)i, j ? j : "(nothing)");
		}
	}
	/* Absent and null params are none. */
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"h\",\"command\":\"device.health\"}");
	CHECK(j && strcmp(j, "{\"method\":\"link.result\",\"id\":\"h\",\"ok\":true,\"payload\":"
			     "{\"battery\":87,\"overall\":\"ok\"}}") == 0);
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"h\",\"command\":\"device.health\","
		   "\"params\":null}");
	CHECK(j && strstr(j, "\"ok\":true") != NULL);
	/* A handler's failure drops its partial payload. */
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"f\",\"command\":\"test.fail\"}");
	CHECK_STR(j, "{\"method\":\"link.result\",\"id\":\"f\",\"ok\":false,\"error\":"
		     "\"the lamp is out\"}");
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"f\",\"command\":\"test.silent\"}");
	CHECK_STR(j, "{\"method\":\"link.result\",\"id\":\"f\",\"ok\":false,\"error\":"
		     "\"test.silent failed\"}");
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"g\",\"command\":\"test.big\"}");
	CHECK_STR(j, "{\"method\":\"link.result\",\"id\":\"g\",\"ok\":false,\"error\":"
		     "\"result too large\"}");
	/* No command, or no id: answered / ignored. */
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"n\"}");
	CHECK_STR(j, "{\"method\":\"link.result\",\"id\":\"n\",\"ok\":false,\"error\":"
		     "\"no command named \"}");
	j = invoke("{\"method\":\"link.invoke\",\"command\":\"device.health\"}");
	CHECK(j == NULL);
	/* An id that needs escaping is echoed escaped. */
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"a\\\"b\",\"command\":\"device.health\"}");
	CHECK(j && strstr(j, "\"id\":\"a\\\"b\"") != NULL);

	/* Registration refuses bad names and duplicates. */
	static const struct mg_vm_command bad[] = {
		{"Device.x", "d", NULL, 0, 0, echo_handler}, {"nodot", "d", NULL, 0, 0, echo_handler},
		{"a..b", "d", NULL, 0, 0, echo_handler},     {"a.b.", "d", NULL, 0, 0, echo_handler},
		{"a.b", "d", NULL, 0, 0, NULL},              {"a.b", NULL, NULL, 0, 0, echo_handler},
	};
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		CHECK(mg_vm_link_add_command(&bad[i]) == -EINVAL);
	}
	CHECK(mg_vm_link_add_command(&extra[0]) == -EEXIST);
}

static void test_control_framing(void)
{
	static uint8_t c[3000], m[3200];
	static const char a[] = "{\"method\":\"link.invoke\",\"id\":\"A\",\"command\":"
				"\"device.health\"}";
	static const char b[] = "{\"method\":\"link.invoke\",\"id\":\"B\",\"command\":"
				"\"device.health\"}";
	size_t n;
	int seen;

	printf("  /link-control framing: split, joined, oversized, other streams\n");
	use_vector_commands();
	open_session(247, 65536);

	/* Two control messages in one chunk: two results. */
	n = control(a, c);
	n += control(b, &c[n]);
	host_notes_clear();
	send_frag(mg_session_data, m, vm_chunk(1, c, n, m, sizeof(m)));
	seen = 0;
	CHECK(strstr(next_control(&seen), "\"id\":\"A\"") != NULL);
	CHECK(strstr(next_control(&seen), "\"id\":\"B\"") != NULL);

	/* One split over three chunks, the length itself split. */
	n = control(a, c);
	host_notes_clear();
	send_frag(mg_session_data, m, vm_chunk(1, c, 2, m, sizeof(m)));
	send_frag(mg_session_data, m, vm_chunk(1, &c[2], 10, m, sizeof(m)));
	CHECK(next_control(&(int){0}) == NULL);
	send_frag(mg_session_data, m, vm_chunk(1, &c[12], n - 12, m, sizeof(m)));
	seen = 0;
	CHECK(strstr(next_control(&seen), "\"id\":\"A\"") != NULL);

	/* Too long to keep: skipped, the next one still handled. */
	n = 4 + CONFIG_MG_VM_LINK_CONTROL_MAX;
	put32((uint32_t)n - 4, c);
	memset(&c[4], ' ', n - 4);
	host_notes_clear();
	for (size_t off = 0; off < n; off += 1000) {
		send_frag(mg_session_data, m,
			  vm_chunk(1, &c[off], MIN((size_t)1000, n - off), m, sizeof(m)));
	}
	send_frag(mg_session_data, m, vm_chunk(1, c, control(b, c), m, sizeof(m)));
	seen = 0;
	CHECK(strstr(next_control(&seen), "\"id\":\"B\"") != NULL);

	/* Not JSON, an event, another stream, a reset, an undecodable message:
	 * credit back, nothing else. */
	static const char *const quiet[] = {
		"not json", "{\"type\":\"event\",\"event\":\"link.unpaired\"}",
		"{\"method\":\"link.something\"}", "{\"type\":\"res\",\"id\":\"reg-1\"}",
	};
	for (size_t i = 0; i < ARRAY_SIZE(quiet); i++) {
		host_notes_clear();
		vm_send_json(quiet[i]);
		CHECK(host_nnotes == 1 && host_notes[0].data[1] == mg_session_credit);
	}
	n = control(a, c);
	host_notes_clear();
	send_frag(mg_session_data, m, vm_chunk(3, c, n, m, sizeof(m)));
	CHECK(host_nnotes == 1 && host_notes[0].data[1] == mg_session_credit);
	host_notes_clear();
	send_frag(mg_session_data, (const uint8_t *)"\xff\xff\xff", 3);
	/* Not a service envelope: invalid_value, its credit back. */
	CHECK(host_nnotes == 2 && host_notes[0].data[0] == mg_command_error &&
	      host_notes[1].data[1] == mg_session_credit);
	const struct mg_env_frame rst = {.stream_id = 1, .kind = MG_ENV_RESET,
					 .reset_code = MG_ENV_RESET_CANCELLED};

	n = (size_t)mg_env_encode_response_message(&rst, m, sizeof(m));
	host_notes_clear();
	send_frag(mg_session_data, m, n);
	CHECK(host_nnotes == 1 && !mg_vm_link_registered());
	/* A refused /link-control: logged only. */
	const struct mg_env_frame no = {.stream_id = 1, .kind = MG_ENV_RESPONSE, .status = 403};

	n = (size_t)mg_env_encode_response_message(&no, m, sizeof(m));
	host_notes_clear();
	send_frag(mg_session_data, m, n);
	CHECK(host_nnotes == 1);
}

static bool open_requests(const uint8_t *want, int n)
{
	if (host_nnotes != n) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		const uint8_t m[] = {mg_command_session, mg_session_open_request, want[i]};

		if (host_notes[i].len != 3 || memcmp(host_notes[i].data, m, 3) != 0) {
			return false;
		}
	}
	return true;
}

/* How many notifications, all open_request [1]; -1 if any other. */
static int only_open_requests(void)
{
	for (int i = 0; i < host_nnotes; i++) {
		const uint8_t m[] = {mg_command_session, mg_session_open_request, 1};

		if (host_notes[i].len != 3 || memcmp(host_notes[i].data, m, 3) != 0) {
			return -1;
		}
	}
	return host_nnotes;
}

static void vm_frame(const struct mg_env_frame *f)
{
	static uint8_t m[256];
	int n = mg_env_encode_response_message(f, m, sizeof(m));

	CHECK(n > 0);
	send_frag(mg_session_data, m, (size_t)n);
}

/* The 200 response to POST /link-control carries control messages in its own
 * body too: read like a body chunk's (as the ESP32 reads them). */
static void test_control_response_body(void)
{
	static uint8_t c[512], m[600];
	size_t cn;
	int n;

	printf("  /link-control: messages in the 200 response's own body are read\n");
	use_vector_commands();
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	state(mg_session_state_open, 1);
	CHECK(!mg_vm_link_registered());
	cn = control("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\","
		     "\"node_id\":\"" SV_NODE_ID "\"}}",
		     c);
	const struct mg_env_frame ok = {
		.stream_id = 1, .kind = MG_ENV_RESPONSE, .status = 200, .body = {c, cn}};

	n = mg_env_encode_response_message(&ok, m, sizeof(m));
	CHECK(n > 0);
	send_frag(mg_session_data, m, n > 0 ? (size_t)n : 0);
	CHECK(mg_vm_link_registered());
}

static void test_control_failure(void)
{
	const struct mg_env_frame reset = {.stream_id = 1, .kind = MG_ENV_RESET, .reset_code = 2};
	const struct mg_env_frame refused = {.stream_id = 1, .kind = MG_ENV_RESPONSE, .status = 403};
	const struct mg_env_frame other = {.stream_id = 3, .kind = MG_ENV_RESET, .reset_code = 2};
	int seen = 0;

	printf("  /link-control reset or refused: a new session, with backoff\n");
	use_vector_commands();
	open_session(247, 65536);
	/* Another stream's reset: nothing. */
	vm_frame(&other);
	host_notes_clear();
	host_advance(60000);
	CHECK(host_nnotes == 0 && mg_vm_link_registered());
	/* Reset: open_request 0 then 1 after a second, no heartbeat. */
	vm_frame(&reset);
	CHECK(!mg_vm_link_registered());
	host_notes_clear();
	host_advance(999);
	CHECK(host_nnotes == 0);
	host_advance(1);
	CHECK(open_requests((const uint8_t[]){0, 1}, 2));
	host_notes_clear();
	host_advance((int64_t)CONFIG_MG_VM_LINK_HEARTBEAT_S * 2000);
	CHECK(host_nnotes == 0);
	/* The client reopens: /link-control and link.register again. */
	state(mg_session_state_closed, 1);
	state(mg_session_state_open, 2);
	CHECK(next_control(&seen) == NULL); /* the POST */
	CHECK(strstr(next_control(&seen), "\"method\":\"link.register\"") != NULL);
	/* Refused this time: two seconds (the backoff doubles)... */
	vm_frame(&refused);
	host_notes_clear();
	host_advance(1999);
	CHECK(host_nnotes == 0);
	host_advance(1);
	CHECK(open_requests((const uint8_t[]){0, 1}, 2));
	/* ...and the close goes, the open waits for the queue: a state closed
	 * in between doesn't drop it. */
	state(mg_session_state_open, 3);
	vm_frame(&refused);
	host_notes_clear();
	host_notify_skip = 1;
	host_notify_fail = 1;
	host_advance(4000);
	CHECK(open_requests((const uint8_t[]){0}, 1));
	state(mg_session_state_closed, 3);
	host_advance(1000);
	CHECK(open_requests((const uint8_t[]){0, 1}, 2));
	/* A registered session that stays up 60 s resets the backoff (it was
	 * 8 s by now). */
	state(mg_session_state_open, 4);
	vm_send_json("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}");
	CHECK(mg_vm_link_registered());
	host_advance(60000);
	vm_frame(&reset);
	host_notes_clear();
	host_advance(1000);
	CHECK(open_requests((const uint8_t[]){0, 1}, 2));
	/* A session that ends before the link's request goes sends none of the
	 * link's (close, open): the state it didn't ask for is re-asked with
	 * open_request [1] (Session state), and again while the client
	 * doesn't answer: 0, 1, 3, 7, 15, 30, 45, 60 s. */
	state(mg_session_state_open, 5);
	vm_frame(&reset);
	state(mg_session_state_no_network, 5);
	host_notes_clear();
	host_advance(60000);
	CHECK(only_open_requests() == 8);
	host_notify_skip = host_notify_fail = 0;
}

/* A clean end of /link-control alone asks nothing (the
 * client reports closed, and the session's re-ask reopens: one path). */
static void test_control_clean_end(void)
{
	const struct mg_env_frame end = {.stream_id = 1, .kind = MG_ENV_BODY_CHUNK,
					 .end_body = true};

	printf("  /link-control ended cleanly: no open_request of its own\n");
	use_vector_commands();
	open_session(247, 65536);
	CHECK(mg_vm_link_registered());
	vm_frame(&end);
	host_notes_clear();
	host_advance(20000);
	CHECK(host_nnotes == 0);
	/* The client reports closed: the session's re-ask, at once. */
	state(mg_session_state_closed, 2);
	host_advance(1);
	CHECK(open_requests((const uint8_t[]){1}, 1));
}

/* ---- the board's commands ---- */

static void test_board_commands(void)
{
	static const uint8_t addr[6] = {0x02, 0, 0, 0, 0x1a, 0x2b};
	const char *j;
	int seen = 0;

	printf("  the board's commands: device.health, device.led\n");
	mg_identity_init("MuseGadget-", "", addr);
	mg_vm_link_test_clear();
	CHECK(mg_vm_commands_init() == 0);
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	state(mg_session_state_open, 1);
	CHECK(next_control(&seen) == NULL); /* the POST */
	j = next_control(&seen);
	CHECK(j && strstr(j, "\"display_name\":\"MuseGadget-001A2B\",\"platform\":\"zephyr\","
			     "\"version\":\"1.0.0\",\"device_family\":\"gadget\","
			     "\"model_id\":\"muse-gadget-xiao-nrf54l15\"") != NULL);
	/* The id stored with this setup's K (CONFIG_MG_NODE_ID_GADGET off here:
	 * homelink-; test_handover_node_id has gadget-). */
	CHECK(j && strstr(j, "\"node_id\":\"homelink-001a2b\"") != NULL);
	CHECK(j && strstr(j, "\"device.health\":{\"description\":\"Report basic gadget health") !=
			   NULL);
	CHECK(j && strstr(j, "\"device.led\":{\"description\":\"Set the gadget's LED.") != NULL);
	CHECK(j && strstr(j, "\"required\":{\"r\":{\"type\":\"integer\",\"description\":"
			     "\"Red 0-255\"},\"g\":") != NULL);
	CHECK(j && strstr(j, "\"optional\":{\"duration_ms\":{\"type\":\"integer\"") != NULL);
	CHECK(j && strstr(j, "\"timeout_ms\":5000}") != NULL);

	/* device.led */
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"1\",\"command\":\"device.led\","
		   "\"params\":{\"r\":255,\"g\":0,\"b\":0}}");
	CHECK_STR(j, "{\"method\":\"link.result\",\"id\":\"1\",\"ok\":true,\"payload\":{\"on\":true,"
		     "\"duration_ms\":5000}}");
	CHECK(led_command == MG_LED_COMMAND_ON && led_ms == 5000);
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"2\",\"command\":\"device.led\","
		   "\"params\":{\"r\":0,\"g\":0,\"b\":0,\"duration_ms\":100}}");
	CHECK(j && strstr(j, "\"payload\":{\"on\":false,\"duration_ms\":100}") != NULL);
	CHECK(led_command == MG_LED_COMMAND_OFF && led_ms == 100);
	led_command = -1;
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"3\",\"command\":\"device.led\","
		   "\"params\":{\"r\":256,\"g\":0,\"b\":0}}");
	CHECK(j && strstr(j, "\"error\":\"parameter r must be 0-255\"") != NULL);
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"4\",\"command\":\"device.led\","
		   "\"params\":{\"r\":1,\"g\":0,\"b\":0,\"duration_ms\":0}}");
	CHECK(j && strstr(j, "\"error\":\"parameter duration_ms must be 1-60000\"") != NULL);
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"5\",\"command\":\"device.led\","
		   "\"params\":{\"r\":1,\"b\":0}}");
	CHECK(j && strstr(j, "\"error\":\"missing parameter g\"") != NULL);
	CHECK(led_command == -1);

	/* device.health, without and with a battery reading. */
	host_now_ms = 125000;
	battery_mv = -1;
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"6\",\"command\":\"device.health\"}");
	CHECK_STR(j, "{\"method\":\"link.result\",\"id\":\"6\",\"ok\":true,\"payload\":"
		     "{\"overall\":\"ok\",\"levels\":{},\"critical\":[],\"warnings\":[],"
		     "\"metrics\":{\"uptime_seconds\":125,\"battery_pct\":null,\"battery_mv\":null,"
		     "\"charging\":null,\"usb_power\":null},\"version\":\"1.0.0\"}}");
	battery_mv = 3900;
	j = invoke("{\"method\":\"link.invoke\",\"id\":\"7\",\"command\":\"device.health\"}");
	CHECK(j && strstr(j, "\"battery_pct\":87,\"battery_mv\":3900") != NULL);
}

/* The credentials name the node
 * id stored with the current pair: gadget- for a setup since, the kept
 * homelink- for one from before, never device_info's new offer. */
static bool notes_have(const char *text)
{
	size_t n = strlen(text);

	if (strstr(credentials_sent(), text)) {
		return true;
	}
	for (int i = 0; i < host_nnotes; i++) {
		for (size_t o = 0; o + n <= host_notes[i].len; o++) {
			if (memcmp(&host_notes[i].data[o], text, n) == 0) {
				return true;
			}
		}
	}
	return false;
}

static uint8_t done_rec[32];
static size_t done_len;

static int done_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		   void *param)
{
	(void)param;
	if (key == NULL && len <= sizeof(done_rec)) {
		done_len = (size_t)read_cb(cb_arg, done_rec, len);
	}
	return 0;
}

static void test_handover_node_id(void)
{
	static const uint8_t addr[6] = {0x02, 0, 0, 0, 0x1a, 0x2b};

	printf("  credentials name the stored node id: gadget-, or the kept homelink-\n");
	mg_identity_set_offer_gadget(true); /* CONFIG_MG_NODE_ID_GADGET on */
	mg_identity_init("MuseGadget-", "", addr);
	mg_vm_link_test_clear();
	CHECK(mg_vm_commands_init() == 0); /* the board's identity, as the firmware */
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	CHECK(notes_have("\"node_id\":\"gadget-001a2b\""));
	CHECK(!notes_have("homelink-"));
	/* A setup from before the gadget- form: the same record without the flag. */
	done_len = 0;
	(void)settings_load_subtree_direct("mg/setup/done", done_cb, NULL);
	CHECK(done_len > 5 && (done_rec[4] & 0x10));
	done_rec[4] &= (uint8_t)~0x10;
	CHECK(settings_save_one("mg/setup/done", done_rec, done_len) == 0);
	(void)mg_setup_store_load();
	CHECK_STR(mg_identity_offer_node_id(), "gadget-001a2b"); /* what device_info offers */
	reconnect(247, true, true);
	hello(65536, 65536);
	CHECK(notes_have("\"node_id\":\"homelink-001a2b\""));
	CHECK(!notes_have("gadget-"));
	mg_vm_link_test_clear();
	mg_identity_set_offer_gadget(false);
}

/* ---- sealed credentials ---- */

static void hello_sealed_flags(uint8_t flags)
{
	uint8_t m[12] = {mg_command_session, mg_session_hello, MG_SESSION_VERSION};

	put32(65536, &m[3]);
	put32(65536, &m[7]);
	m[11] = flags;
	hs_sealed = false; /* the caller seals and opens with its own keys */
	write(m, sizeof(m));
}

/* An older app's sealed hello (no sealed_saved: the legacy answer). */
static void hello_sealed(void)
{
	hello_sealed_flags(mg_session_hello_flag_sealed);
}

/* A newer app's: sealed credentials_saved asked for. */
static void hello_sealed_large(void)
{
	hello_sealed_flags(mg_session_hello_flag_sealed | mg_session_hello_flag_sealed_saved);
}

/* The client's keys for the proof prove() runs. */
static void client_keys(mgseal_keys_t *keys)
{
	CHECK(mgseal_keys(keys, K, client_nonce, device_nonce, SV_NODE_ID, strlen(SV_NODE_ID)) ==
	      MGSEAL_OK);
}

/* The sealed credentials the device sent (every fragment flagged sealed,
 * total_len the sealed length), opened with counter ctr; NULL if anything
 * is off. */
static const char *sealed_credentials(const mgseal_keys_t *keys, uint32_t ctr)
{
	static uint8_t buf[2200];
	size_t len = 0, total = 0, plen = 0;
	bool flags_ok = true;

	for (int i = 0; i < host_nnotes; i++) {
		const struct host_note *n = &host_notes[i];

		if (n->len < 3 || n->data[0] != mg_command_session ||
		    n->data[1] != mg_session_credentials) {
			continue;
		}
		flags_ok &= (n->data[2] & mg_session_flag_sealed) != 0;
		size_t h = (n->data[2] & mg_session_flag_first) ? 7 : 3;

		if (n->data[2] & mg_session_flag_first) {
			total = get32(&n->data[3]);
		}
		memcpy(&buf[len], &n->data[h], n->len - h);
		len += n->len - h;
	}
	if (!flags_ok || len == 0 || len != total ||
	    mgseal_open(keys, MGSEAL_DIR_DEVICE, ctr, buf, len, &plen) != MGSEAL_OK) {
		return NULL;
	}
	buf[4 + plen] = '\0';
	return (const char *)&buf[4];
}

/* A sealed credentials_update: json sealed with counter ctr in dir. */
static void update_sealed(const mgseal_keys_t *keys, uint8_t dir, uint32_t ctr, const char *json,
			  bool flip_tag)
{
	static uint8_t buf[2200];
	size_t n = strlen(json);

	memcpy(&buf[4], json, n);
	CHECK(mgseal_seal(keys, dir, ctr, buf, n) == MGSEAL_OK);
	if (flip_tag) {
		buf[4 + n + 3] ^= 0x40;
	}
	send_frag_flags(mg_session_credentials_update, mg_session_flag_sealed, buf,
			n + MGSEAL_OVERHEAD);
}

/* A sealed receive counter given back opens a hole only within 255 of the
 * next one (the reference receiver's limits, mg-session-seal-v1.json's
 * receiver_limits), through the session: a hole (an earlier message refused
 * by its tag after a later counter was taken) is accepted once while it's
 * within 255 back, and closes once the receive counter has moved further. */
static void hole_make(const mgseal_keys_t *keys)
{
	static const char json[] = "{\"access_token\":\"h1\",\"refresh_token\":\"h2\"}";
	static uint8_t buf[128];
	uint8_t f[64], c[MGSEAL_OVERHEAD + 1];
	size_t n = strlen(json), len = n + MGSEAL_OVERHEAD, k = 20;

	/* credentials_update on counter 0, its tag spoiled: the first fragment
	 * takes counter 0 ... */
	memcpy(&buf[4], json, n);
	CHECK(mgseal_seal(keys, MGSEAL_DIR_CLIENT, 0, buf, n) == MGSEAL_OK);
	buf[len - 1] ^= 1;
	f[0] = mg_command_session;
	f[1] = mg_session_credentials_update;
	f[2] = mg_session_flag_first | mg_session_flag_sealed;
	put32((uint32_t)len, &f[3]);
	memcpy(&f[MG_SESSION_FIRST_HEADER], buf, k);
	write(f, MG_SESSION_FIRST_HEADER + k);
	/* ... a sealed clear with another plaintext takes counter 1 for good ... */
	c[4] = 2;
	CHECK(mgseal_seal(keys, MGSEAL_DIR_CLIENT, 1, c, 1) == MGSEAL_OK);
	send_frag_flags(mg_session_clear, mg_session_flag_sealed, c, sizeof(c));
	/* ... and the update's last fragment fails its tag: counter 0 is the hole. */
	f[2] = mg_session_flag_last | mg_session_flag_sealed;
	memcpy(&f[MG_SESSION_DATA_HEADER], &buf[k], len - k);
	write(f, MG_SESSION_DATA_HEADER + len - k);
	host_notes_clear();
}

static void test_hole_limit(void)
{
	mgseal_keys_t keys;
	uint8_t c[MGSEAL_OVERHEAD + 1];

	printf("  sealed receive: a hole once within 255 counters back, never after\n");
	use_vector_commands();
	host_settings_clear();
	(void)mg_setup_store_load();
	store_credentials(false);
	client_keys(&keys);
	saved_keys = &keys;

	/* Within the window: the hole's counter is taken once. */
	reconnect(247, true, false);
	hello_sealed();
	host_notes_clear();
	hole_make(&keys);
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"a5\",\"refresh_token\":\"r5\"}",
		      false);
	CHECK(saved(1) && stored_pair("a5", "r5"));

	/* The edge (mg-session-seal-v1.json receiver_limits, 255): a clear at
	 * 254 (the receive counter 255, the hole 255 back) keeps the hole; one
	 * at 255 (256 back) closes it. */
	for (int at = 254; at <= 255; at++) {
		reconnect(247, true, false);
		hello_sealed();
		host_notes_clear();
		hole_make(&keys);
		mg_vm_session_test_set_counters((uint32_t)at, -1);
		c[4] = 2;
		CHECK(mgseal_seal(&keys, MGSEAL_DIR_CLIENT, (uint32_t)at, c, 1) == MGSEAL_OK);
		send_frag_flags(mg_session_clear, mg_session_flag_sealed, c, sizeof(c));
		host_notes_clear();
		update_sealed(&keys, MGSEAL_DIR_CLIENT, 0,
			      at == 254 ? "{\"access_token\":\"a7\",\"refresh_token\":\"r7\"}"
					: "{\"access_token\":\"a8\",\"refresh_token\":\"r8\"}",
			      false);
		if (at == 254) {
			CHECK(stored_pair("a7", "r7"));
		} else {
			CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
			CHECK(stored_pair("a7", "r7"));
		}
	}

	/* The receive counter 300 on: the hole fell more than 255 back. */
	reconnect(247, true, false);
	hello_sealed();
	host_notes_clear();
	hole_make(&keys);
	mg_vm_session_test_set_counters(300, -1);
	c[4] = 2; /* any message taking counter 300 moves the window */
	CHECK(mgseal_seal(&keys, MGSEAL_DIR_CLIENT, 300, c, 1) == MGSEAL_OK);
	send_frag_flags(mg_session_clear, mg_session_flag_sealed, c, sizeof(c));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"a6\",\"refresh_token\":\"r6\"}",
		      false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	CHECK(stored_pair("a7", "r7"));
	saved_keys = NULL;
}

static void test_sealed(void)
{
	static const char want[] = "{\"node_id\":\"" SV_NODE_ID "\",\"access_token\":\"" SV_ACCESS
				   "\",\"refresh_token\":\"" SV_REFRESH "\"}";
	mgseal_keys_t keys;
	const char *j;

	printf("  sealed: hello on a plain link, one session per proof, sealed updates\n");
	use_vector_commands();
	host_settings_clear();
	(void)mg_setup_store_load();
	store_credentials(false);
	client_keys(&keys);
	saved_keys = &keys;

	/* Before a proof: proof_required, sealed or not. */
	reconnect(247, false, false);
	hello_sealed();
	CHECK(host_nnotes == 1 && last_error(mg_error_code_proof_required));

	/* After a matching proof, on a plain link: no insecure; the hello
	 * echoes the flag and the credentials are sealed. */
	reconnect(247, true, false);
	hello_sealed();
	CHECK(host_nnotes > 1 && host_notes[0].len == 12 &&
	      host_notes[0].data[1] == mg_session_hello &&
	      host_notes[0].data[11] == mg_session_hello_flag_sealed);
	j = sealed_credentials(&keys, 0);
	CHECK(j != NULL);
	CHECK_STR(j ? j : "", want);
	/* Nothing in the clear: the JSON's tokens never go out unsealed. */
	for (int i = 0; i < host_nnotes; i++) {
		CHECK(memmem(host_notes[i].data, host_notes[i].len, SV_ACCESS, 8) == NULL);
	}

	/* A second sealed hello under the same proof: proof_required, and the
	 * open session keeps its keys (an update still opens). */
	host_notes_clear();
	hello_sealed();
	CHECK(host_nnotes == 1 && last_error(mg_error_code_proof_required));

	/* A sealed update, counter 0: saved. */
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"a2\",\"refresh_token\":\"r2\"}",
		      false);
	CHECK(saved(1) && stored_pair("a2", "r2"));

	/* The same counter again (a replay), a flipped tag, the reflected
	 * direction, and an unsealed update: invalid_value, nothing saved. */
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"a3\",\"refresh_token\":\"r3\"}",
		      false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 1, "{\"access_token\":\"a3\",\"refresh_token\":\"r3\"}",
		      true);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_DEVICE, 1, "{\"access_token\":\"a3\",\"refresh_token\":\"r3\"}",
		      false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	host_notes_clear();
	/* Unsealed in a sealed session: insecure. */
	update("{\"access_token\":\"a3\",\"refresh_token\":\"r3\"}");
	CHECK(last_error(mg_error_code_insecure));
	CHECK(stored_pair("a2", "r2"));

	/* A refused tag (the flip, the reflected direction) doesn't use its
	 * counter; a good tag does, whatever the plaintext: a sealed
	 * non-object at counter 1 is invalid_value and uses it. */
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 1, "[1]", false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 2, "{\"access_token\":\"a4\",\"refresh_token\":\"r4\"}",
		      false);
	CHECK(saved(1) && stored_pair("a4", "r4"));

	/* The apps add "access_expires_at" (a number) and may add other flat
	 * members: ignored, the pair saved. Both tokens are still required. */
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 3,
		      "{\"access_token\":\"a6\",\"refresh_token\":\"r6\",\"access_expires_at\":1793874000,"
		      "\"x_future\":\"y\",\"flag\":true}",
		      false);
	CHECK(saved(1) && stored_pair("a6", "r6"));
	/* access_expires_at is kept with the pair. */
	CHECK(mg_setup_store_expires() == 1793874000ull);
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 4,
		      "{\"access_token\":\"a7\",\"access_expires_at\":1793874000}", false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value) && stored_pair("a6", "r6"));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 5,
		      "{\"refresh_token\":\"r7\",\"access_expires_at\":1793874000}", false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value) && stored_pair("a6", "r6"));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 6,
		      "{\"access_token\":1,\"refresh_token\":\"r7\"}", false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value) && stored_pair("a6", "r6"));

	/* A new proof (the K setup left is unchanged by an update): a sealed
	 * hello again, counters from 0. */
	prove();
	host_notes_clear();
	hello_sealed();
	j = sealed_credentials(&keys, 0);
	CHECK(j != NULL && strstr(j, "\"access_token\":\"a6\"") != NULL);
	/* ... and handed back, last, in the next credentials. */
	CHECK(j != NULL && strstr(j, ",\"access_expires_at\":1793874000}") != NULL);
	/* An update without it clears it. */
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"a6\",\"refresh_token\":\"r6\"}",
		      false);
	CHECK(saved(1) && mg_setup_store_expires() == 0);

	/* A plain hello is insecure on any link. */
	reconnect(247, true, false);
	hello_plain(65536, 65536);
	CHECK(last_error(mg_error_code_insecure));

	/* No session (its unsealed hello refused) takes no sealed
	 * update. */
	reconnect(247, true, true);
	hello_plain(65536, 65536);
	CHECK(last_error(mg_error_code_insecure));
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"a5\",\"refresh_token\":\"r5\"}",
		      false);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	CHECK(stored_pair("a6", "r6"));

	/* A sealed hello on an encrypted link is sealed too. */
	reconnect(247, true, true);
	hello_sealed();
	CHECK(host_notes[0].len == 12 && sealed_credentials(&keys, 0) != NULL);
	mgseal_wipe(&keys, sizeof(keys));
	saved_keys = NULL;
}

/* ---- memory: the session's block only while a session is open ---- */

static void test_memory(void)
{
	printf("  memory: allocated at a hello, freed at the connection's end\n");
	use_vector_commands();
	connect_fresh(247, true, true, true);
	CHECK(!mg_vm_session_mem_held());
	hello(65536, 65536);
	CHECK(mg_vm_session_mem_held() && host_notes[0].data[1] == mg_session_hello);
	reconnect(247, true, true);
	CHECK(!mg_vm_session_mem_held());

	/* A refused hello holds nothing. */
	reconnect(247, false, true);
	hello(65536, 65536);
	CHECK(last_error(mg_error_code_proof_required) && !mg_vm_session_mem_held());
	reconnect(247, true, true);
	hello_plain(65536, 65536);
	CHECK(last_error(mg_error_code_insecure) && !mg_vm_session_mem_held());

	reconnect(247, true, false);
	hello_sealed();

	/* A second sealed hello is refused and the open session keeps its
	 * memory; a new challenge frees it. */
	host_notes_clear();
	hello_sealed();
	CHECK(last_error(mg_error_code_proof_required) && mg_vm_session_mem_held());
	const uint8_t ch[17] = {mg_token_proof_challenge};

	mg_token_proof_rx(ch, sizeof(ch));
	CHECK(!mg_vm_session_mem_held());
}

/* ---- the relay's placeholders: anything inbound taken, a test trigger out ---- */

static void test_relay_placeholders(void)
{
	static uint8_t m[512];
	int seen = 0;
	uint64_t before;
	int drops;
	const uint8_t blob[] = {1, 2, 3, 4, 5};

	printf("  relay: unknown inbound taken with credit back, the test trigger\n");
	use_vector_commands();
	open_session(247, 65536);
	drops = host_disconnects;
	/* A frame for a stream that isn't open and an unknown control message:
	 * taken, no error, credit back. (A message that isn't a service envelope
	 * is refused, checked above.) */
	send_frag(mg_session_data, m, vm_chunk(7, blob, sizeof(blob), m, sizeof(m)));
	vm_send_json("{\"type\":\"event\",\"event\":\"agent.hello\"}");
	vm_send_json("{\"type\":\"mystery\",\"method\":\"agent.ping\",\"x\":1}");
	host_advance(100);
	before = credit_given();
	CHECK(before > 0);
	for (int i = 0; i < host_nnotes; i++) {
		CHECK(host_notes[i].data[0] != mg_command_error);
	}
	CHECK(host_disconnects == drops && mg_vm_link_registered());
	/* The test trigger: one control message, a heartbeat. */
	host_notes_clear();
	CHECK(mg_vm_link_send_test() == 0);
	host_advance(100);
	const char *j = next_control(&seen);

	CHECK(j != NULL && strcmp(j, "{\"method\":\"link.heartbeat\"}") == 0);
	/* No session: it waits for the next open (mgcommands.h Session state),
	 * at most 10 s; never after the connection ends. */
	reconnect(247, true, true);
	host_notes_clear();
	CHECK(mg_vm_link_send_test() == 1 && host_nnotes == 0);
	mg_vm_session_connection_reset();
	host_advance(1000);
	CHECK(host_nnotes == 0);
}

/* ---- sealed credentials_saved (the vectors) ---- */

static void test_sealed_saved(void)
{
	int k;

	printf("  sealed credentials_saved: the vectors byte for byte, saved and not saved\n");
	use_vector_commands();
	connect_fresh(247, true, true, false);
	hello_sealed_large(); /* the credentials: device counter 0 */
	CHECK(mg_vm_session_test_set_keys(SC_K, SC_CLIENT_NONCE, SC_DEVICE_NONCE, SC_NODE_ID) == 0);
	host_advance(100);
	for (size_t i = 0; i < SC_NSTEPS; i++) {
		const struct sc_step *st = &SC_STEPS[i];

		host_notes_clear();
		if (!st->saved) {
			host_settings_fail_save(1); /* the commit fails: sealed [0] */
		}
		for (size_t f = 0; f < st->nframes; f++) {
			write(st->frames[f].p, st->frames[f].n);
		}
		host_advance(100);
		host_settings_fail_save(0);
		for (k = 0; k < host_nnotes; k++) {
			if (host_notes[k].len == st->answer_n &&
			    memcmp(host_notes[k].data, st->answer, st->answer_n) == 0) {
				break;
			}
		}
		CHECK(k < host_nnotes);
		if (k == host_nnotes) {
			fprintf(stderr, "    %s\n", st->name);
			for (k = 0; k < host_nnotes; k++) {
				dump("got   ", host_notes[k].data, host_notes[k].len);
			}
			dump("want  ", st->answer, st->answer_n);
		}
	}
	/* A sealed session whose hello didn't carry sealed_saved keeps the
	 * one-byte answer. */
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	host_notes_clear();
	update("{\"access_token\":\"a9\",\"refresh_token\":\"r9\"}");
	CHECK(saved(1));
}

/* Opt-in: a sealed hello without
 * sealed_saved gets the flag echoed without it and every credentials_saved
 * as the legacy unsealed byte, taking no counter, so an older app's relay
 * never falls behind; with it, the echo carries both flags. A plain hello's
 * sealed_saved bit alone changes nothing. */
static void test_sealed_saved_opt_in(void)
{
	mgseal_keys_t keys;
	int seen = 0;
	static uint8_t msg[600];

	printf("  sealed credentials_saved only when the hello asks (sealed_saved flag)\n");
	use_vector_commands();
	store_credentials(false);
	client_keys(&keys);
	saved_keys = &keys;
	/* An older app's sealed hello: echo 0x01, the legacy answer twice, then the
	 * device's data on the counter right after link.register's. */
	reconnect(247, true, false);
	hello_sealed();
	CHECK(host_notes[0].len == 12 && host_notes[0].data[11] == mg_session_hello_flag_sealed);
	state(mg_session_state_open, 1); /* POST 1, link.register 2 */
	host_advance(100);
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0,
		      "{\"access_token\":\"la1\",\"refresh_token\":\"lr1\"}", false);
	host_advance(100);
	CHECK(saved(1) && host_notes[0].len == 3);
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 1,
		      "{\"access_token\":\"la2\",\"refresh_token\":\"lr2\"}", false);
	host_advance(100);
	CHECK(saved(1) && host_notes[0].len == 3);
	host_notes_clear();
	CHECK(mg_vm_link_send_test() == 0);
	host_advance(100);
	size_t len = next_device_msg(&seen, msg, sizeof(msg));
	uint32_t dctr = len >= 4 ? (uint32_t)msg[0] << 24 | (uint32_t)msg[1] << 16 |
					   (uint32_t)msg[2] << 8 | msg[3]
				 : 0;

	CHECK(dctr == 3);
	/* A newer app's hello (a new proof first: one sealed session per proof): echo
	 * 0x03, and the answer sealed. */
	reconnect(247, true, false);
	hello_sealed_large();
	CHECK(host_notes[0].len == 12 &&
	      host_notes[0].data[11] ==
		      (mg_session_hello_flag_sealed | mg_session_hello_flag_sealed_saved));
	host_advance(100);
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0,
		      "{\"access_token\":\"sa1\",\"refresh_token\":\"sr1\"}", false);
	host_advance(100);
	CHECK(saved(1) && host_notes[0].len != 3);
	/* sealed_saved without sealed is an unsealed hello: insecure. */
	reconnect(247, true, true);
	hello_sealed_flags(mg_session_hello_flag_sealed_saved);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_insecure));
	saved_keys = NULL;
}

/* The counter a sealed device message took (its first four bytes). */
static uint32_t note_ctr(const struct host_note *h)
{
	const uint8_t *b = &h->data[MG_SESSION_FIRST_HEADER];

	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* sealed_data_large: every sealed step's bytes from its
 * plaintext (the device's credentials_saved, result || hash, included), then
 * the client's frames into the device: refusals as the vectors say, the
 * update answered sealed on the next shared device counter, and the device's
 * next data on the one after it. */
static void test_sealed_data_large(void)
{
	static uint8_t buf[600], msg[600];
	mgseal_keys_t vk;
	uint32_t next_ctr = 3; /* the credentials 0, POST /link-control 1, link.register 2 */
	int seen = 0, saved_seen = 0;

	printf("  sealed data with a sealed credentials_saved (sealed_data_large)\n");
	CHECK(mgseal_keys(&vk, SLARGE_K, SLARGE_CLIENT_NONCE, SLARGE_DEVICE_NONCE, SLARGE_NODE_ID,
			  strlen(SLARGE_NODE_ID)) == MGSEAL_OK);
	for (size_t i = 0; i < SLARGE_NSTEPS; i++) {
		const struct sd_step *st = &SLARGE_STEPS[i];

		if (st->sealed_n == 0 || strcmp(st->outcome, "refused") == 0) {
			continue;
		}
		memcpy(&buf[4], st->pt, st->pt_n);
		CHECK(mgseal_seal(&vk, (uint8_t)st->dir, (uint32_t)st->counter, buf, st->pt_n) ==
		      MGSEAL_OK);
		CHECK(st->sealed_n == st->pt_n + MGSEAL_OVERHEAD &&
		      memcmp(buf, st->sealed, st->sealed_n) == 0);
	}
	use_vector_commands();
	connect_fresh(SLARGE_MAX_WRITE + 3, true, true, false);
	hello_sealed_large();
	state(mg_session_state_open, 1);
	CHECK(mg_vm_session_test_set_keys(SLARGE_K, SLARGE_CLIENT_NONCE, SLARGE_DEVICE_NONCE,
					  SLARGE_NODE_ID) == 0);
	host_advance(100);
	saved_keys = &vk;
	for (size_t i = 0; i < SLARGE_NSTEPS; i++) {
		const struct sd_step *st = &SLARGE_STEPS[i];

		if (st->dir != MGSEAL_DIR_CLIENT) {
			continue;
		}
		host_notes_clear();
		for (size_t f = 0; f < st->nframes; f++) {
			write(st->frames[f].p, st->frames[f].n);
		}
		host_advance(100);
		if (strcmp(st->outcome, "refused") == 0) {
			bool found = false;

			for (int n = 0; n < host_nnotes; n++) {
				found |= host_notes[n].len == st->answer_n &&
					 memcmp(host_notes[n].data, st->answer, st->answer_n) == 0;
			}
			CHECK(found);
		} else if (strstr(st->name, "credentials_update") != NULL) {
			CHECK(saved(1) && note_ctr(&host_notes[0]) == next_ctr);
			next_ctr++;
			saved_seen++;
		}
	}
	CHECK(saved_seen == 1);
	/* The device's next data takes the counter after the sealed answer. */
	host_notes_clear();
	CHECK(mg_vm_link_send_test() == 0);
	host_advance(100);
	size_t len = next_device_msg(&seen, msg, sizeof(msg)), plen = 0;
	uint32_t dctr = len >= 4 ? (uint32_t)msg[0] << 24 | (uint32_t)msg[1] << 16 |
					   (uint32_t)msg[2] << 8 | msg[3]
				 : 0;

	CHECK(dctr == next_ctr &&
	      mgseal_open(&vk, MGSEAL_DIR_DEVICE, dctr, msg, len, &plen) == MGSEAL_OK);
	saved_keys = NULL;
}

/* Invariant: no sealed device counter goes out twice,
 * nor out of order, even with a data message sealed and partly written (its
 * first fragment out, the rest held by a full queue) when a
 * credentials_update's sealed answer is made: the data took its counter
 * when it was sealed, the answer takes the next one and goes after it. */
/* A sealed data first fragment with counter ctr (and nothing valid after
 * it: refused at the counter, or dropped at a later check). */
static void data_first_ctr(uint32_t ctr)
{
	uint8_t m[3 + 4 + 4 + 8] = {mg_command_session, mg_session_data,
				    mg_session_flag_first | mg_session_flag_sealed};

	put32(4 + 8 + MGSEAL_OVERHEAD, &m[3]);
	m[7] = (uint8_t)(ctr >> 24);
	m[8] = (uint8_t)(ctr >> 16);
	m[9] = (uint8_t)(ctr >> 8);
	m[10] = (uint8_t)ctr;
	write(m, sizeof(m));
}

/* Three counter refusals in a row end the sealed session (the portable
 * core's MGDS_CTR_END): tag refusals and an accepted
 * counter reset the run; the end is complete (a new proof and hello, from 0). */
static void test_ctr_end(void)
{
	mgseal_keys_t keys;
	unsigned int ends = mg_vm_session_ctr_ends();

	printf("  sealed: three counter refusals in a row end the session\n");
	use_vector_commands();
	host_settings_clear();
	(void)mg_setup_store_load();
	store_credentials(false);
	client_keys(&keys);
	saved_keys = &keys;
	reconnect(247, true, false);
	hello_sealed();
	state(mg_session_state_open, 1); /* data needs an open session */
	host_advance(100);
	host_notes_clear();
	/* In sync at 0, then the relay one ahead (the cut-fragment epoch): 2, 3,
	 * 4 refused; the third ends it. */
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"c0\",\"refresh_token\":\"d0\"}",
		      false);
	CHECK(saved(1));
	for (uint32_t c = 2; c <= 3; c++) {
		host_notes_clear();
		data_first_ctr(c);
		CHECK(had_error(mg_error_code_invalid_value));
		CHECK(mg_vm_session_ctr_ends() == ends);
	}
	host_notes_clear();
	data_first_ctr(4);
	CHECK(had_error(mg_error_code_invalid_value));
	CHECK(mg_vm_session_ctr_ends() == ends + 1);
	CHECK(!mg_vm_session_mem_held());
	/* Ended: a sealed hello under the old proof is proof_required. */
	host_notes_clear();
	hello_sealed();
	CHECK(host_nnotes == 1 && last_error(mg_error_code_proof_required));
	/* A new proof and hello: counters from 0 again. */
	reconnect(247, true, false);
	client_keys(&keys);
	hello_sealed();
	state(mg_session_state_open, 1);
	host_advance(100);
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 0, "{\"access_token\":\"c1\",\"refresh_token\":\"d1\"}",
		      false);
	CHECK(saved(1) && stored_pair("c1", "d1"));
	/* Two counter refusals, then a tag refusal (its counter acceptable: the
	 * run resets), then two more: not ended. */
	data_first_ctr(9);
	data_first_ctr(9);
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 1, "{\"access_token\":\"c2\",\"refresh_token\":\"d2\"}",
		      true);
	CHECK(last_error(mg_error_code_invalid_value));
	data_first_ctr(9);
	data_first_ctr(9);
	CHECK(mg_vm_session_ctr_ends() == ends + 1);
	/* An accepted counter resets the run too: then three credentials_update
	 * counter refusals end it. */
	host_notes_clear();
	update_sealed(&keys, MGSEAL_DIR_CLIENT, 1, "{\"access_token\":\"c3\",\"refresh_token\":\"d3\"}",
		      false);
	CHECK(saved(1));
	for (int i = 0; i < 3; i++) {
		update_sealed(&keys, MGSEAL_DIR_CLIENT, 7,
			      "{\"access_token\":\"c4\",\"refresh_token\":\"d4\"}", false);
	}
	CHECK(mg_vm_session_ctr_ends() == ends + 2);
	CHECK(stored_pair("c3", "d3"));
	saved_keys = NULL;
}

static void test_sealed_counters_unique(void)
{
	uint32_t seen_ctr[64];
	int n_ctr = 0, saved_at = -1;

	printf("  sealed: no device counter twice (data part-written, then a sealed answer)\n");
	use_vector_commands();
	connect_fresh(247, true, true, false);
	hello_sealed_large();
	CHECK(mg_vm_session_test_set_keys(SC_K, SC_CLIENT_NONCE, SC_DEVICE_NONCE, SC_NODE_ID) == 0);
	host_advance(100);
	host_notes_clear();
	state(mg_session_state_open, 1); /* POST and link.register: counters 1 and 2 */
	host_advance(100);
	host_mtu = 40;           /* the test message in several small fragments: */
	host_notify_skip = 1;    /* its first goes... */
	host_notify_fail = 1000; /* ...and nothing after it, for now */
	CHECK(mg_vm_link_send_test() == 0);
	host_mtu = 247;          /* (the client's update needs whole writes) */
	for (size_t f = 0; f < SC_STEPS[0].nframes; f++) {
		write(SC_STEPS[0].frames[f].p, SC_STEPS[0].frames[f].n);
	}
	host_advance(50);
	host_notify_skip = host_notify_fail = 0;
	host_advance(1000);
	for (int k = 0; k < host_nnotes && n_ctr < 64; k++) {
		const struct host_note *h = &host_notes[k];

		if (h->len < MG_SESSION_FIRST_HEADER + 4 || h->data[0] != mg_command_session ||
		    !(h->data[2] & mg_session_flag_first) || !(h->data[2] & mg_session_flag_sealed) ||
		    (h->data[1] != mg_session_data && h->data[1] != mg_session_credentials &&
		     h->data[1] != mg_session_credentials_saved)) {
			continue;
		}
		if (h->data[1] == mg_session_credentials_saved) {
			saved_at = n_ctr;
		}
		seen_ctr[n_ctr++] = note_ctr(h);
	}
	CHECK(n_ctr >= 4 && saved_at == n_ctr - 1);
	for (int a = 1; a < n_ctr; a++) {
		CHECK(seen_ctr[a] == seen_ctr[a - 1] + 1); /* each once, in order */
	}
	CHECK(n_ctr >= 1 && seen_ctr[0] == 1);
}

/* The sealed device messages' counters in host_notes, in the order they went
 * out (each first fragment once); saved_at: the credentials_saved's index. */
static int sent_ctrs(uint32_t *ctr, int cap, int *saved_at)
{
	int n = 0;

	*saved_at = -1;
	for (int k = 0; k < host_nnotes && n < cap; k++) {
		const struct host_note *h = &host_notes[k];

		if (h->len < MG_SESSION_FIRST_HEADER + 4 || h->data[0] != mg_command_session ||
		    !(h->data[2] & mg_session_flag_first) || !(h->data[2] & mg_session_flag_sealed) ||
		    (h->data[1] != mg_session_data && h->data[1] != mg_session_credentials_saved)) {
			continue;
		}
		if (h->data[1] == mg_session_credentials_saved) {
			*saved_at = n;
		}
		ctr[n++] = note_ctr(h);
	}
	return n;
}

/* A sealed credentials_saved held back by a full notification queue goes out
 * before data sealed after it: send order is counter order. */
static void test_sealed_saved_backpressure(void)
{
	uint32_t ctr[32];
	int n, saved_at;

	printf("  sealed: a credentials_saved held by a full queue goes before later data\n");
	use_vector_commands();
	connect_fresh(247, true, true, false);
	hello_sealed_large();
	CHECK(mg_vm_session_test_set_keys(SC_K, SC_CLIENT_NONCE, SC_DEVICE_NONCE, SC_NODE_ID) == 0);
	host_advance(100);
	state(mg_session_state_open, 1); /* POST and link.register: counters 1 and 2 */
	host_advance(100);
	host_notes_clear();
	for (size_t f = 0; f < SC_STEPS[0].nframes; f++) {
		if (f + 1 == SC_STEPS[0].nframes) {
			host_notify_fail = 1000; /* the sealed answer (counter 3) waits */
		}
		write(SC_STEPS[0].frames[f].p, SC_STEPS[0].frames[f].n);
	}
	host_advance(50);
	CHECK(mg_vm_link_send_test() == 0); /* sealed on counter 4, queued */
	host_advance(50);
	host_notify_skip = host_notify_fail = 0;
	host_advance(1000);
	n = sent_ctrs(ctr, 32, &saved_at);
	CHECK(n == 2 && saved_at == 0);
	for (int a = 0; a < n; a++) {
		CHECK(ctr[a] == 3u + (uint32_t)a);
	}
}

/* Messages queued but not started when the session closes give their
 * counters back: after the reopen the next sealed message takes the first of
 * them, so the client sees no gap. */
static void test_sealed_reopen_no_gap(void)
{
	uint32_t ctr[32];
	int n, saved_at;

	printf("  sealed: a close drops queued messages without skipping a counter\n");
	use_vector_commands();
	connect_fresh(247, true, true, false);
	hello_sealed_large();
	CHECK(mg_vm_session_test_set_keys(SC_K, SC_CLIENT_NONCE, SC_DEVICE_NONCE, SC_NODE_ID) == 0);
	host_advance(100);
	state(mg_session_state_open, 1); /* counters 1 and 2 */
	host_advance(100);
	host_notify_fail = 1000;
	CHECK(mg_vm_link_send_test() == 0); /* counter 3, never started */
	CHECK(mg_vm_link_send_test() == 0); /* counter 4, never started */
	state(mg_session_state_closed, 1);
	host_notify_fail = 0;
	host_notes_clear();
	state(mg_session_state_open, 2); /* POST /link-control again */
	host_advance(100);
	n = sent_ctrs(ctr, 32, &saved_at);
	CHECK(n >= 1 && ctr[0] == 3);
}

/* ---- sealed data (the sealed_data vectors) ---- */

static void test_sealed_data(void)
{
	static uint8_t buf[600], msg[600];
	mgseal_keys_t vk;
	int seen = 0;

	printf("  sealed data: the vectors, both directions, the counter rules\n");
	CHECK(mgseal_keys(&vk, SD_K, SD_CLIENT_NONCE, SD_DEVICE_NONCE, SD_NODE_ID,
			  strlen(SD_NODE_ID)) == MGSEAL_OK);
	saved_keys = &vk;
	/* The seal itself, every sealed step, both directions. */
	for (size_t i = 0; i < SD_NSTEPS; i++) {
		const struct sd_step *st = &SD_STEPS[i];

		/* A refused step's sealed bytes may be damaged on purpose. */
		if (st->sealed_n == 0 || strcmp(st->outcome, "refused") == 0) {
			continue;
		}
		memcpy(&buf[4], st->pt, st->pt_n);
		CHECK(mgseal_seal(&vk, (uint8_t)st->dir, (uint32_t)st->counter, buf, st->pt_n) ==
		      MGSEAL_OK);
		CHECK(st->sealed_n == st->pt_n + MGSEAL_OVERHEAD &&
		      memcmp(buf, st->sealed, st->sealed_n) == 0);
	}

	/* The device at ATT_MTU 100: a sealed session (its credentials at
	 * counter 0, link.register at 1), then the vectors' keys. */
	use_vector_commands();
	connect_fresh(SD_MAX_WRITE + 3, true, true, false);
	hello_sealed();
	state(mg_session_state_open, 1);
	CHECK(mg_vm_session_test_set_keys(SD_K, SD_CLIENT_NONCE, SD_DEVICE_NONCE, SD_NODE_ID) == 0);
	host_advance(100);

	/* The client's frames, frame by frame, with the device's answers. */
	for (size_t i = 0; i < SD_NSTEPS; i++) {
		const struct sd_step *st = &SD_STEPS[i];

		if (st->dir != MGSEAL_DIR_CLIENT) {
			continue;
		}
		host_notes_clear();
		for (size_t f = 0; f < st->nframes; f++) {
			CHECK(st->frames[f].n <= SD_MAX_WRITE);
			write(st->frames[f].p, st->frames[f].n);
		}
		host_advance(100);
		bool err = false;

		for (int n = 0; n < host_nnotes; n++) {
			err |= host_notes[n].data[0] == mg_command_error;
		}
		if (strcmp(st->outcome, "refused") == 0) {
			/* The answer, then the dropped message's credit. */
			bool found = false;

			for (int n = 0; n < host_nnotes; n++) {
				found |= host_notes[n].len == st->answer_n &&
					 memcmp(host_notes[n].data, st->answer, st->answer_n) == 0;
			}
			CHECK(found && credit_given() > 0);
		} else {
			/* Data whose plaintext isn't a service envelope is taken
			 * by the seal (its counter used) and then refused as data;
			 * a seal vector's plaintext may be arbitrary bytes. */
			struct mg_env_bytes pl;
			bool env = strstr(st->name, "credentials_update") != NULL ||
				   mg_env_decode_response_envelope(st->pt, st->pt_n, &pl) == 0;

			CHECK(err == !env); /* refused exactly when it isn't an envelope */
			if (strstr(st->name, "credentials_update") != NULL) {
				/* An older app's hello: the legacy byte, no
				 * counter. */
				CHECK(saved(1) && host_notes[0].len == 3);
			}
		}
	}
	/* The device's data: sealed whole under its next counter (3: the
	 * credentials used 0, the POST /link-control request 1, link.register
	 * 2; then one per sealed credentials_saved), every fragment flagged,
	 * each write within the MTU. */
	host_notes_clear();
	CHECK(mg_vm_link_send_test() == 0);
	host_advance(100);
	for (int n = 0; n < host_nnotes; n++) {
		const struct host_note *h = &host_notes[n];

		if (h->len >= 3 && h->data[0] == mg_command_session &&
		    h->data[1] == mg_session_data) {
			CHECK(h->data[2] & mg_session_flag_sealed);
			CHECK(h->len <= SD_MAX_WRITE);
		}
	}
	size_t len = next_device_msg(&seen, msg, sizeof(msg)), plen = 0;

	uint32_t dctr = len >= 4 ? (uint32_t)msg[0] << 24 | (uint32_t)msg[1] << 16 |
					   (uint32_t)msg[2] << 8 | msg[3]
				 : 0;

	CHECK(dctr == 3 && len > MGSEAL_OVERHEAD &&
	      mgseal_open(&vk, MGSEAL_DIR_DEVICE, dctr, msg, len, &plen) == MGSEAL_OK);
	const char *j = plen ? control_json(&msg[4], plen) : NULL;

	CHECK(j != NULL && strcmp(j, "{\"method\":\"link.heartbeat\"}") == 0);
	/* A bad tag: refused, credit back, the counter not used. */
	host_notes_clear();
	/* A service envelope (client data must be one). */
	int en = mg_env_encode_response_envelope((const uint8_t *)"{}", 2, &buf[4], 32);

	CHECK(en > 0);
	CHECK(mgseal_seal(&vk, MGSEAL_DIR_CLIENT, 6, buf, en) == MGSEAL_OK);
	buf[en + MGSEAL_OVERHEAD - 1] ^= 1;
	send_frag_flags(mg_session_data, mg_session_flag_sealed, buf, en + MGSEAL_OVERHEAD);
	host_advance(100);
	CHECK(credit_given() > 0);
	{
		bool found = false;

		for (int n = 0; n < host_nnotes; n++) {
			const uint8_t e[] = {mg_command_error, mg_command_session,
					     mg_error_data_code, mg_error_code_invalid_value, 0};

			found |= host_notes[n].len == sizeof(e) &&
				 memcmp(host_notes[n].data, e, sizeof(e)) == 0;
		}
		CHECK(found);
	}
	host_notes_clear();
	en = mg_env_encode_response_envelope((const uint8_t *)"{}", 2, &buf[4], 32);
	CHECK(mgseal_seal(&vk, MGSEAL_DIR_CLIENT, 6, buf, en) == MGSEAL_OK);
	send_frag_flags(mg_session_data, mg_session_flag_sealed, buf, en + MGSEAL_OVERHEAD);
	host_advance(100);
	for (int n = 0; n < host_nnotes; n++) {
		CHECK(host_notes[n].data[0] != mg_command_error);
	}
	reconnect(247, true, true);
	saved_keys = NULL;
}

/* ---- two sealed messages in flight (sealed_interleaved) ---- */

static void test_sealed_interleaved(void)
{
	printf("  sealed data: two in flight, the earlier refused at its tag (sealed_interleaved)\n");
	use_vector_commands();
	connect_fresh(SI_MAX_WRITE + 3, true, true, false);
	hello_sealed();
	state(mg_session_state_open, 1);
	CHECK(mg_vm_session_test_set_keys(SI_K, SI_CLIENT_NONCE, SI_DEVICE_NONCE, SI_NODE_ID) == 0);
	host_advance(100);
	static mgseal_keys_t si_keys;

	CHECK(mgseal_keys(&si_keys, SI_K, SI_CLIENT_NONCE, SI_DEVICE_NONCE, SI_NODE_ID,
			  strlen(SI_NODE_ID)) == MGSEAL_OK);
	saved_keys = &si_keys;
	/* Every step's frames, frame by frame, with the device's answers as the
	 * reference receiver's: nothing for a partial message, credentials_saved
	 * [1] for an update, no error for data, the answer for a refused one. */
	for (size_t i = 0; i < SI_NSTEPS; i++) {
		const struct sd_step *st = &SI_STEPS[i];
		bool err = false, found = false, is_update = false;

		CHECK(st->dir == MGSEAL_DIR_CLIENT);
		host_notes_clear();
		for (size_t f = 0; f < st->nframes; f++) {
			CHECK(st->frames[f].n <= SI_MAX_WRITE);
			is_update |= f == 0 && st->frames[f].n > 1 && st->frames[f].p[1] == mg_session_credentials_update;
			write(st->frames[f].p, st->frames[f].n);
		}
		host_advance(100);
		for (int n = 0; n < host_nnotes; n++) {
			err |= host_notes[n].data[0] == mg_command_error;
			found |= host_notes[n].len == st->answer_n && st->answer_n &&
				 memcmp(host_notes[n].data, st->answer, st->answer_n) == 0;
		}
		if (strcmp(st->outcome, "partial") == 0) {
			if (host_nnotes != 0) {
				printf("    step %zu (%s): %d notifications\n", i, st->name, host_nnotes);
			}
			CHECK(host_nnotes == 0);
		} else if (strcmp(st->outcome, "message") == 0) {
			/* Data whose plaintext isn't a service envelope: taken by
			 * the seal, then refused as data. */
			struct mg_env_bytes pl;
			bool env = is_update ||
				   mg_env_decode_response_envelope(st->pt, st->pt_n, &pl) == 0;

			if (err && env) {
				printf("    step %zu (%s): refused\n", i, st->name);
			}
			CHECK(err == !env); /* refused exactly when it isn't an envelope */
			if (is_update) {
				CHECK(saved(1));
			}
		} else {
			CHECK(strcmp(st->outcome, "refused") == 0);
			if (!found) {
				printf("    step %zu (%s): not refused\n", i, st->name);
			}
			CHECK(found);
		}
	}
	/* The real update's pair is the one kept. */
	{
		static char at[2100], rt[2100];
		size_t al = 0, rl = 0;

		CHECK(mg_setup_store_get_tokens(at, sizeof(at), &al, rt, sizeof(rt), &rl) == 0 &&
		      al == strlen("mga_interleaved_access") && memcmp(at, "mga_interleaved_access", al) == 0);
	}
	reconnect(247, true, true);
	saved_keys = NULL;
}

static void setup_all(void)
{
	CHECK(mg_psa_init() == 0);
	host_log_verbose = getenv("HOST_VERBOSE") != NULL;
	mg_vm_session_init();
	/* The vectors and these tests don't model the keepalive (an empty
	 * /link-control frame every 15 s while a task is active). */
	mg_vm_link_test_keepalive(0);
}

/* mgcommands.h: a sealed message's first fragment carries the whole
 * 4-byte counter. The device waits for 4 bytes of credit before starting
 * one, and refuses a shorter sealed first fragment with invalid_length. */
static void test_sealed_first_fragment(void)
{
	static uint8_t msg[4300];
	int data = 0, seen = 0;

	printf("  sealed first fragments carry the whole counter, both ways\n");
	use_vector_commands();
	/* A sealed hello's window under 4 is invalid_value. */
	connect_fresh(247, true, true, true);
	hello(65536, 3);
	CHECK(host_nnotes == 1 && last_error(mg_error_code_invalid_value));
	reconnect(247, true, true);
	hello(65536, 4); /* the client's window: one counter */
	host_notes_clear();
	state(mg_session_state_open, 1); /* the POST and link.register queue */
	/* The POST's first fragment is its counter alone. */
	CHECK(host_nnotes >= 1 && host_notes[host_nnotes - 1].data[1] == mg_session_data &&
	      host_notes[host_nnotes - 1].len == MG_SESSION_FIRST_HEADER + 4);
	uint32_t post = get32(&host_notes[host_nnotes - 1].data[3]);

	/* Credit for the rest of the POST and 3 bytes more: the POST ends and
	 * link.register doesn't start. */
	host_notes_clear();
	credit(post - 4 + 3);
	for (int i = 0; i < host_nnotes; i++) {
		if (host_notes[i].data[1] == mg_session_data &&
		    (host_notes[i].data[2] & mg_session_flag_first)) {
			data++;
		}
	}
	CHECK(data == 0);
	host_notes_clear();
	credit(1);
	CHECK(host_nnotes == 1 && host_notes[0].data[1] == mg_session_data &&
	      host_notes[0].data[2] == (mg_session_flag_first | mg_session_flag_sealed) &&
	      host_notes[0].len == MG_SESSION_FIRST_HEADER + 4);
	credit(1u << 20);
	CHECK(next_device_msg(&seen, msg, sizeof(msg)) > 0); /* link.register, whole */

	/* A sealed first fragment with 3 bytes of its counter: invalid_length,
	 * its credit back, no counter used (the next message takes 0's
	 * successor as usual). */
	host_notes_clear();
	write((const uint8_t[]){mg_command_session, mg_session_data,
				mg_session_flag_first | mg_session_flag_sealed, 40, 0, 0, 0, 0, 0, 0},
	      10);
	CHECK(had_error(mg_error_code_invalid_length) && credit_given() == 3);
	host_notes_clear();
	vm_send_json("{\"method\":\"link.heartbeat\"}");
	CHECK(!had_error(mg_error_code_invalid_value) && !had_error(mg_error_code_insecure));
}

#if defined(CONFIG_MG_SESSION_SEALED_CLEAR)
/* Sends mg_session_clear sealed with plaintext pt on counter ctr (tag
 * flipped when bad). */
static void sealed_clear(uint8_t pt, uint32_t ctr, bool bad)
{
	uint8_t b[MGSEAL_OVERHEAD + 1];

	b[4] = pt;
	CHECK(mgseal_seal(&hs_keys, MGSEAL_DIR_CLIENT, ctr, b, 1) == MGSEAL_OK);
	if (bad) {
		b[sizeof(b) - 1] ^= 1;
	}
	send_frag_flags(mg_session_clear, mg_session_flag_sealed, b, sizeof(b));
}

/* The device's sealed clear answer, opened: its plaintext byte and counter;
 * -1 if none. */
static int clear_answer(uint32_t *ctr)
{
	static uint8_t b[64];
	size_t len = 0, plen = 0;

	for (int i = 0; i < host_nnotes; i++) {
		const struct host_note *n = &host_notes[i];

		if (n->len >= 3 && n->data[0] == mg_command_session &&
		    n->data[1] == mg_session_clear) {
			size_t h = (n->data[2] & mg_session_flag_first) ? 7 : 3;

			CHECK(n->data[2] & mg_session_flag_sealed);
			memcpy(&b[len], &n->data[h], n->len - h);
			len += n->len - h;
		}
	}
	if (len != MGSEAL_OVERHEAD + 1) {
		return -1;
	}
	*ctr = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	if (mgseal_open(&hs_keys, MGSEAL_DIR_DEVICE, *ctr, b, len, &plen) != MGSEAL_OK || plen != 1) {
		return -1;
	}
	return b[4];
}

/* Forget's clear is taken only as
 * the sealed mg_session_clear; the unsealed token proof clear, an unsealed
 * session clear, a bad tag or another plaintext erase nothing. */
static void test_sealed_clear(void)
{
	uint32_t ctr = 99;

	printf("  Forget's clear only inside the sealed session (mg_session_clear)\n");
	use_vector_commands();
	/* The unsealed token proof clear, in the hello's order: no token
	 * not_found, before a match proof_required, then insecure. */
	connect_fresh(247, false, false, true);
	host_notes_clear();
	mg_token_proof_rx((const uint8_t[]){mg_token_proof_clear}, 1);
	CHECK(host_nnotes == 1 && host_notes[0].data[1] == mg_command_token_proof &&
	      (host_notes[0].data[3] | host_notes[0].data[4] << 8) == mg_error_code_not_found);
	connect_fresh(247, true, false, true);
	host_notes_clear();
	mg_token_proof_rx((const uint8_t[]){mg_token_proof_clear}, 1);
	CHECK(host_nnotes == 1 && host_notes[0].data[1] == mg_command_token_proof &&
	      (host_notes[0].data[3] | host_notes[0].data[4] << 8) ==
		      mg_error_code_proof_required);
	CHECK(mg_setup_store_complete());
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	host_notes_clear();
	mg_token_proof_rx((const uint8_t[]){mg_token_proof_clear}, 1);
	CHECK(host_nnotes == 1 && host_notes[0].data[0] == mg_command_error &&
	      host_notes[0].data[1] == mg_command_token_proof &&
	      (host_notes[0].data[3] | host_notes[0].data[4] << 8) == mg_error_code_insecure);
	CHECK(mg_setup_store_complete());
	host_notes_clear();
	send_frag_flags(mg_session_clear, 0, (const uint8_t[]){0}, 1);
	CHECK(last_error(mg_error_code_insecure) && mg_setup_store_complete());
	host_notes_clear();
	sealed_clear(0, hs_cctr, true); /* a bad tag: the counter comes back */
	CHECK(last_error(mg_error_code_invalid_value) && mg_setup_store_complete());
	host_notes_clear();
	sealed_clear(5, hs_cctr++, false); /* another plaintext: its counter used */
	CHECK(last_error(mg_error_code_invalid_value) && mg_setup_store_complete());
	host_notes_clear();
	sealed_clear(0, hs_cctr++, false);
	CHECK(clear_answer(&ctr) == 1 && ctr == 1); /* the credentials took 0 */
	CHECK(!mg_setup_store_complete() && !mg_token_proof_matched());
	host_notes_clear();
	hello(65536, 65536);
	CHECK(last_error(mg_error_code_not_found));

	/* With data queued and its first message started (window 4: the POST's
	 * counter alone went): erased at once, the answer waits for the POST
	 * and takes the next unstarted counter; what wasn't started never goes
	 * (mgcommands.h). */
	connect_fresh(247, true, true, true);
	hello(65536, 4);
	state(mg_session_state_open, 1);
	host_notes_clear();
	sealed_clear(0, hs_cctr++, false);
	CHECK(!mg_setup_store_complete() && clear_answer(&ctr) == -1);
	credit(4096);
	host_advance(0);
	CHECK(clear_answer(&ctr) == 1 && ctr == 2); /* 0 credentials, 1 the POST */
	host_notes_clear();
	credit(4096);
	host_advance(0);
	CHECK(host_nnotes == 0 || last_error(mg_error_code_invalid_value)); /* ended */

	/* A clear whose first fragment arrives mid credentials_update: busy,
	 * its counter used; the next one, on the next counter, goes. */
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	static const char j[] = "{\"access_token\":\"a\",\"refresh_token\":\"r\"}";
	uint8_t u[64], f[80] = {mg_command_session, mg_session_credentials_update,
				mg_session_flag_first | mg_session_flag_sealed};
	size_t n = strlen(j) + MGSEAL_OVERHEAD;

	memcpy(&u[4], j, strlen(j));
	CHECK(mgseal_seal(&hs_keys, MGSEAL_DIR_CLIENT, hs_cctr++, u, strlen(j)) == MGSEAL_OK);
	put32((uint32_t)n, &f[3]);
	memcpy(&f[7], u, 5); /* the counter and one byte: the update is open */
	write(f, 12);
	host_notes_clear();
	sealed_clear(0, hs_cctr++, false);
	CHECK(last_error(mg_error_code_busy) && mg_setup_store_complete());
	f[2] = mg_session_flag_last | mg_session_flag_sealed; /* the update's rest */
	memcpy(&f[3], &u[5], n - 5);
	write(f, 3 + n - 5);
	host_notes_clear();
	sealed_clear(0, hs_cctr++, false);
	CHECK(clear_answer(&ctr) == 1 && !mg_setup_store_complete());

	/* The erase fails: answered [0], still sealed. */
	connect_fresh(247, true, true, true);
	hello(65536, 65536);
	host_notes_clear();
	host_settings_fail_delete(1);
	sealed_clear(0, hs_cctr++, false);
	host_settings_fail_delete(0);
	CHECK(clear_answer(&ctr) == 0 && ctr == 1);
}

HOST_MAIN(setup_all, test_sealed_clear)
#else
HOST_MAIN(setup_all, test_sealed_first_fragment, test_vectors, test_token_update, test_token_update_bound, test_credentials,
	  test_sealed, test_memory, test_relay_placeholders, test_sealed_data, test_sealed_interleaved, test_sealed_saved, test_sealed_saved_opt_in, test_sealed_data_large, test_sealed_counters_unique, test_sealed_saved_backpressure, test_sealed_reopen_no_gap, test_hole_limit, test_ctr_end,
	  test_send_queue, test_window, test_retry, test_proof_ends_session, test_write_lost,
	  test_invoke, test_invoke_busy, test_control_framing, test_control_response_body, test_control_failure, test_control_clean_end,
	  test_board_commands, test_handover_node_id)
#endif
