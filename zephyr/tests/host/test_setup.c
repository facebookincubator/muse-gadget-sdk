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
 * Muse Link setup on the gadget, driven as the Muse app drives it: mg_setup.c
 * with real PSA crypto, the real store over an in-memory settings fake, and
 * the app's side computed here. The gadget's identity, firmware version and
 * ephemeral key are those of the community_v5 vector, so pairing_ready must
 * carry the vector's transcript hash and session id, and the vector's own
 * client_finished record must open.
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "check.h"
#include "host_fakes.h"
#include "mg_led_pattern.h"
#include "mg_connparam.h"
#include "mg_identity.h"
#include "mg_json.h"
#include "mg_ota_policy.h"
#include "mg_psa.h"
#include "mg_session_seal.h"
#include "mg_setup.h"
#include "mg_resetup_policy.h"
#include "mg_setup_crypto.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_VM_SESSION)
#include "mg_vm_link.h"
#include "mg_vm_session.h"
#endif
#include "vectors.h"

static const struct link_vec *V;
static uint8_t dev_tx[32], dev_rx[32]; /* the device's keys = the app's rx / tx */
static uint64_t app_ctr, dev_ctr;
static int seen; /* notes consumed */

/* ---- app side ---- */

static void write_chunked(const char *msg, size_t chunk)
{
	size_t len = strlen(msg);
	size_t total = (len + chunk - 1) / chunk;
	uint8_t pkt[256];

	for (size_t i = 0; i < total; i++) {
		size_t n = MIN(chunk, len - i * chunk);

		pkt[0] = 0xFE;
		pkt[1] = (uint8_t)i;
		pkt[2] = (uint8_t)total;
		memcpy(&pkt[3], &msg[i * chunk], n);
		mg_setup_rx(pkt, n + 3);
	}
}

static void app_send(const char *msg)
{
	if (strlen(msg) <= 100) {
		mg_setup_rx((const uint8_t *)msg, strlen(msg));
	} else {
		write_chunked(msg, 150);
	}
}

/* The next message from the device: a reassembled chunked one, or a raw
 * plaintext status. NULL if none is waiting. */
static char *next_msg(void)
{
	static char buf[4096];
	size_t len = 0;

	while (seen < host_nnotes) {
		struct host_note *n = &host_notes[seen++];

		if (n->ch != MG_ATT_SETUP) {
			continue;
		}
		if (n->len < 3 || n->data[0] != 0xFE) {
			memcpy(buf, n->data, n->len);
			buf[n->len] = '\0';
			return buf;
		}
		CHECK(n->len <= 160 && n->len <= host_mtu - 3);
		memcpy(&buf[len], &n->data[3], n->len - 3);
		len += n->len - 3;
		if (n->data[1] + 1 == n->data[2]) {
			buf[len] = '\0';
			return buf;
		}
	}
	return NULL;
}

static bool no_more(void)
{
	return next_msg() == NULL;
}

static void app_send_enc(const char *plain)
{
	static uint8_t ct[3200];
	static char ct_b64[4400], env[4600];
	char tag_b64[32], ctr[24];
	size_t n = strlen(plain);
	char sid[24];

	strcpy(sid, V->session_id);
	CHECK(mg_setup_seal(dev_rx, MG_SETUP_M2D, app_ctr, sid, (const uint8_t *)plain, n, ct) ==
	      0);
	mg_b64url_encode(ct, n, ct_b64, sizeof(ct_b64));
	mg_b64url_encode(&ct[n], 16, tag_b64, sizeof(tag_b64));
	snprintf(ctr, sizeof(ctr), "%llu", (unsigned long long)app_ctr++);
	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"%s\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 sid, ctr, ct_b64, tag_b64);
	app_send(env);
}

/* Opens the device's next record; returns its plaintext (NULL on failure). */
static char *app_recv_enc(void)
{
	static char pt[600];
	static struct mg_json_obj o;
	char *m = next_msg();
	uint8_t raw[700];
	size_t cl, tl;
	char ctr[24];

	if (!m) {
		return NULL;
	}
	if (mg_json_parse(m, strlen(m), &o) || !mg_json_str(&o, "type") ||
	    strcmp(mg_json_str(&o, "type"), "pairing_encrypted") != 0) {
		printf("    not a record: %s\n", m);
		return NULL;
	}
	snprintf(ctr, sizeof(ctr), "%llu", (unsigned long long)dev_ctr);
	CHECK_STR(mg_json_str(&o, "session_id"), V->session_id);
	CHECK_STR(mg_json_str(&o, "counter"), ctr);
	const char *c = mg_json_str(&o, "ciphertext"), *t = mg_json_str(&o, "tag");

	if (!c || !t || mg_b64url_decode(c, strlen(c), raw, sizeof(raw) - 16, &cl) ||
	    cl >= sizeof(pt) || mg_b64url_decode(t, strlen(t), &raw[cl], 16, &tl) || tl != 16 ||
	    mg_setup_open(dev_tx, MG_SETUP_D2M, dev_ctr, V->session_id, raw, cl + 16,
			  (uint8_t *)pt)) {
		printf("    record doesn't open\n");
		return NULL;
	}
	dev_ctr++;
	pt[cl] = '\0';
	return pt;
}

static bool status_is(const char *plain, const char *want)
{
	static struct mg_json_obj o;
	static char b[600];

	if (!plain) {
		return false;
	}
	strcpy(b, plain);
	return mg_json_parse(b, strlen(b), &o) == 0 && mg_json_str(&o, "type") &&
	       strcmp(mg_json_str(&o, "type"), "status") == 0 && mg_json_str(&o, "status") &&
	       strcmp(mg_json_str(&o, "status"), want) == 0;
}

static void hello(void)
{
	char h[400];

	snprintf(h, sizeof(h),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\","
		 "\"mobile_nonce\":\"%s\"}",
		 V->mobile_pub, V->mobile_nonce);
	app_send(h);
}

/* ---- setup ---- */

/* The vectors were made for a homelink- node id: the tests offer that form,
 * as a build without CONFIG_MG_NODE_ID_GADGET does (gadget- is
 * test_node_ids'). */
static bool offer_gadget;

static void boot(void)
{
	static const uint8_t addr[6] = {0x02, 0, 0, 0, 0, 0x01};

	mg_identity_set_offer_gadget(offer_gadget);
	mg_identity_init("MuseGadget-", "", addr);
	(void)mg_setup_store_load();
	mg_setup_init();
	mg_token_proof_connection_reset();
}

static void connect(void)
{
	host_conn_gen++; /* a new connection (mg_transport_conn_gen()) */
	host_notes_clear();
	seen = 0;
	app_ctr = dev_ctr = 0;
	mg_setup_connected();
}

static void fresh(void)
{
	host_settings_clear();
	host_disconnects = 0;
	host_now_ms = 1000;
	boot();
	connect();
}

#if defined(CONFIG_MG_SDK_TOKEN_SLOT)
/* The image's token slot (mg_token_slot.c's), written by the tests the way the
 * web updater writes it: mg_setup.c reads it at run time. */
char mg_sdk_token_slot[MG_SDK_TOKEN_SLOT_LEN] = MG_SDK_TOKEN_SLOT_MARKER;
#endif
#if defined(CONFIG_MG_SDK_TOKEN_BOARD)
/* The board's token (CONFIG_MG_SDK_TOKEN_BOARD, mg_app.h), as a board's
 * loader would hand it over; NULL: none. */
static const char *board_token;

const char *mg_board_sdk_token(void)
{
	return board_token;
}

/* mg_board_setup_state() (mg_app.h): this build's board hears every change. */
static int board_setup_calls;
static bool board_set_up;

void mg_board_setup_state(bool complete)
{
	board_setup_calls++;
	board_set_up = complete;
}
#endif
#if defined(CONFIG_MG_SDK_TOKEN_SLOT) || defined(CONFIG_MG_SDK_TOKEN_BOARD)
static const char *handshake_confirmed; /* the last pairing_confirmed */
#endif

/* hello .. pairing_confirmed, checked step by step. */
/* The next handshake stops at the prompt, or (resetup_wait) waits ten
 * minutes on it and then confirms with one press. */
static bool resetup_wait;
static bool stop_at_prompt; /* handshake() ends at confirm_required */
/* Setup mode (the button held at boot on the device, main.c): the rows that
 * set a set-up gadget up again run in it; test_resetup_setup_mode turns it
 * off. */
static bool host_setup_mode = true;

bool mg_setup_mode(void)
{
	return host_setup_mode;
}

static void handshake(void)
{
	static struct mg_json_obj o;
	uint8_t dpriv[32], dnonce[16];
	size_t n;

	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	char *m = next_msg();

	CHECK(m != NULL);
	if (!m) {
		return;
	}
	CHECK(mg_json_parse(m, strlen(m), &o) == 0);
	CHECK_STR(mg_json_str(&o, "type"), "pairing_ready");
	CHECK(mg_json_get(&o, "version") && mg_json_get(&o, "version")->num == 5);
	CHECK_STR(mg_json_str(&o, "device_id"), V->device_id);
	CHECK_STR(mg_json_str(&o, "node_id"), V->node_id);
	CHECK_STR(mg_json_str(&o, "mac"), V->mac);
	CHECK_STR(mg_json_str(&o, "model"), "hatch_link");
	CHECK_STR(mg_json_str(&o, "firmware_version"), "1.0.0");
	CHECK_STR(mg_json_str(&o, "pairing_auth"), "none");
	CHECK(mg_json_get(&o, "pairing_auth_epoch") &&
	      mg_json_get(&o, "pairing_auth_epoch")->num == 0);
	CHECK_STR(mg_json_str(&o, "pairing_policy"), "confirm_press");
	CHECK_STR(mg_json_str(&o, "device_pub"), V->device_pub);
	CHECK_STR(mg_json_str(&o, "device_nonce"), V->device_nonce);
	CHECK_STR(mg_json_str(&o, "transcript_hash"), V->transcript_hash);
	CHECK_STR(mg_json_str(&o, "session_id"), V->session_id);
	CHECK(mg_json_get(&o, "device_proof") == NULL);

	/* The vector's own client_finished record. */
	char env[300];

	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 V->session_id, V->cf_ct, V->cf_tag);
	app_send(env);
	app_ctr = 1;
	CHECK(status_is(app_recv_enc(), "confirm_required"));
	CHECK(host_pair_led);
	if (stop_at_prompt) {
		return;
	}

	if (resetup_wait) {
		/* The prompt has no timer: ten minutes on it is still up, nothing
		 * sent, and then one press confirms (a re-setup as a first setup). */
		resetup_wait = false;
		host_advance(10 * 60 * 1000);
		CHECK(host_pair_led && mg_setup_confirm_pending() && no_more());
	}
	/* One press confirms, a first setup and a re-setup alike (the button,
	 * or the bench line); pairing_confirmed carries the SDK token. */
	CHECK(mg_setup_button(true) == true);
	CHECK(!host_pair_led);
	char *pc = app_recv_enc();

	CHECK(status_is(pc, "pairing_confirmed"));
#if defined(CONFIG_MG_SDK_TOKEN_SLOT) || defined(CONFIG_MG_SDK_TOKEN_BOARD)
	handshake_confirmed = pc;
#else
	CHECK(pc && strstr(pc, "\"sdk_token\":\"" CONFIG_MG_SDK_TOKEN "\"") != NULL);
#endif
}

/* ---- tests ---- */

static void test_device_info(void)
{
	static struct mg_json_obj o;

	printf("  device_info\n");
	fresh();
	app_send("{\"action\":\"get_device_info\"}");
	char *m = next_msg();

	CHECK(m && mg_json_parse(m, strlen(m), &o) == 0);
	CHECK_STR(mg_json_str(&o, "type"), "device_info");
	CHECK_STR(mg_json_str(&o, "node_id"), "homelink-000001");
	CHECK_STR(mg_json_str(&o, "version"), "1.0.0");
	CHECK_STR(mg_json_str(&o, "device_id"), "hatch-link:02:00:00:00:00:01");
	CHECK_STR(mg_json_str(&o, "mac"), "02:00:00:00:00:01");
	CHECK_STR(mg_json_str(&o, "model"), "hatch_link");
	CHECK(mg_json_get(&o, "pairing_protocol") && mg_json_get(&o, "pairing_protocol")->num == 5);
	CHECK_STR(mg_json_str(&o, "pairing_auth"), "none");
	CHECK(mg_json_get(&o, "pairing_auth_epoch")->num == 0);
	CHECK_STR(mg_json_str(&o, "pairing_policy"), "confirm_press");
	CHECK_STR(mg_json_str(&o, "wifi"), "none");
	/* The device key contract: provision_v2 takes one, and the phone runs
	 * the device session, the attestation handshake included. */
	/* A BLE-only Link setup never offers
	 * device-key auth, so the app's setup leaves the gadget keyless and the
	 * phone relays its session. */
	CHECK(mg_json_get(&o, "device_key_auth") &&
	      mg_json_get(&o, "device_key_auth")->type == MG_JSON_NUMBER &&
	      mg_json_get(&o, "device_key_auth")->num == 0);
	CHECK(mg_json_get(&o, "cvm_supported") &&
	      mg_json_get(&o, "cvm_supported")->type == MG_JSON_TRUE);
	CHECK(mg_json_get(&o, "mgcommands") && mg_json_get(&o, "mgcommands")->type == MG_JSON_NUMBER &&
	      mg_json_get(&o, "mgcommands")->num == 1);
	CHECK(mg_setup_advertise_link());
}

static void test_plaintext_rules(void)
{
	printf("  plaintext rules\n");
	fresh();
	/* Only the four errors are sent in plaintext before a session. */
	app_send("{\"action\":\"no_such_action\"}");
	CHECK(no_more());
	app_send("not json");
	CHECK(no_more());
	app_send("{\"action\":\"provision_v2\",\"access_token\":\"a\"}");
	CHECK_STR(next_msg(), "error_encryption_required");
	app_send("{\"action\":\"wifi_scan\"}");
	CHECK_STR(next_msg(), "error_encryption_required");
	app_send("{\"action\":\"pairing_client_hello\",\"version\":4,\"pairing_auth\":\"none\"}");
	CHECK_STR(next_msg(), "error_pairing_invalid_hello");
	app_send("{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":"
		 "\"fleet_ecdsa_p256_v1\",\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"x\","
		 "\"mobile_nonce\":\"y\"}");
	CHECK_STR(next_msg(), "error_pairing_invalid_hello");
	/* A point off the curve. */
	char h[400], pub[100];

	strcpy(pub, V->mobile_pub);
	pub[60] = pub[60] == 'A' ? 'B' : 'A';
	snprintf(h, sizeof(h),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\",\"mobile_nonce\":\"%s\"}",
		 pub, V->mobile_nonce);
	app_send(h);
	CHECK_STR(next_msg(), "error_pairing_invalid_hello");
	app_send("{\"action\":\"pairing_encrypted\",\"session_id\":\"x\",\"counter\":\"0\","
		 "\"ciphertext\":\"AAAA\",\"tag\":\"AAAA\"}");
	CHECK_STR(next_msg(), "error_pairing_decrypt");
	host_advance(400);
	CHECK(host_disconnects == 1);

	/* After pairing_ready, plaintext commands are ignored (even device_info). */
	fresh();
	handshake();
	app_send("{\"action\":\"get_device_info\"}");
	CHECK(no_more());
	app_send("{\"action\":\"provision_v2\"}");
	CHECK(no_more());
	/* The press is never push-to-talk while a confirmation is pending; with
	 * none pending it is. */
	CHECK(mg_setup_button(true) == false);
}

static void test_provision(void)
{
	const struct proof_vec *pv = &proof_vecs[2]; /* node homelink-000001 */
	char msg[1400];

	printf("  token-only provision_v2\n");
	CHECK_STR(pv->node_id, "homelink-000001");
	fresh();
	handshake();

	/* Missing credentials and Wi-Fi fields: answered, nothing stored. */
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"a\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_missing_credentials"));
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"a\",\"refresh_token\":\"r\","
		     "\"token_type\":\"user\"}");
	CHECK(status_is(app_recv_enc(), "error_missing_credentials"));
	app_send_enc("{\"action\":\"provision_v2\",\"ssid\":\"home\",\"password\":\"pw\","
		     "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_wifi_unsupported"));
	app_send_enc("{\"action\":\"provision_v2\",\"ssid\":\"\",\"password\":\"pw\","
		     "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_wifi_unsupported"));
	CHECK(host_settings_count() == 0 && !mg_setup_store_complete());
	app_send_enc("{\"action\":\"get_device_info\"}");
	CHECK(status_is(app_recv_enc(), "error_unknown_action"));

	/* Token-only: empty / absent Wi-Fi fields, extra fields ignored. */
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"ssid\":\"\",\"password\":null,"
		 "\"access_token\":\"%s\",\"refresh_token\":\"refresh-\\u00fc-1\","
		 "\"token_type\":\"device\",\"username\":\"u\",\"api_url_v2\":\"https://x\","
		 "\"ota_url\":\"https://ota\",\"ota_force\":true,\"extra\":{\"a\":[1,2]}}",
		 pv->access_token);
	int done_cues = host_led_events[MG_LED_EVENT_SETUP_DONE];

	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	/* Setup done shows its cue (3 quick blinks) once stored. */
	CHECK(host_led_events[MG_LED_EVENT_SETUP_DONE] == done_cues + 1);
	CHECK(mg_setup_store_complete() && mg_setup_store_wifi_skipped());
	CHECK(!mg_setup_advertise_link());
	CHECK(host_settings_len("mg/setup/at") == (int)strlen(pv->access_token));
	CHECK(host_settings_len("mg/setup/rt") == (int)strlen("refresh-\xc3\xbc-1"));
	CHECK(host_settings_get("mg/setup/rt") &&
	      memcmp(host_settings_get("mg/setup/rt"), "refresh-\xc3\xbc-1", 11) == 0);
	CHECK(host_settings_len("mg/setup/k") == 32);
	CHECK(host_settings_len("mg/setup/done") > 0);
	uint8_t k[32];

	CHECK(mg_setup_store_get_k(k) == 0);
	CHECK_MEM(k, pv->K, 32);
	CHECK(host_disconnects == 0);

	/* Provisioning again on this session: it provisioned once; a new setup
	 * needs a new handshake and press. */
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "error_pairing_confirm_required"));
	CHECK(mg_setup_store_get_k(k) == 0);
	CHECK_MEM(k, pv->K, 32);

	/* A new connection: hello is answered (Gadget setup v2), never
	 * error_pairing_unavailable; nothing changes without a commit. */
	mg_setup_disconnected();
	connect();
	hello();
	char *ready = next_msg();

	CHECK(ready && strstr(ready, "\"type\":\"pairing_ready\"") != NULL);
	mg_setup_disconnected();
	connect();

	/* A reboot keeps it. */
	boot();
	CHECK(mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(k) == 0);
	CHECK_MEM(k, pv->K, 32);
}

/* provision_v2's optional device_key and device_key_id (protocols/README.md,
 * Gadget setup over Muse Link), with the VM they are bound to: stored in the
 * same commit, for the device session's credentials. */
static void test_device_key(void)
{
	/* bytes 0xf0..0xff, 0x00..0x0f (with '+' and '/') and 0xa0..0xaf, in
	 * standard base64 with padding. */
	static const char dk[] = "8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=";
	static const char kid[] = "oKGio6SlpqeoqaqrrK2urw==";
	static const char vm[] = "00000000-0000-4000-8000-00000000c0de";
	static const char *const bad[] = {
		/* not base64 */
		"\"device_key\":\"!!!!\",\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\"",
		/* short, long */
		"\"device_key\":\"AAEC\",\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\"",
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2ur6Cg\""),
		/* one without the other */
		"\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\"",
		"\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\"",
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":null"),
		/* not a string */
		"\"device_key\":5,\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\"",
		/* the id unpadded, or in base64url */
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw\""),
		("\"device_key\":\"8PHy8_T19vf4-fr7_P3-_wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\""),
		/* the key without its padding, or with bits past the last byte */
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\""),
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg9=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\""),
		/* an unknown vm_kind, a vm_id that isn't printable ASCII, too
		 * long or not a string */
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_kind\":\"tdx\""),
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_kind\":1"),
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_id\":\"my vm\""),
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_id\":"
		 "\"0123456789012345678901234567890123456789012345678901234567890123x\""),
		("\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		 "\"device_key_id\":\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_id\":7"),
	};
	uint8_t want_dk[32], want_kid[16], got_dk[32], got_kid[16];
	char msg[600], vm_id[MG_SETUP_VM_ID_MAX + 1];
	size_t vm_len;
	enum mg_setup_vm_kind kind;

	printf("  provision_v2 with a device key\n");
	for (int i = 0; i < 32; i++) {
		want_dk[i] = (uint8_t)(0xf0 + i);
	}
	for (int i = 0; i < 16; i++) {
		want_kid[i] = (uint8_t)(0xa0 + i);
	}
	fresh();
	handshake();
	/* Each refused, nothing stored, and the session may try again. */
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		snprintf(msg, sizeof(msg),
			 "{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":"
			 "\"ref\",\"token_type\":\"device\",%s}",
			 bad[i]);
		app_send_enc(msg);
		CHECK(status_is(app_recv_enc(), "error_invalid_device_key"));
		CHECK(no_more() && host_disconnects == 0);
		CHECK(host_settings_count() == 0 && !mg_setup_store_complete());
	}
	/* The 64-byte vm_id, the most there is, is kept. */
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		 "\"token_type\":\"device\",\"device_key\":\"%s\",\"device_key_id\":\"%s\","
		 "\"vm_id\":\"%s\",\"vm_kind\":\"cvm\"}",
		 dk, kid, vm);
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(host_settings_len("mg/setup/dk") == 32 && host_settings_len("mg/setup/kid") == 16 &&
	      host_settings_len("mg/setup/vm") == (int)strlen(vm));
	CHECK(mg_setup_store_has_device_key());
	CHECK(mg_setup_store_get_device_key(got_dk, got_kid) == 0);
	CHECK_MEM(got_dk, want_dk, 32);
	CHECK_MEM(got_kid, want_kid, 16);
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0);
	CHECK(vm_len == strlen(vm) && kind == MG_SETUP_VM_KIND_ATTESTED);
	CHECK_STR(vm_id, vm);
	/* A reboot keeps it all; a missing key id or vm_id makes the setup
	 * half-written. */
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_has_device_key());
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 && kind == MG_SETUP_VM_KIND_ATTESTED);
	CHECK_STR(vm_id, vm);
	settings_delete("mg/setup/vm");
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
	CHECK(mg_setup_store_commit_device_key(
		      "acc", 3, "ref", 3, want_dk,
		      &(struct mg_setup_device_key){.key = want_dk, .key_id = want_kid}) == 0);
	settings_delete("mg/setup/kid");
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
	/* The store refuses a binding it can't keep, writing nothing. */
	CHECK(mg_setup_store_commit_device_key(
		      "acc", 3, "ref", 3, want_dk,
		      &(struct mg_setup_device_key){.key = want_dk, .key_id = want_kid,
						    .vm_kind = (enum mg_setup_vm_kind)3}) ==
	      -EINVAL);
	CHECK(mg_setup_store_commit_device_key(
		      "acc", 3, "ref", 3, want_dk,
		      &(struct mg_setup_device_key){.key = want_dk, .key_id = want_kid,
						    .vm_id = msg, .vm_id_len = 65}) == -EINVAL);
	CHECK(host_settings_count() == 0);

	/* A key with only a vm_kind, or only a vm_id: each kept as sent. */
	fresh();
	handshake();
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		 "\"token_type\":\"device\",\"device_key\":\"%s\",\"device_key_id\":\"%s\","
		 "\"vm_id\":\"\",\"vm_kind\":\"svm\"}",
		 dk, kid);
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 && vm_len == 0 &&
	      kind == MG_SETUP_VM_KIND_PLAIN && vm_id[0] == '\0');
	CHECK(host_settings_len("mg/setup/vm") == -1);
	fresh();
	handshake();
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		 "\"token_type\":\"device\",\"device_key\":\"%s\",\"device_key_id\":\"%s\","
		 "\"vm_id\":\"vm-1\"}",
		 dk, kid);
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 && vm_len == 4 &&
	      kind == MG_SETUP_VM_KIND_NONE);
	CHECK_STR(vm_id, "vm-1");

	/* Without a key: none held, a vm_id and vm_kind alone mean nothing, and
	 * stray key and vm entries are dropped at boot. */
	fresh();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		     "\"token_type\":\"device\",\"device_key\":\"\",\"device_key_id\":null,"
		     "\"vm_id\":\"vm-1\",\"vm_kind\":\"cvm\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(!mg_setup_store_has_device_key() && host_settings_count() == 4);
	CHECK(mg_setup_store_get_device_key(got_dk, got_kid) == -ENOENT);
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == -ENOENT);
	settings_save_one("mg/setup/dk", want_dk, 32);
	settings_save_one("mg/setup/vm", "vm-1", 4);
	boot();
	CHECK(mg_setup_store_complete() && host_settings_len("mg/setup/dk") == -1 &&
	      host_settings_len("mg/setup/vm") == -1 && host_settings_count() == 4);

	/* A failed write of the key, its id, the vm_id or the record rolls
	 * everything back. */
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		 "\"token_type\":\"device\",\"device_key\":\"%s\",\"device_key_id\":\"%s\","
		 "\"vm_id\":\"%s\",\"vm_kind\":\"svm\"}",
		 dk, kid, vm);
	for (int fail = 4; fail <= 7; fail++) {
		fresh();
		handshake();
		host_settings_fail_save(fail);
		app_send_enc(msg);
		CHECK(status_is(app_recv_enc(), "error_storage"));
		CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
	}
	/* A reset erases it. */
	fresh();
	handshake();
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(host_settings_count() == 7);
	CHECK(mg_setup_factory_reset("test") == 0);
	CHECK(host_settings_count() == 0 && !mg_setup_store_has_device_key());
}

/* Gadget setup v2: re-provisioning a set-up gadget. */

static void provision(const char *access, const char *refresh)
{
	static char msg[1400];

	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"%s\",\"refresh_token\":\"%s\","
		 "\"token_type\":\"device\"}",
		 access, refresh);
	app_send_enc(msg);
}

/* The store holds K for this access token (node homelink-000001). */
static bool holds_k_for(const char *access)
{
	uint8_t want[32], got[32];

	return mg_token_proof_derive_key(access, strlen(access), "homelink-000001", want) == 0 &&
	       mg_setup_store_get_k(got) == 0 && memcmp(want, got, 32) == 0;
}

/* The proof vector for homelink-000001, frame by frame: 1 if the device's
 * response and result are the vector's (it holds that vector's K), 0 if its
 * MAC differs (it holds another K), -1 on anything else. */
static int vector_proof(const struct proof_vec *pv)
{
	host_notes_clear();
	seen = 0;
	mg_token_proof_connection_reset();
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	if (host_nnotes != 1 || host_notes[0].len != pv->response.n) {
		return -1;
	}
	if (memcmp(host_notes[0].data, pv->response.b, pv->response.n) != 0) {
		return 0;
	}
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	return host_nnotes == 2 && host_notes[1].len == pv->result.n &&
			       memcmp(host_notes[1].data, pv->result.b, pv->result.n) == 0
		       ? 1
		       : -1;
}

/* The proof with the device's own nonce and the K this access token gives. */
/* The last live proof's K and nonces: the sealed session's keys. */
static uint8_t lp_k[32], lp_cn[16], lp_dn[16];

static int live_proof(const char *access)
{
	uint8_t k[32], cn[16] = {7}, frame[1 + 32], mac[32];

	mg_token_proof_derive_key(access, strlen(access), "homelink-000001", k);
	host_notes_clear();
	seen = 0;
	mg_token_proof_connection_reset();
	mg_token_proof_rx((const uint8_t[]){mg_token_proof_challenge, 7, 0, 0, 0, 0, 0, 0, 0, 0,
					     0, 0, 0, 0, 0, 0, 0},
			  17);
	if (host_nnotes != 1 || host_notes[0].data[1] != mg_token_proof_response) {
		return -1;
	}
	mg_token_proof_mac(k, true, cn, &host_notes[0].data[2], mac);
	if (memcmp(mac, &host_notes[0].data[18], 32) != 0) {
		return 0; /* the device holds another K */
	}
	memcpy(lp_k, k, 32);
	memcpy(lp_cn, cn, 16);
	memcpy(lp_dn, &host_notes[0].data[2], 16);
	frame[0] = mg_token_proof_confirm;
	mg_token_proof_mac(k, false, cn, &host_notes[0].data[2], &frame[1]);
	mg_token_proof_rx(frame, sizeof(frame));
	return host_nnotes == 2 && host_notes[1].data[2] == 1 ? 1 : -1;
}

/* hello .. pairing_confirmed again on the connection that's up. */
static void handshake_again(void)
{
	app_ctr = dev_ctr = 0;
	handshake();
}

/* The re-setup prompt's rate: a hello on the prompt's own connection while
 * it is up takes it over (no refusal, the light stays) and the one press
 * confirms the newest hello; with no prompt up a hello shows it at once; a
 * new connection shows it at once. */
static void test_resetup_rate_flow(void)
{
	const struct proof_vec *pv = &proof_vecs[2];

	printf("  a hello while the re-setup prompt is up takes it over; one press confirms\n");
	fresh();
	handshake();
	provision(pv->access_token, "refresh-1");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	stop_at_prompt = true;
	handshake_again(); /* hello 1: the prompt shows */
	host_now_ms += 4000;
	handshake_again(); /* hello 2, 4 s on, same connection: takes it over */
	stop_at_prompt = false;
	CHECK(host_pair_led);
	CHECK(mg_setup_button(true));
	CHECK(status_is(app_recv_enc(), "pairing_confirmed")); /* to hello 2's session */
	provision("access-two", "refresh-2");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(holds_k_for("access-two"));
	handshake_again(); /* hello 3, no prompt up: shows and confirms at once */
	mg_setup_disconnected();
	connect(); /* a new connection: at once */
	handshake();
}

/* A takeover hello that fails (the portable core's #524 finding, proof_gates:
 * a takeover hello that fails validation ends the prompt): refused on its own
 * fields or key, before any wipe, it's answered under the session's keys and
 * then ends the prompt; failing after the wipe (a point off the curve) it has
 * ended it already, and its error goes out in plaintext. Either way a talk press is a talk press, a dismiss finds
 * nothing and the next valid hello asks again, nothing confirmed before its
 * own press. A dismiss while a takeover's hello is answered ends the prompt
 * too. */
static void test_resetup_bad_takeover(void)
{
	const struct proof_vec *pv = &proof_vecs[2];
	uint8_t dpriv[32], dnonce[16];
	char h[400], pub[100];
	size_t n;

	printf("  a takeover hello that fails ends the prompt\n");
	fresh();
	handshake();
	provision(pv->access_token, "refresh-1");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	stop_at_prompt = true;
	strcpy(pub, V->mobile_pub);
	pub[60] = pub[60] == 'A' ? 'B' : 'A';
	for (int kind = 0; kind < 3; kind++) {
		handshake_again(); /* the prompt shows */
		CHECK(host_pair_led && mg_setup_confirm_pending());
		if (kind == 0) { /* refused on its fields */
			snprintf(h, sizeof(h), "%s",
				 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
				 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"x\"}");
		} else {   /* refused on its key; a point off the curve fails after the wipe */
			snprintf(h, sizeof(h),
				 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
				 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\",\"mobile_nonce\":\"%s\"}",
				 kind == 1 ? "AAAA" : pub, V->mobile_nonce);
		}
		app_send(h);
		if (kind < 2) {
			CHECK(status_is(app_recv_enc(), "error_pairing_invalid_hello")); /* its session's keys */
		} else {
			/* After the wipe no keys are left: the hello started
			 * over, so its error goes out in plaintext, as a first
			 * hello's does. */
			CHECK_STR(next_msg(), "error_pairing_invalid_hello");
		}
		CHECK(!host_pair_led && !mg_setup_confirm_pending());
		CHECK(!mg_setup_button(true)); /* no prompt: a talk press */
		mg_setup_dismiss();        /* the back gesture: nothing up */
		CHECK(no_more());
	}
	/* The next valid hello asks again: nothing confirmed before its press. */
	handshake_again();
	CHECK(host_pair_led && mg_setup_confirm_pending() && no_more());
	CHECK(mg_setup_button(true));
	CHECK(status_is(app_recv_enc(), "pairing_confirmed"));
	CHECK(holds_k_for(pv->access_token)); /* nothing committed yet */
	/* A dismiss while a takeover's hello is answered: the prompt ends, and
	 * that hello never finishes into a new one. */
	mg_setup_disconnected();
	connect();
	app_ctr = dev_ctr = 0;
	handshake(); /* the prompt shows */
	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	app_ctr = dev_ctr = 0;
	hello(); /* takes it over */
	CHECK(next_msg() != NULL); /* pairing_ready */
	CHECK(host_pair_led && !mg_setup_confirm_pending());
	host_disconnects = 0;
	mg_setup_dismiss();
	CHECK(!host_pair_led && !mg_setup_confirm_pending());
	CHECK(status_is(app_recv_enc(), "pairing_confirm_timeout"));
	CHECK(!mg_setup_button(true)); /* nothing up: a talk press */
	host_advance(200);
	CHECK(host_disconnects == 1);
	CHECK(holds_k_for(pv->access_token));
	stop_at_prompt = false;
}

static void test_reprovision(void)
{
	const struct proof_vec *pv = &proof_vecs[2]; /* node homelink-000001 */

	printf("  re-provisioning a set-up gadget\n");
	fresh();
	handshake();
	provision(pv->access_token, "refresh-1");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_store_complete() && holds_k_for(pv->access_token));
	CHECK(vector_proof(pv) == 1 && mg_token_proof_matched());

	/* Set up again on the next connection: the same handshake, confirmed by
	 * one press after ten minutes on the prompt. */
	mg_setup_disconnected();
	mg_token_proof_connection_reset();
	host_conn_gen++;
	host_disconnects = 0;
	connect();
	CHECK(vector_proof(pv) == 1 && mg_token_proof_matched());
	resetup_wait = true;
	handshake_again();
	CHECK(mg_token_proof_matched()); /* nothing is replaced before the commit */
	provision("access-two", "refresh-2");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	/* The commit replaced tokens and K, and the proof made with the old K
	 * no longer stands. */
	CHECK(!mg_token_proof_matched());
	CHECK(mg_setup_store_complete() && mg_setup_store_wifi_skipped());
	CHECK(holds_k_for("access-two"));
	CHECK(host_settings_len("mg/setup/at1") == 10 && host_settings_len("mg/setup/rt1") == 9 &&
	      host_settings_len("mg/setup/k1") == 32);
	CHECK(host_settings_len("mg/setup/at") < 0 && host_settings_len("mg/setup/rt") < 0 &&
	      host_settings_len("mg/setup/k") < 0);
	CHECK(host_settings_count() == 4); /* one set and its record */
	CHECK(vector_proof(pv) == 0);       /* the old K: the device's MAC differs */
	CHECK(live_proof(pv->access_token) == 0);
	CHECK(live_proof("access-two") == 1);
	/* clear needs a match with the new K now. */
	mg_token_proof_connection_reset();
	mg_token_proof_rx((const uint8_t[]){mg_token_proof_clear}, 1);
	CHECK(mg_setup_store_complete());
	CHECK(!mg_setup_advertise_link()); /* set up: mg advertising only */
	CHECK(host_disconnects == 0);

	/* Kept across a reboot. */
	boot();
	CHECK(mg_setup_store_complete() && holds_k_for("access-two"));

	/* Again, on a new connection (found by its mg advertisement): back to
	 * the first slot. */
	connect();
	handshake();
	provision("access-three", "refresh-3");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(holds_k_for("access-three") && host_settings_len("mg/setup/at") == 12 &&
	      host_settings_len("mg/setup/at1") < 0 && host_settings_count() == 4);
	boot();
	CHECK(mg_setup_store_complete() && holds_k_for("access-three"));
	CHECK(live_proof("access-two") == 0 && live_proof("access-three") == 1);

	/* Abandoned before the commit: the old setup stays. */
	connect();
	handshake();
	mg_setup_disconnected();
	CHECK(mg_setup_store_complete() && holds_k_for("access-three"));
	CHECK(mg_setup_button(true) == false); /* no session: the press is push-to-talk */
	/* ... after the press too, and through the confirm timeout. */
	connect();
	handshake_again();
	host_advance(MG_SETUP_CONFIRMED_IDLE_MS + 10);
	CHECK(status_is(app_recv_enc(), "pairing_confirm_timeout"));
	host_advance(200);
	CHECK(mg_setup_store_complete() && holds_k_for("access-three"));
	/* A refused provision_v2 changes nothing either. */
	mg_setup_disconnected();
	connect();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"ssid\":\"home\",\"password\":\"pw\","
		     "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_wifi_unsupported"));
	CHECK(holds_k_for("access-three") && host_settings_count() == 4);
	mg_setup_disconnected();

	/* A storage failure at each write: error_storage, and the old setup
	 * stays, now and after a reboot. The commit record's own write failing
	 * leaves the old record in place. */
	for (int fail = 1; fail <= 4; fail++) {
		host_disconnects = 0;
		connect();
		handshake();
		host_settings_fail_save(fail);
		provision("access-four", "refresh-4");
		CHECK(status_is(app_recv_enc(), "error_storage"));
		CHECK(no_more());
		CHECK(mg_setup_store_complete() && holds_k_for("access-three"));
		CHECK(host_settings_count() == 4);
		host_advance(600);
		CHECK(host_disconnects == 1);
		mg_setup_disconnected();
		boot();
		CHECK(mg_setup_store_complete() && holds_k_for("access-three"));
	}

	/* A confirmed session may also unpair a set-up gadget (the press was
	 * made for it). */
	connect();
	handshake();
	int boosts = host_adv_boosts;

	app_send_enc("{\"action\":\"unpair\"}");
	CHECK(status_is(app_recv_enc(), "unpaired"));
	CHECK(host_adv_boosts == boosts + 1); /* fast again for setup */
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0 && mg_setup_advertise_link());
	mg_setup_disconnected();
}

/* A set-up gadget whose only key is the talk key takes a re-setup only in
 * setup mode (the button held at boot): outside it the hello is refused in
 * plaintext and nothing shows; a first setup keeps any press. */
static void test_resetup_setup_mode(void)
{
	const struct proof_vec *pv = &proof_vecs[2];

	printf("  outside setup mode a re-setup hello is refused, nothing shows\n");
	host_setup_mode = false;
	fresh();
	handshake(); /* a first setup: any press, setup mode or not */
	provision(pv->access_token, "refresh-1");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	app_ctr = dev_ctr = 0;
	hello();
	CHECK_STR(next_msg(), "error_pairing_unavailable");
	CHECK(!host_pair_led && no_more());
	CHECK(mg_setup_button(true) == false); /* no prompt: the press is push-to-talk */
	CHECK(mg_setup_store_complete() && holds_k_for(pv->access_token));
	/* In setup mode the same hello shows the prompt and one press confirms. */
	host_setup_mode = true;
	mg_setup_disconnected();
	connect();
	handshake();
	provision("access-two", "refresh-2");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(holds_k_for("access-two"));
	/* "the talk key never confirms a re-setup": with a prompt up outside
	 * setup mode a talk press only dismisses it (no confirm, no talk turn;
	 * the link drops), and that dismiss starts the 10 s rule on its
	 * connection. (A talk-key-only board reaches this only if setup mode
	 * ends while its prompt is up.) */
	stop_at_prompt = true;
	handshake_again();
	stop_at_prompt = false;
	host_setup_mode = false;
	host_disconnects = 0;
	CHECK(mg_setup_button(true)); /* the talk key: not a talk turn */
	CHECK(!host_pair_led);
	CHECK(status_is(app_recv_enc(), "pairing_confirm_timeout"));
	host_setup_mode = true;
	app_ctr = dev_ctr = 0;
	hello(); /* the same connection, before its drop: within 10 s of the dismiss */
	CHECK_STR(next_msg(), "error_operation_in_progress");
	host_advance(200);
	CHECK(host_disconnects == 1);
	CHECK(holds_k_for("access-two")); /* nothing confirmed */
	mg_setup_disconnected();
	connect();
	app_ctr = dev_ctr = 0;
	handshake(); /* a new connection: shows at once, one press (setup mode) confirms */
}

/* Re-provisioning and the device key: a new key (with its VM binding)
 * replaces the old one, a setup without one leaves none, a failed or
 * refused one keeps the old key. */
static void test_reprovision_device_key(void)
{
	static const char with_key[] =
		"{\"action\":\"provision_v2\",\"access_token\":\"acc-k\",\"refresh_token\":"
		"\"ref-k\",\"token_type\":\"device\",\"device_key\":"
		"\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\",\"device_key_id\":"
		"\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_id\":\"vm-k\",\"vm_kind\":\"svm\"}";
	uint8_t dk[32], kid[16];
	char vm_id[MG_SETUP_VM_ID_MAX + 1];
	size_t vm_len;
	enum mg_setup_vm_kind kind;

	printf("  re-provisioning replaces or drops the device key\n");
	fresh();
	handshake();
	app_send_enc(with_key);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_store_has_device_key() && host_settings_len("mg/setup/dk") == 32 &&
	      host_settings_len("mg/setup/vm") == 4);

	/* Without a key: the old one, and its VM binding, don't survive. */
	handshake_again();
	provision("access-two", "refresh-2");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(!mg_setup_store_has_device_key() && holds_k_for("access-two"));
	CHECK(mg_setup_store_get_device_key(dk, kid) == -ENOENT);
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == -ENOENT);
	CHECK(host_settings_len("mg/setup/dk") < 0 && host_settings_len("mg/setup/dk1") < 0 &&
	      host_settings_len("mg/setup/kid") < 0 && host_settings_len("mg/setup/vm") < 0 &&
	      host_settings_count() == 4);
	boot();
	CHECK(mg_setup_store_complete() && !mg_setup_store_has_device_key());

	/* With one again: held in the new slot. */
	connect();
	handshake();
	app_send_enc(with_key);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_store_has_device_key() && holds_k_for("acc-k"));
	CHECK(mg_setup_store_get_device_key(dk, kid) == 0 && dk[0] == 0xf0 && kid[0] == 0xa0);
	CHECK(host_settings_len("mg/setup/vm") == 4 && host_settings_count() == 7);

	/* A failed re-setup (at the new key, its id, its vm_id or the record)
	 * keeps the old set, key and binding included, now and after a
	 * reboot. */
	for (int fail = 4; fail <= 7; fail++) {
		mg_setup_disconnected();
		connect();
		handshake();
		host_settings_fail_save(fail);
		app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc-x\","
			     "\"refresh_token\":\"ref-x\",\"token_type\":\"device\",\"device_key\":"
			     "\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\",\"device_key_id\":"
			     "\"AAECAwQFBgcICQoLDA0ODw==\",\"vm_id\":\"vm-x\",\"vm_kind\":\"cvm\"}");
		CHECK(status_is(app_recv_enc(), "error_storage"));
		CHECK(holds_k_for("acc-k") && host_settings_count() == 7);
		CHECK(mg_setup_store_get_device_key(dk, kid) == 0 && dk[0] == 0xf0);
		CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 &&
		      kind == MG_SETUP_VM_KIND_PLAIN && strcmp(vm_id, "vm-k") == 0);
		mg_setup_disconnected();
		boot();
		CHECK(holds_k_for("acc-k") && mg_setup_store_has_device_key());
		CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 &&
		      strcmp(vm_id, "vm-k") == 0);
	}
	/* A malformed key on a re-setup: refused, the old set kept whole. */
	mg_setup_disconnected();
	connect();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc-x\","
		     "\"refresh_token\":\"ref-x\",\"token_type\":\"device\",\"device_key\":"
		     "\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8\",\"device_key_id\":"
		     "\"AAECAwQFBgcICQoLDA0ODw==\"}");
	CHECK(status_is(app_recv_enc(), "error_invalid_device_key"));
	CHECK(holds_k_for("acc-k") && host_settings_count() == 7);
	CHECK(mg_setup_store_get_device_key(dk, kid) == 0 && dk[0] == 0xf0);
	mg_setup_disconnected();
	{
		int boosts = host_adv_boosts;

		CHECK(mg_setup_factory_reset("test") == 0 && host_settings_count() == 0);
		CHECK(host_adv_boosts == boosts + 1); /* fast again for setup */
	}
}

#if defined(CONFIG_MG_VM_SESSION)
/* mg_command_session as the app sends it (without the command byte). */
/* A sealed hello (every unsealed one is insecure). */
static void session_hello(void)
{
	const uint8_t m[] = {mg_session_hello, MG_SESSION_VERSION, 0, 0, 1, 0, 0, 0, 1, 0,
			     mg_session_hello_flag_sealed};

	mg_vm_session_rx(m, sizeof(m));
}

/* The credentials the device sent after its hello, reassembled. */
static const char *session_credentials(void)
{
	static char json[2100];
	size_t len = 0;

	for (int i = 0; i < host_nnotes; i++) {
		const struct host_note *n = &host_notes[i];

		if (n->len < 3 || n->data[0] != mg_command_session ||
		    n->data[1] != mg_session_credentials) {
			continue;
		}
		size_t h = (n->data[2] & mg_session_flag_first) ? 7 : 3;

		memcpy(&json[len], &n->data[h], n->len - h);
		len += n->len - h;
	}
	/* Sealed under the last proof (counter 0): opened in place. */
	mgseal_keys_t keys;
	size_t plen = 0;

	if (len <= MGSEAL_OVERHEAD ||
	    mgseal_keys(&keys, lp_k, lp_cn, lp_dn, "homelink-000001", 15) != MGSEAL_OK ||
	    mgseal_open(&keys, MGSEAL_DIR_DEVICE, 0, (uint8_t *)json, len, &plen) != MGSEAL_OK) {
		return "";
	}
	memmove(json, &json[4], plen);
	json[plen] = '\0';
	return json;
}

static bool session_error(uint16_t code)
{
	const uint8_t e[] = {mg_command_error, mg_command_session, mg_error_data_code,
			     (uint8_t)code, (uint8_t)(code >> 8)};
	const struct host_note *n = host_nnotes ? &host_notes[host_nnotes - 1] : NULL;

	return n && n->len == sizeof(e) && memcmp(n->data, e, sizeof(e)) == 0;
}

/* A re-setup's commit ends the device session opened with the old
 * credential, not just the proof: its hello no longer counts, and the next
 * one (after a proof with the new K) sends the new credentials. */
static void test_reprovision_ends_session(void)
{
	const uint8_t open[] = {mg_session_state, mg_session_state_open, 1, 0};
	const uint8_t update[] = {mg_session_credentials_update,
				  mg_session_flag_first | mg_session_flag_last,
				  2,
				  0,
				  0,
				  0,
				  '{',
				  '}'};

	static const struct mg_vm_identity id = {.node_id = "homelink-000001"};

	printf("  re-provisioning ends the device session\n");
	mg_vm_link_set_identity(&id);
	fresh();
	host_encrypted = true;
	host_mtu = 247;
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc-k\",\"refresh_token\":"
		     "\"ref-k\",\"token_type\":\"device\",\"device_key\":"
		     "\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\",\"device_key_id\":"
		     "\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_id\":\"vm-k\",\"vm_kind\":\"cvm\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(live_proof("acc-k") == 1);
	host_notes_clear();
	session_hello();
	CHECK(host_nnotes > 1 && host_notes[0].data[1] == mg_session_hello);
	CHECK_STR(session_credentials(),
		  "{\"node_id\":\"homelink-000001\",\"access_token\":\"acc-k\",\"refresh_token\":"
		  "\"ref-k\",\"device_key\":\"8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8=\","
		  "\"key_id\":\"oKGio6SlpqeoqaqrrK2urw==\",\"vm_id\":\"vm-k\",\"vm_kind\":\"cvm\"}");
	mg_vm_session_rx(open, sizeof(open));
	CHECK(mg_vm_session_is_open());

	/* A new setup on the same connection: the session stays until the
	 * commit... */
	handshake_again();
	CHECK(mg_vm_session_is_open() && mg_token_proof_matched());
	provision("access-two", "refresh-2");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	/* ...which ends it and its hello, with the proof. */
	CHECK(!mg_vm_session_is_open() && !mg_token_proof_matched());
	host_notes_clear();
	mg_vm_session_rx(update, sizeof(update));
	CHECK(session_error(mg_error_code_invalid_value));
	host_notes_clear();
	session_hello();
	CHECK(session_error(mg_error_code_proof_required));
	/* The new K's proof, then the new credentials, without a key. */
	CHECK(live_proof("access-two") == 1);
	host_notes_clear();
	session_hello();
	CHECK_STR(session_credentials(),
		  "{\"node_id\":\"homelink-000001\",\"access_token\":\"access-two\","
		  "\"refresh_token\":\"refresh-2\"}");
	/* A failed re-setup ends nothing. */
	mg_vm_session_rx(open, sizeof(open));
	handshake_again();
	host_settings_fail_save(1);
	provision("access-three", "refresh-3");
	CHECK(status_is(app_recv_enc(), "error_storage"));
	CHECK(mg_vm_session_is_open() && mg_token_proof_matched());
	mg_setup_disconnected();
	mg_vm_session_connection_reset();
	host_encrypted = false;
	CHECK(mg_setup_factory_reset("test") == 0);
}
#endif

static void test_reprovision_boot_recovery(void)
{
	uint8_t k1[32] = {1}, k2[32] = {2}, k[32];

	printf("  re-provisioning cut short by a power loss\n");
	host_settings_clear();
	boot();
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k1) == 0);
	/* The new set half written into the free slot, no new record: the old
	 * set stays and the leftovers go. */
	settings_save_one("mg/setup/at1", "access", 6);
	settings_save_one("mg/setup/rt1", "refresh", 7);
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_get_k(k) == 0 && k[0] == 1);
	CHECK(host_settings_count() == 4 && host_settings_len("mg/setup/at1") < 0);

	/* The record moved but the old slot wasn't erased yet: the new set, and
	 * the old entries go. */
	CHECK(mg_setup_store_commit("access", 6, "refresh", 7, k2) == 0);
	CHECK(mg_setup_store_get_k(k) == 0 && k[0] == 2);
	settings_save_one("mg/setup/at", "acc", 3);
	settings_save_one("mg/setup/k", k1, 32);
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_get_k(k) == 0 && k[0] == 2);
	CHECK(host_settings_count() == 4 && host_settings_len("mg/setup/at") < 0 &&
	      host_settings_len("mg/setup/k") < 0);

	/* A record naming a slot whose entries are missing: half-written,
	 * erased (never the other slot's set in its place). */
	settings_delete("mg/setup/k1");
	settings_save_one("mg/setup/at", "acc", 3);
	settings_save_one("mg/setup/rt", "ref", 3);
	settings_save_one("mg/setup/k", k1, 32);
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);

	/* A single-slot record from earlier firmware (slot byte 0) loads. */
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k1) == 0);
	CHECK(host_settings_len("mg/setup/at") == 3);
	CHECK(((const uint8_t *)host_settings_get("mg/setup/done"))[5] == 0);
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_get_k(k) == 0 && k[0] == 1);
	CHECK(mg_setup_store_erase() == 0 && host_settings_count() == 0);

	/* A record from before the VM binding (12 bytes) loads, as a set
	 * without one, the device key included. */
	struct mg_setup_device_key key = {.key = k1, .key_id = k2, .vm_id = "vm", .vm_id_len = 2};
	uint8_t rec[14], dk[32], kid[16];
	char vm_id[MG_SETUP_VM_ID_MAX + 1];
	size_t vm_len;
	enum mg_setup_vm_kind kind;

	CHECK(mg_setup_store_commit_device_key("acc", 3, "ref", 3, k1, &key) == 0);
	CHECK(host_settings_len("mg/setup/done") == 14 + 8); /* + access_expires_at */
	memcpy(rec, host_settings_get("mg/setup/done"), sizeof(rec));
	/* A record from before the expiry (14 bytes) loads too, as none. */
	settings_save_one("mg/setup/done", rec, 14);
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_complete());
	CHECK(mg_setup_store_expires() == 0);
	CHECK(rec[12] == MG_SETUP_VM_KIND_NONE && rec[13] == 2);
	settings_save_one("mg/setup/done", rec, 12);
	settings_delete("mg/setup/vm");
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_get_device_key(dk, kid) == 0 &&
	      dk[0] == 1);
	CHECK(mg_setup_store_get_vm(vm_id, &vm_len, &kind) == 0 && vm_len == 0 &&
	      kind == MG_SETUP_VM_KIND_NONE);
	/* A record whose binding doesn't add up: half-written. */
	rec[12] = 3;
	settings_save_one("mg/setup/done", rec, sizeof(rec));
	settings_save_one("mg/setup/vm", "vm", 2);
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
}

static void test_rollback(void)
{
	printf("  storage failure rolls back\n");
	for (int fail = 1; fail <= 4; fail++) {
		fresh();
		handshake();
		host_settings_fail_save(fail);
		app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc\","
			     "\"refresh_token\":\"ref\",\"token_type\":\"device\"}");
		CHECK(status_is(app_recv_enc(), "error_storage"));
		CHECK(no_more());
		CHECK(!mg_setup_store_complete());
		CHECK(host_settings_count() == 0);
		host_advance(600);
		CHECK(host_disconnects == 1);
		boot();
		CHECK(!mg_setup_store_complete() && mg_setup_advertise_link());
	}
	/* A token too long to store: error_storage, nothing written. */
	static char big[2400], msg[2600];

	fresh();
	handshake();
	memset(big, 'a', 2049);
	big[2049] = 0;
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"%s\",\"refresh_token\":\"r\","
		 "\"token_type\":\"device\"}",
		 big);
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "error_storage"));
	CHECK(host_settings_count() == 0);
}

static void test_boot_recovery(void)
{
	printf("  boot recovery\n");
	host_settings_clear();
	/* Tokens without the commit record: a provisioning cut short. */
	settings_save_one("mg/setup/at", "acc", 3);
	settings_save_one("mg/setup/rt", "ref", 3);
	boot();
	CHECK(!mg_setup_store_complete());
	CHECK(host_settings_count() == 0);

	/* A record whose lengths don't match. */
	uint8_t k[32] = {1};

	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	settings_save_one("mg/setup/rt", "refresh", 7);
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);

	/* Reset cut short after the record went: the rest is erased. */
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	settings_delete("mg/setup/done");
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);

	/* Whole: kept. */
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_wifi_skipped());
	CHECK(mg_setup_store_erase() == 0 && host_settings_count() == 0);
	CHECK(!mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(k) == -ENOENT);
}

/* A commit record that is there but can't be read is not an absent one: the
 * load erases nothing and chooses nothing, a commit meanwhile loads again and
 * refuses while it still can't, and the next good read finds the set. */
static void test_boot_unreadable(void)
{
	uint8_t k[32] = {7}, got[32];
	int before;

	printf("  boot: an unreadable setup record erases nothing\n");
	host_settings_clear();
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	before = host_settings_count();
	host_settings_fail_read(1); /* the record's read */
	CHECK(mg_setup_store_load() == -EIO);
	/* Fail closed: taken as set up, with nothing of the set to use. */
	CHECK(mg_setup_store_complete() && host_settings_count() == before);
	CHECK(mg_setup_store_get_k(got) == -ENOENT);
	host_settings_fail_read(1);
	CHECK(mg_setup_store_commit("acc2", 4, "ref2", 4, k) == -EIO);
	CHECK(host_settings_count() == before);
	host_settings_fail_read(0);
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(got) == 0 && memcmp(got, k, sizeof(k)) == 0);
	CHECK(mg_setup_store_erase() == 0);
}

/* A listing that ends early (ZMS stops, returning 0, at a list link it
 * can't read) or a lookup that fails is not an absent setup either: the
 * load erases nothing and chooses nothing, a commit meanwhile refuses, and
 * the next whole listing finds the set. A half-written set is still erased
 * once the listing is whole. */
static void test_boot_partial_listing(void)
{
	uint8_t k[32] = {9}, got[32];
	int before;

	printf("  boot: a partial listing or a failed lookup erases nothing\n");
	host_settings_clear();
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	before = host_settings_count();
	/* The record unseen (the listing ends before it, or sees nothing). */
	for (int n = 0; n < before; n++) {
		host_settings_list_only(n);
		CHECK(mg_setup_store_load() == -EIO);
		CHECK(mg_setup_store_complete() && host_settings_count() == before);
	}
	host_settings_list_only(1);
	CHECK(mg_setup_store_commit("acc2", 4, "ref2", 4, k) == -EIO);
	CHECK(host_settings_count() == before);
	host_settings_list_only(-1);
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(got) == 0 && memcmp(got, k, sizeof(k)) == 0);

	/* The record seen, a slot entry it names unseen: at listed last. */
	CHECK(settings_delete("mg/setup/at") == 0);
	CHECK(settings_save_one("mg/other", "x", 1) == 0);
	CHECK(settings_save_one("mg/setup/at", "acc", 3) == 0);
	host_settings_list_only(before - 1);
	CHECK(mg_setup_store_load() == -EIO);
	CHECK(mg_setup_store_complete() && host_settings_len("mg/setup/at") == 3 &&
	      host_settings_len("mg/setup/done") > 0 && host_settings_len("mg/setup/k") == 32);
	host_settings_list_only(-1);
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_complete());
	CHECK(mg_setup_store_erase() == 0);
	CHECK(settings_delete("mg/other") == 0 && host_settings_count() == 0);

	/* Half-written, and a lookup fails: kept until a load can tell. */
	CHECK(settings_save_one("mg/setup/at", "acc", 3) == 0);
	CHECK(settings_save_one("mg/setup/rt", "ref", 3) == 0);
	host_settings_fail_len(1);
	CHECK(mg_setup_store_load() == -EIO && host_settings_count() == 2);
	host_settings_fail_len(0);
	CHECK(mg_setup_store_load() == 0 && !mg_setup_store_complete());
	CHECK(host_settings_count() == 0);
}

/* Each token within its limit is not enough: the whole credentials document
 * the device session hands back (node id, the pair, the device key and its
 * binding, room for access_expires_at) must fit MG_SESSION_MAX_CREDENTIALS,
 * checked at the commit; a pair past it stores nothing and keeps the old set. */
static void test_commit_doc_limit(void)
{
	static char a[1201], r[1201];
	uint8_t k[32] = {3}, got[32], dk[MG_SETUP_DEVICE_KEY_LEN] = {1}, kid[MG_SETUP_KEY_ID_LEN] = {2};
	const struct mg_setup_device_key key = {.key = dk, .key_id = kid, .vm_id = "vm-1",
						.vm_id_len = 4, .vm_kind = MG_SETUP_VM_KIND_PLAIN};
	int before;

	printf("  commit: the whole credentials document within its limit\n");
	host_settings_clear();
	(void)mg_setup_store_load();
	memset(a, 'a', 1200);
	memset(r, 'r', 1200);
	CHECK(mg_setup_store_commit(a, 1200, r, 1200, k) == -EMSGSIZE);
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
	a[900] = r[900] = '\0';
	CHECK(mg_setup_store_commit_device_key(a, 900, r, 900, k, &key) == 0);
	before = host_settings_count();
	a[900] = r[900] = 'x';
	a[1000] = r[1000] = '\0';
	CHECK(mg_setup_store_commit_device_key(a, 1000, r, 1000, k, &key) == -EMSGSIZE);
	CHECK(mg_setup_store_complete() && host_settings_count() == before);
	CHECK(mg_setup_store_get_k(got) == 0 && memcmp(got, k, sizeof(k)) == 0);
	CHECK(mg_setup_store_erase() == 0);
}

static void test_timeouts(void)
{
	printf("  timeouts\n");
	/* client_finished after 60 s: the session is gone. */
	fresh();
	uint8_t dpriv[32], dnonce[16];
	size_t n;

	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	CHECK(next_msg() != NULL);
	CHECK(host_connparam_holds & MG_CONNPARAM_HOLD_SETUP);
	/* With nothing more from the client the keys and the fast-link hold go
	 * at the deadline, not at its next write. */
	host_advance(MG_SETUP_CLIENT_FINISHED_MS + 1);
	CHECK(!(host_connparam_holds & MG_CONNPARAM_HOLD_SETUP));
	char env[300];

	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 V->session_id, V->cf_ct, V->cf_tag);
	app_send(env);
	CHECK(no_more()); /* error_pairing_decrypt, but plaintext is blocked now */
	host_advance(400);
	CHECK(host_disconnects == 1);

	/* No timer on the prompt: an hour on, still no timeout and no drop; the
	 * link's end closes it, and then the press is a talk press. */
	fresh();
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	(void)next_msg();
	app_send(env);
	app_ctr = 1;
	CHECK(status_is(app_recv_enc(), "confirm_required"));
	host_advance(60 * 60 * 1000);
	CHECK(no_more() && host_disconnects == 0 && mg_setup_confirm_pending());
	mg_setup_disconnected();
	CHECK(mg_setup_button(true) == false); /* the session is gone */

	/* Confirmed but no provision_v2 within 120 s. */
	fresh();
	handshake();
	host_advance(MG_SETUP_CONFIRMED_IDLE_MS + 10);
	CHECK(status_is(app_recv_enc(), "pairing_confirm_timeout"));
	host_advance(200);
	CHECK(host_disconnects == 1);

	/* A replayed record (old counter) drops the session. */
	fresh();
	handshake();
	app_ctr = 0;
	app_send_enc("{\"action\":\"wifi_scan\"}");
	CHECK(no_more());
	host_advance(400);
	CHECK(host_disconnects == 1);

	/* Disconnecting ends the session: the next connection starts over. */
	fresh();
	handshake();
	mg_setup_disconnected();
	connect();
	app_send_enc("{\"action\":\"wifi_scan\"}");
	CHECK_STR(next_msg(), "error_pairing_decrypt");
}

static void test_sensitive_before_confirm(void)
{
	printf("  sensitive actions need the press\n");
	fresh();
	uint8_t dpriv[32], dnonce[16];
	size_t n;
	char env[300];

	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	(void)next_msg();
	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 V->session_id, V->cf_ct, V->cf_tag);
	app_send(env);
	app_ctr = 1;
	CHECK(status_is(app_recv_enc(), "confirm_required"));
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"a\",\"refresh_token\":\"r\","
		     "\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_pairing_confirm_required"));
	CHECK(host_settings_count() == 0);
	CHECK(mg_setup_button(true));
	CHECK(status_is(app_recv_enc(), "pairing_confirmed"));
	app_send_enc("{\"action\":\"wifi_scan\"}");
	char *r = app_recv_enc();

	CHECK(r && strstr(r, "wifi_scan_result") && strstr(r, "\"networks\":[]"));
}

/* Token proof (mgcommands.h) against mg-token-proof-v1.json, through the
 * command handler. */
/* An ownership change's clip erase owed at boot: a clip store that didn't
 * mount keeps the mark (never taken for a build without one); only a build
 * without a store drops it unerased; a mounted one erases, once. */
static void test_clips_owed(void)
{
	printf("  owed clip erase: a store that didn't mount keeps the mark\n");
	CHECK(mg_clips_owed_step(true, false, false) == MG_CLIPS_OWED_KEEP);
	CHECK(mg_clips_owed_step(true, false, true) == MG_CLIPS_OWED_KEEP);
	CHECK(mg_clips_owed_step(true, true, false) == MG_CLIPS_OWED_ERASE);
	CHECK(mg_clips_owed_step(true, true, true) == MG_CLIPS_OWED_KEEP);
	CHECK(mg_clips_owed_step(false, false, false) == MG_CLIPS_OWED_GONE);
}

extern int host_proof_results;

/* A confirm the device can't check (its key's read or the HMAC failed) is
 * not a mismatch: no result 0 (the app would take it for a gadget that
 * doesn't hold its token), no match, nothing held released or discarded,
 * busy; the next challenge and confirm match. */
static void test_token_proof_key_error(void)
{
	const struct proof_vec *pv = &proof_vecs[0];
	int results;

	printf("  token proof: a key read that fails is busy, never a mismatch\n");
	host_settings_clear();
	boot();
	CHECK(mg_setup_store_commit(pv->access_token, strlen(pv->access_token), "r", 1, pv->K) == 0);
	mg_token_proof_connection_reset();
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(mg_token_proof_pending());
	results = host_proof_results;
	host_notes_clear();
	host_settings_fail_read(1); /* the confirm's read of K */
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	host_settings_fail_read(0);
	CHECK(host_nnotes == 1 && host_notes[0].data[0] == mg_command_error &&
	      host_notes[0].data[1] == mg_command_token_proof &&
	      host_notes[0].data[3] == mg_error_code_busy);
	CHECK(!mg_token_proof_matched() && !mg_token_proof_pending());
	CHECK(host_proof_results == results);
	CHECK(mg_setup_store_complete());
	/* The retry matches. */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	host_notes_clear();
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(host_nnotes == 1 && host_notes[0].len == pv->result.n &&
	      memcmp(host_notes[0].data, pv->result.b, pv->result.n) == 0);
	CHECK(mg_token_proof_matched() && host_proof_results == results + 1);
	mg_token_proof_connection_reset();
}

/* A set boot couldn't read fails closed: the gadget is taken for set up
 * (no first-setup window: no Link setup advertising, a hello outside setup
 * mode refused as on any set-up gadget; the token proof busy, never
 * not_found), none of the set's secrets are used, and a reset's erase
 * opens setup again. */
static void test_boot_unreadable_fails_closed(void)
{
	const struct proof_vec *pv = &proof_vecs[0];
	uint8_t got[32];

	printf("  boot: an unreadable set fails closed (taken as set up)\n");
	host_setup_mode = false;
	host_settings_clear();
	host_disconnects = 0;
	CHECK(mg_setup_store_commit(pv->access_token, strlen(pv->access_token), "r", 1, pv->K) == 0);
	host_settings_fail_read(1); /* the record's read at boot */
	boot();
	host_settings_fail_read(0);
	CHECK(mg_setup_store_complete());
	CHECK(!mg_setup_advertise_link());
	CHECK(mg_setup_store_get_k(got) == -ENOENT);
	connect();
	app_ctr = dev_ctr = 0;
	hello();
	CHECK_STR(next_msg(), "error_pairing_unavailable");
	CHECK(!host_pair_led && no_more());
	host_notes_clear();
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(host_nnotes == 1 && host_notes[0].data[0] == mg_command_error &&
	      host_notes[0].data[1] == mg_command_token_proof &&
	      host_notes[0].data[3] == mg_error_code_busy);
	CHECK(!mg_token_proof_matched());
	/* The reset hold's erase: nothing unread is left, setup is open. */
	CHECK(mg_setup_store_erase() == 0);
	CHECK(!mg_setup_store_complete() && mg_setup_advertise_link());
	host_setup_mode = true;
}

/* The setup store's entries, name, length and bytes, to tell it untouched. */
static const char *const STORE_KEYS[] = {"mg/setup/at",  "mg/setup/rt",  "mg/setup/k",
					 "mg/setup/dk",  "mg/setup/kid", "mg/setup/vm",
					 "mg/setup/at1", "mg/setup/rt1", "mg/setup/k1",
					 "mg/setup/dk1", "mg/setup/kid1", "mg/setup/vm1",
					 "mg/setup/done", "mg/setup/clips"};
struct store_snap {
	int n;
	int len[ARRAY_SIZE(STORE_KEYS)];
	uint8_t val[ARRAY_SIZE(STORE_KEYS)][64];
};

static void store_snap(struct store_snap *sn)
{
	memset(sn, 0, sizeof(*sn));
	sn->n = host_settings_count();
	for (size_t i = 0; i < ARRAY_SIZE(STORE_KEYS); i++) {
		sn->len[i] = host_settings_len(STORE_KEYS[i]);
		if (sn->len[i] > 0) {
			memcpy(sn->val[i], host_settings_get(STORE_KEYS[i]),
			       MIN((size_t)sn->len[i], sizeof(sn->val[i])));
		}
	}
}

/* An unreadable store and the boot hold: setup mode still opens (as on any
 * set-up gadget), the re-setup runs to its press, and its commit, which
 * loads the store again, is refused while it still can't be read
 * (error_storage): the store keeps every entry, length and byte, and the
 * gadget stays failed closed. */
static void test_unreadable_resetup_refused(void)
{
	struct store_snap before, after;
	uint8_t k[32] = {6};

	printf("  boot: an unreadable store in setup mode: the re-setup's commit is refused\n");
	CHECK(mg_setup_mode_begins(false, true)); /* the boot hold, released before the SAFE cue */
	host_setup_mode = true;
	host_settings_clear();
	host_disconnects = 0;
	CHECK(mg_setup_store_commit("acc-old", 7, "ref-old", 7, k) == 0);
	store_snap(&before);
	CHECK(before.n == 4);
	host_settings_fail_read(1); /* the record's read at boot */
	boot();
	host_settings_fail_read(0);
	CHECK(mg_setup_store_complete() && !mg_setup_advertise_link());
	connect();
	app_ctr = dev_ctr = 0;
	handshake(); /* setup mode: the prompt shows, one press confirms */
	host_settings_fail_read(1); /* the commit's load: still unreadable */
	provision("access-new", "refresh-new");
	host_settings_fail_read(0);
	CHECK(status_is(app_recv_enc(), "error_storage"));
	store_snap(&after);
	CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	CHECK(mg_setup_store_complete() && mg_setup_store_get_k(k) == -ENOENT);
	/* Readable again: the old set, untouched. */
	CHECK(mg_setup_store_load() == 0 && mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(k) == 0 && k[0] == 6);
	CHECK(mg_setup_store_erase() == 0);
}

extern int host_button_flushes;

/* A press that began before the prompt armed (queued or down then: the core
 * passes after_arm false) never confirms it: the prompt takes it, and its
 * release; the next press confirms. On a gadget that isn't set up, with
 * setup mode off and on, and on a set-up gadget's re-setup in setup mode. */
static void test_press_before_prompt_never_confirms(void)
{
	static const char *const what[] = {"not set up", "not set up, setup mode",
					   "set up, setup mode"};

	printf("  setup: a press from before the prompt never confirms it\n");
	for (int i = 0; i < 3; i++) {
		int flushes;

		host_setup_mode = i > 0;
		if (i < 2) {
			fresh();
			CHECK(!mg_setup_store_complete());
		} else {
			CHECK(mg_setup_store_complete());
		}
		app_ctr = dev_ctr = 0;
		flushes = host_button_flushes;
		stop_at_prompt = true;
		handshake();
		stop_at_prompt = false;
		CHECK(host_button_flushes == flushes + 1); /* the prompt armed the queue */
		/* The press down when it armed, then its release (the core sends
		 * setup nothing for a release). */
		CHECK(mg_setup_button_at(true, false));
		CHECK(mg_setup_confirm_pending() && host_pair_led && no_more());
		/* A new press confirms. */
		CHECK(mg_setup_button_at(true, true));
		CHECK(!host_pair_led && status_is(app_recv_enc(), "pairing_confirmed"));
		provision(i == 2 ? "access-two" : "access-one", "refresh");
		CHECK(status_is(app_recv_enc(), "auth_ok"));
		CHECK(holds_k_for(i == 2 ? "access-two" : "access-one"));
		if (host_failures) {
			printf("    (%s)\n", what[i]);
		}
	}
	host_setup_mode = true;
}

/* On a release build (this one) only the button confirms a setup or a
 * re-setup: an injected press is taken by the prompt (never a talk press)
 * and confirms nothing, set up or not, in setup mode or not; the button's
 * press then confirms. */
static void test_injected_press_never_confirms(void)
{
	printf("  setup: an injected press never confirms (release build)\n");
	for (int again = 0; again < 2; again++) {
		if (!again) {
			fresh();
		} else {
			app_ctr = dev_ctr = 0;
		}
		stop_at_prompt = true;
		handshake();
		stop_at_prompt = false;
		CHECK(mg_setup_confirm_pending() && host_pair_led);
		CHECK(mg_setup_button(false)); /* taken: not a talk press */
		CHECK(mg_setup_confirm_pending() && host_pair_led && no_more());
		CHECK(mg_setup_button(true));
		CHECK(!host_pair_led && status_is(app_recv_enc(), "pairing_confirmed"));
		provision(again ? "access-two" : "access-one", "refresh");
		CHECK(status_is(app_recv_enc(), "auth_ok"));
		CHECK(mg_setup_store_complete() && holds_k_for(again ? "access-two" : "access-one"));
	}
}

static void test_token_proof(void)
{
	printf("  token proof\n");
	for (int i = 0; i < PROOF_VEC_COUNT; i++) {
		const struct proof_vec *pv = &proof_vecs[i];
		uint8_t k[32], mac[32];

		printf("    vector %s\n", pv->name);
		CHECK(mg_token_proof_derive_key(pv->access_token, strlen(pv->access_token),
						pv->node_id, k) == 0);
		CHECK_MEM(k, pv->K, 32);
		CHECK(mg_token_proof_mac(k, true, pv->client_nonce, pv->device_nonce, mac) == 0);
		CHECK_MEM(mac, pv->device_mac, 32);
		CHECK(mg_token_proof_mac(k, false, pv->client_nonce, pv->device_nonce, mac) == 0);
		CHECK_MEM(mac, pv->client_mac, 32);

		/* The exchange, frame for frame. */
		host_settings_clear();
		boot();
		CHECK(mg_setup_store_commit(pv->access_token, strlen(pv->access_token), "r", 1, k) ==
		      0);
		host_notes_clear();
		mg_token_proof_connection_reset();
		mg_token_proof_test_set_nonce(pv->device_nonce);
		mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
		CHECK(host_nnotes == 1 && host_notes[0].ch == MG_ATT_CONTROL &&
		      host_notes[0].len == pv->response.n &&
		      memcmp(host_notes[0].data, pv->response.b, pv->response.n) == 0);
		mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
		CHECK(host_nnotes == 2 && host_notes[1].len == pv->result.n &&
		      memcmp(host_notes[1].data, pv->result.b, pv->result.n) == 0);
		CHECK(mg_token_proof_matched());
		/* clear: result 1 first, then everything is erased. */
		int refreshes = host_adv_refreshes;

		mg_token_proof_rx(&pv->clear.b[1], pv->clear.n - 1);
		CHECK(host_nnotes == 3 && host_notes[2].len == 3 && host_notes[2].data[2] == 1);
		CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
		CHECK(mg_setup_advertise_link() && host_adv_refreshes > refreshes);
		CHECK(!mg_token_proof_matched());
	}

	const struct proof_vec *pv = &proof_vecs[0];
	uint8_t bad[33];

#define LAST_ERR(code)                                                                             \
	(host_nnotes > 0 && host_notes[host_nnotes - 1].data[0] == mg_command_error &&            \
	 host_notes[host_nnotes - 1].data[1] == mg_command_token_proof &&                          \
	 host_notes[host_nnotes - 1].data[3] == (code))

	/* No token: challenge is not_found. */
	host_settings_clear();
	boot();
	host_notes_clear();
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(LAST_ERR(mg_error_code_not_found));
	/* clear before a match: proof_required, and nothing is erased. */
	CHECK(mg_setup_store_commit(pv->access_token, strlen(pv->access_token), "r", 1, pv->K) == 0);
	mg_token_proof_connection_reset();
	host_notes_clear();
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required));
	CHECK(mg_setup_store_complete());
	/* confirm without a pending response: invalid_value. */
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(LAST_ERR(mg_error_code_invalid_value));
	/* Wrong lengths. */
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 2);
	CHECK(LAST_ERR(mg_error_code_invalid_length));
	mg_token_proof_rx((const uint8_t *)"", 0);
	CHECK(LAST_ERR(mg_error_code_invalid_length));
	/* Device -> client sub-commands and unknown ones: unsupported. */
	bad[0] = mg_token_proof_response;
	mg_token_proof_rx(bad, 1);
	CHECK(LAST_ERR(mg_error_code_unsupported));
	bad[0] = 9;
	mg_token_proof_rx(bad, 1);
	CHECK(LAST_ERR(mg_error_code_unsupported));
	/* A wrong client MAC: result 0, and clear stays refused. */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	memcpy(bad, &pv->confirm.b[1], 33);
	bad[32] ^= 1;
	host_notes_clear();
	mg_token_proof_rx(bad, 33);
	CHECK(host_nnotes == 1 && host_notes[0].data[0] == mg_command_token_proof &&
	      host_notes[0].data[1] == mg_token_proof_result && host_notes[0].data[2] == 0);
	CHECK(!mg_token_proof_matched());
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1); /* nothing pending now */
	CHECK(LAST_ERR(mg_error_code_invalid_value));
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required));
	/* A match, then a new challenge: the exchange restarts (no clear until it
	 * matches again). */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(mg_token_proof_matched());
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(!mg_token_proof_matched());
	/* Random device nonces differ, and the MAC follows them. */
	host_notes_clear();
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(host_nnotes == 2 && memcmp(&host_notes[0].data[2], &host_notes[1].data[2], 16) != 0);
	uint8_t mac[32];

	mg_token_proof_mac(pv->K, true, pv->client_nonce, &host_notes[1].data[2], mac);
	CHECK_MEM(mac, &host_notes[1].data[18], 32);
	/* A match counts only on its own connection, even before the
	 * connection events reach the app work queue (SMP asks from its own
	 * thread). */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(mg_token_proof_matched());
	host_conn_gen++;
	CHECK(!mg_token_proof_matched());
	host_notes_clear();
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required) && mg_setup_store_complete());
	/* A confirm queued on one connection and handled after the next one
	 * came up binds nothing to the new link: no match, update writes stay
	 * refused and clear needs a proof. */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(mg_token_proof_pending());
	host_conn_gen++;
	CHECK(!mg_token_proof_pending());
	host_notes_clear();
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(LAST_ERR(mg_error_code_invalid_value) && !mg_token_proof_matched());
	CHECK(!mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, mg_token_proof_matched(),
				  false, false, true, false));
	host_notes_clear();
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required) && mg_setup_store_complete());
	/* The connection ends: no match carries over. */
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1); /* result 0 (other nonce) */
	mg_token_proof_connection_reset();
	host_notes_clear();
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required));
}

static void test_reset(void)
{
	printf("  factory reset\n");
	fresh();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		     "\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_factory_reset("test") == 0);
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0 && mg_setup_advertise_link());
	/* Set up again from scratch on a new connection. */
	mg_setup_disconnected();
	connect();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc2\",\"refresh_token\":"
		     "\"ref2\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(host_settings_len("mg/setup/at") == 4);
}

/* A new setup offers and stores gadget-<6 hex>; a
 * gadget set up before (a commit record without the flag) keeps its
 * homelink- id, through reboots and token updates, until it is set up again. */
static uint8_t rec[32];
static size_t rec_len;

static int rec_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		  void *param)
{
	(void)param;
	if (key == NULL && len <= sizeof(rec)) {
		rec_len = (size_t)read_cb(cb_arg, rec, len);
	}
	return 0;
}

/* mg_setup_store.c's commit hook (weak there): what the app runs at an
 * ownership change (settings flushed, a staged image dropped). */
static int committed_calls;

void mg_setup_store_committed(void)
{
	committed_calls++;
}

/* A setup's commit is an ownership change, a
 * token refresh (credentials_update, >expire) is not: the hook that drops an
 * image staged before the owner runs only at a setup. */
static void test_commit_hook(void)
{
	uint8_t k[MG_SETUP_K_LEN] = {1};

	printf("  commit hook: a setup runs it, a token refresh doesn't\n");
	host_settings_clear();
	boot();
	committed_calls = 0;
	CHECK(mg_setup_store_commit("a1", 2, "r1", 2, k) == 0);
	CHECK(committed_calls == 1);
	CHECK(mg_setup_store_update_tokens("a2", 2, "r2", 2) == 0);
	CHECK(mg_setup_store_update_tokens_exp("a3", 2, "r3", 2, 1) == 0);
	CHECK(committed_calls == 1);
	CHECK(mg_setup_store_commit("a4", 2, "r4", 2, k) == 0); /* a re-setup */
	CHECK(committed_calls == 2);
	/* A refused setup is no ownership change. */
	CHECK(mg_setup_store_commit("", 0, "r5", 2, k) != 0);
	CHECK(committed_calls == 2);
}

static void test_node_ids(void)
{
	static struct mg_json_obj o;
	static const uint8_t k[MG_SETUP_K_LEN] = {1};

	printf("  node ids: gadget- at a new setup, a stored homelink- kept\n");
	offer_gadget = true;
	fresh();
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	app_send("{\"action\":\"get_device_info\"}");
	char *m = next_msg();

	CHECK(m && mg_json_parse(m, strlen(m), &o) == 0);
	CHECK_STR(mg_json_str(&o, "node_id"), "gadget-000001");
	/* A new setup: gadget-, kept across a reboot and a token update. */
	CHECK(mg_setup_store_commit("a1", 2, "r1", 2, k) == 0);
	CHECK(mg_setup_store_gadget_node());
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	boot();
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	CHECK(mg_setup_store_update_tokens("a2", 2, "r2", 2) == 0);
	boot();
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	/* A setup from before the gadget- form: the same record without the flag. */
	rec_len = 0;
	(void)settings_load_subtree_direct("mg/setup/done", rec_cb, NULL);
	CHECK(rec_len > 5 && (rec[4] & 0x10));
	rec[4] &= (uint8_t)~0x10;
	CHECK(settings_save_one("mg/setup/done", rec, rec_len) == 0);
	boot();
	CHECK(mg_setup_store_complete() && !mg_setup_store_gadget_node());
	CHECK_STR(mg_identity_node_id(), "homelink-000001");
	CHECK_STR(mg_identity_offer_node_id(), "gadget-000001");
	CHECK(mg_setup_store_update_tokens("a3", 2, "r3", 2) == 0); /* a refresh keeps it */
	boot();
	CHECK_STR(mg_identity_node_id(), "homelink-000001");
	/* device_info offers what a setup now would give it. */
	connect();
	app_send("{\"action\":\"get_device_info\"}");
	m = next_msg();
	CHECK(m && mg_json_parse(m, strlen(m), &o) == 0);
	CHECK_STR(mg_json_str(&o, "node_id"), "gadget-000001");
	/* Set up again: gadget-. */
	CHECK(mg_setup_store_commit("a4", 2, "r4", 2, k) == 0);
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	boot();
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	/* Erased: the offer again. */
	CHECK(mg_setup_store_erase() == 0);
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	/* CONFIG_MG_NODE_ID_GADGET off: a setup offers and
	 * stores homelink-, and a gadget- set stays gadget- (the id goes with K,
	 * whatever a later image offers). */
	CHECK(mg_setup_store_commit("a5", 2, "r5", 2, k) == 0);
	mg_identity_set_offer_gadget(false);
	CHECK_STR(mg_identity_node_id(), "gadget-000001");
	CHECK_STR(mg_identity_offer_node_id(), "homelink-000001");
	CHECK(mg_setup_store_commit("a6", 2, "r6", 2, k) == 0);
	CHECK(!mg_setup_store_gadget_node());
	CHECK_STR(mg_identity_node_id(), "homelink-000001");
	mg_identity_set_offer_gadget(true); /* and a later gadget- image keeps it */
	boot();
	CHECK_STR(mg_identity_node_id(), "homelink-000001");
	offer_gadget = false;
	fresh();
}

static void test_small_mtu(void)
{
	printf("  default MTU (23): chunks of 17 bytes\n");
	host_mtu = 23;
	fresh();
	handshake();
	for (int i = 0; i < host_nnotes; i++) {
		CHECK(host_notes[i].len <= 20);
	}
	host_mtu = 185;
}

#if defined(CONFIG_MG_SDK_TOKEN_SLOT) || defined(CONFIG_MG_SDK_TOKEN_BOARD)
/* One setup with the slot as it is: whether pairing_confirmed carries
 * `want` (NULL: no sdk_token at all), and that nothing logged the token. */
static void slot_case(const char *what, const char *want, const char *never_logged)
{
	printf("  SDK token: %s\n", what);
	host_log_clear();
	fresh(); /* boot() runs mg_setup_init(), which says whether it has a token */
	handshake();
	const char *pc = handshake_confirmed;

	CHECK(pc != NULL);
	if (!pc) {
		return;
	}
	if (want) {
		char exp[MG_SDK_TOKEN_SLOT_LEN + 16];

		snprintf(exp, sizeof(exp), "\"sdk_token\":\"%s\"", want);
		CHECK(strstr(pc, exp) != NULL);
		char n[48];

		snprintf(n, sizeof(n), "SDK token (%u chars)", (unsigned int)strlen(want));
		CHECK(strstr(host_log_buf, n) != NULL);
		CHECK(strstr(host_log_buf, "no SDK token") == NULL);
	} else {
		CHECK(strstr(pc, "sdk_token") == NULL);
		CHECK(strstr(host_log_buf, "no SDK token") != NULL);
		CHECK(strstr(host_log_buf, "without an SDK token") != NULL);
	}
	if (never_logged) {
		CHECK(strstr(host_log_buf, never_logged) == NULL);
	}
}

#endif

#if defined(CONFIG_MG_SDK_TOKEN_SLOT)
static void slot_fill(const char *token)
{
	memset(mg_sdk_token_slot, 0, sizeof(mg_sdk_token_slot));
	memcpy(mg_sdk_token_slot, token, strlen(token));
}

static void test_token_slot(void)
{
	static char full[MG_SDK_TOKEN_SLOT_LEN + 1];
	static char longest[MG_SDK_TOKEN_SLOT_LEN];

	/* As built: the marker, then NULs. */
	memset(mg_sdk_token_slot, 0, sizeof(mg_sdk_token_slot));
	memcpy(mg_sdk_token_slot, MG_SDK_TOKEN_SLOT_MARKER, strlen(MG_SDK_TOKEN_SLOT_MARKER));
	slot_case("empty (the marker)", NULL, NULL);

	/* Filled by the updater; the slot wins over CONFIG_MG_SDK_TOKEN. */
	slot_fill("mgst_hostSlotToken_0123456789abcdefghijklmnopqrstuv");
	slot_case("filled", "mgst_hostSlotToken_0123456789abcdefghijklmnopqrstuv",
		  "hostSlotToken");

	/* The longest token that fits: 127 characters and the NUL. */
	memcpy(longest, "mgst_", 5);
	memset(longest + 5, 'L', sizeof(longest) - 6);
	longest[sizeof(longest) - 1] = '\0';
	slot_fill(longest);
	slot_case("127 characters (the most that fits)", longest, "LLLLLLLLLLLLLLLL");

	/* 128 characters, no NUL: malformed, taken as no token (and never read
	 * past the slot: AddressSanitizer). */
	memcpy(full, "mgst_", 5);
	memset(full + 5, 'F', MG_SDK_TOKEN_SLOT_LEN - 5);
	memcpy(mg_sdk_token_slot, full, MG_SDK_TOKEN_SLOT_LEN);
	slot_case("128 characters, no NUL (malformed)", NULL, "FFFFFFFFFFFFFFFF");

	/* A token that starts like the marker ("M..."): still a token; only the
	 * whole marker means none. */
	slot_fill("MG-SDK-TOKEN-mgst_notTheMarker");
	slot_case("starts like the marker", "MG-SDK-TOKEN-mgst_notTheMarker", "notTheMarker");

	/* All NULs (a slot wiped by hand): no token. */
	memset(mg_sdk_token_slot, 0, sizeof(mg_sdk_token_slot));
	slot_case("all NULs", NULL, NULL);

	/* Back to the marker for the other tests' boots. */
	memset(mg_sdk_token_slot, 0, sizeof(mg_sdk_token_slot));
	memcpy(mg_sdk_token_slot, MG_SDK_TOKEN_SLOT_MARKER, strlen(MG_SDK_TOKEN_SLOT_MARKER));
}
#endif

#if defined(CONFIG_MG_SDK_TOKEN_BOARD)
/* The token from the board (CONFIG_MG_SDK_TOKEN_BOARD): the slot's rules, and
 * CONFIG_MG_SDK_TOKEN ignored. */
static void test_token_board(void)
{
	static char longest[MG_SDK_TOKEN_SLOT_LEN];
	static char over[MG_SDK_TOKEN_SLOT_LEN + 1];

	board_token = NULL;
	slot_case("none from the board", NULL, CONFIG_MG_SDK_TOKEN);
	board_token = "";
	slot_case("empty from the board", NULL, CONFIG_MG_SDK_TOKEN);
	board_token = "mgst_hostBoardToken_0123456789abcdefghijklmnopqrstu";
	slot_case("from the board", board_token, "hostBoardToken");
	CHECK(strstr(handshake_confirmed, CONFIG_MG_SDK_TOKEN) == NULL);
	/* 127 characters and the NUL: the most it takes. */
	memcpy(longest, "mgst_", 5);
	memset(longest + 5, 'L', sizeof(longest) - 6);
	longest[sizeof(longest) - 1] = '\0';
	board_token = longest;
	slot_case("127 characters from the board", longest, "LLLLLLLLLLLLLLLL");
	/* 128 characters: too long, no token (and never read past the bound). */
	memcpy(over, "mgst_", 5);
	memset(over + 5, 'O', sizeof(over) - 6);
	over[sizeof(over) - 1] = '\0';
	board_token = over;
	slot_case("128 characters from the board (too long)", NULL, "OOOOOOOOOOOOOOOO");
	board_token = NULL;
}

/* mg_board_setup_state(): at boot and at every change. */
static void test_board_setup_state(void)
{
	printf("  board hook: the setup state\n");
	board_setup_calls = 0;
	fresh();
	CHECK(board_setup_calls == 1 && !board_set_up);
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		     "\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(board_set_up);
	int calls = board_setup_calls;

	boot();
	CHECK(board_setup_calls == calls + 1 && board_set_up);
	CHECK(mg_setup_factory_reset("test") == 0);
	CHECK(board_setup_calls > calls + 1 && !board_set_up);
}
#endif

static void setup_all(void)
{
	CHECK(mg_psa_init() == 0);
	V = &link_vecs[0];
	CHECK_STR(V->name, "community_v5");
	memcpy(dev_tx, V->mobile_rx_key, 32);
	memcpy(dev_rx, V->mobile_tx_key, 32);
	host_log_verbose = getenv("HOST_VERBOSE") != NULL;
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_init();
#endif
}

HOST_MAIN(setup_all, test_resetup_bad_takeover, test_commit_hook, test_node_ids, test_device_info, test_plaintext_rules, test_provision, test_device_key,
	  test_reprovision, test_resetup_rate_flow, test_resetup_setup_mode, test_reprovision_device_key,
#if defined(CONFIG_MG_VM_SESSION)
	  test_reprovision_ends_session,
#endif
	  test_reprovision_boot_recovery, test_rollback, test_boot_recovery, test_boot_unreadable, test_boot_partial_listing, test_commit_doc_limit, test_timeouts,
	  test_sensitive_before_confirm, test_token_proof, test_token_proof_key_error, test_boot_unreadable_fails_closed, test_unreadable_resetup_refused, test_injected_press_never_confirms, test_press_before_prompt_never_confirms, test_clips_owed, test_reset, test_small_mtu
#if defined(CONFIG_MG_SDK_TOKEN_SLOT)
	  , test_token_slot
#endif
#if defined(CONFIG_MG_SDK_TOKEN_BOARD)
	  , test_token_board, test_board_setup_state
#endif
	  )
