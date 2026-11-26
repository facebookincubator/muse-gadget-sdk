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
 * Instructions retired (minstret, QEMU -icount) and stack high-water on
 * rv32 for the crypto primitives and for each step of a whole setup
 * session through the core, with the link_pairing_v5 vector's keys.
 */

#include <stdint.h>
#include <string.h>

#include "mg_link_setup.h"
#include "mg_token_proof.h"
#include "mgls_crypto.h"
#include "mgls_util.h"
#include "vectors_gen.h"

extern uint32_t _stack_bottom[];

static void putc_(char c)
{
	*(volatile uint8_t *)0x10000000 = (uint8_t)c;
}

static void puts_(const char *s)
{
	while (*s) {
		putc_(*s++);
	}
}

static void putu(uint32_t v)
{
	char b[12];
	int i = 11;

	b[i] = 0;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	puts_(&b[i]);
}

static inline uint32_t instret(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, minstret" : "=r"(v));
	return v;
}

static inline uint32_t *sp_now(void)
{
	uint32_t *sp;

	__asm__ volatile("mv %0, sp" : "=r"(sp));
	return sp;
}

/* Paints everything below the caller's stack pointer. */
static __attribute__((noinline)) void paint(uint32_t *below)
{
	volatile uint32_t *p = _stack_bottom;

	while ((uintptr_t)p < (uintptr_t)below - 128) {
		*p++ = 0xa5a5a5a5u;
	}
}

static __attribute__((noinline)) uint32_t used_below(uint32_t *top)
{
	uint32_t *p = _stack_bottom;

	while (p < top && *p == 0xa5a5a5a5u) {
		p++;
	}
	return (uint32_t)((uintptr_t)top - (uintptr_t)p);
}

static uint32_t t0;
static uint32_t *base;

/* The stack used below main's stack pointer: what the measured call and
 * everything it calls took. */
#define BEGIN() (base = sp_now(), paint(base), t0 = instret())
#define END(name)                                                                                  \
	do {                                                                                       \
		uint32_t n = instret() - t0;                                                       \
		puts_(name);                                                                       \
		puts_(": ");                                                                       \
		putu(n);                                                                           \
		puts_(" instructions, stack ");                                                    \
		putu(used_below(base));                                                            \
		puts_(" B\n");                                                                     \
	} while (0)

/* ---- a fake platform -------------------------------------------------------- */

static mgls_workspace_t ws;
static uint8_t nonce_q[16];
static int nonce_set;
static uint32_t rng_state = 1;
static uint32_t notified;
static char last[1024];
static size_t last_len;

static int rng(void *ctx, uint8_t *out, size_t len)
{
	(void)ctx;
	if (len == 16 && nonce_set) {
		memcpy(out, nonce_q, 16);
		nonce_set = 0;
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		rng_state = rng_state * 1103515245u + 12345u;
		out[i] = (uint8_t)(rng_state >> 16);
	}
	return 0;
}

static void *borrow(void *ctx, size_t need)
{
	(void)ctx;
	(void)need;
	return &ws;
}

static void release(void *ctx, void *p)
{
	(void)ctx;
	(void)p;
}

static int notify(void *ctx, const uint8_t *d, size_t n)
{
	(void)ctx;
	notified += (uint32_t)n;
	if (n >= 3 && d[0] == 0xFE) {
		if (d[1] == 0) {
			last_len = 0;
		}
		if (last_len + n - 3 < sizeof(last)) {
			memcpy(&last[last_len], &d[3], n - 3);
			last_len += n - 3;
		}
	}
	return 0;
}

static int commit(void *ctx, const mgls_credentials_t *c)
{
	(void)ctx;
	(void)c;
	return 0;
}

static const char *sdk(void *ctx, size_t *len)
{
	(void)ctx;
	*len = 48;
	return "mgst_TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT";
}

static void disconnect(void *ctx)
{
	(void)ctx;
}

static int provisioned, storage_failed;

static void on_event(void *ctx, enum mgls_event ev)
{
	(void)ctx;
	if (ev == MGLS_EV_PROVISIONED) {
		provisioned = 1;
	} else if (ev == MGLS_EV_STORAGE_FAILED) {
		storage_failed = 1;
	}
}

static bool set_up(void *ctx)
{
	(void)ctx;
	return false; /* a first setup */
}

static bool may_prompt(void *ctx, uint32_t since_ms)
{
	(void)ctx;
	return since_ms >= MGLS_RESETUP_PROMPT_MS; /* a release build's rate */
}

MGLS_STATIC_HOOKS(set_up, may_prompt);

static const uint8_t addr[6] = {0x02, 0, 0, 0, 0, 0x01};

static const mgls_config_t cfg = {
	.bd_addr = addr,
	.firmware_version = "1.0.0",
	.mgcommands = 1,
	.borrow = borrow,
	.release = release,
	.notify = notify,
	.disconnect = disconnect,
	.random = rng,
	.commit = commit,
	.sdk_token = sdk,
	.event = on_event,
	.set_up = set_up,
	.may_prompt = may_prompt,
};

static int get_k(void *ctx, uint8_t k[32])
{
	(void)ctx;
	memset(k, 7, 32);
	return 0;
}

static int tp_send(void *ctx, const uint8_t *f, size_t n)
{
	(void)ctx;
	(void)f;
	(void)n;
	return 0;
}

static const mgtp_platform_t tpp = {NULL, get_k, rng, tp_send, NULL};

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = strlen(h) / 2;

	for (size_t i = 0; i < n; i++) {
		unsigned int hi = (unsigned int)(h[2 * i] <= '9' ? h[2 * i] - '0' : h[2 * i] - 'a' + 10);
		unsigned int lo =
			(unsigned int)(h[2 * i + 1] <= '9' ? h[2 * i + 1] - '0' : h[2 * i + 1] - 'a' + 10);

		out[i] = (uint8_t)(hi << 4 | lo);
	}
	return n;
}

static mgls_t s;

/* Writes msg as the phone does, in 244-byte chunks (MTU 247). */
static void write_msg(const char *msg, size_t len)
{
	uint8_t pkt[247];
	size_t room = 241, chunks = (len + room - 1) / room;

	for (size_t i = 0; i < chunks; i++) {
		size_t n = len - i * room < room ? len - i * room : room;

		pkt[0] = 0xFE;
		pkt[1] = (uint8_t)i;
		pkt[2] = (uint8_t)chunks;
		memcpy(&pkt[3], &msg[i * room], n);
		mgls_rx(&s, pkt, n + 3);
	}
}

static char msgbuf[4096];
static uint8_t recbuf[2600];

int main(void)
{
	static uint8_t buf[2560 + 16], pub[65], ss[32], out[32], key[32] = {1}, nonce[12];
	mglsc_p256_t k;
	uint32_t now = 1000;

	puts_("rv32 (QEMU, -icount): instructions retired and stack high-water\n");
	mglsc_init(rng, NULL);

	BEGIN();
	mglsc_p256_keygen(&k, pub);
	END("P-256 keygen");
	unhex(V5_DEVICE_PRIVATE_SCALAR_HEX, buf);
	mglsc_p256_import(&k, buf, pub);
	{
		size_t n;

		mgls_b64_decode(V5_MOBILE_PUB, strlen(V5_MOBILE_PUB), pub, sizeof(pub), &n);
	}
	BEGIN();
	mglsc_p256_ecdh(&k, pub, ss);
	END("P-256 ECDH (point check + shared secret)");
	BEGIN();
	mglsc_gcm_seal(key, nonce, (const uint8_t *)"aad", 3, buf, 2560);
	END("AES-256-GCM seal, 2560 B");
	BEGIN();
	mglsc_sha256(buf, 1024, out);
	END("SHA-256, 1024 B");
	BEGIN();
	mglsc_hkdf32((const uint8_t *)"salt", 4, buf, 600, (const uint8_t *)"info", 4, out);
	END("HKDF-SHA256 (600 B IKM, the proof key K)");

	/* A whole session through the core. */
	if (mgls_init(&s, &cfg) != MGLS_OK) {
		puts_("  ERROR: mgls_init: MGLS_E_CONFIG (a required hook missing)\n");
		return 0;
	}
	mgls_connected(&s, now);
	mgls_mtu_changed(&s, 247);
	{
		mgj_w w;
		uint8_t d[32];
		int n;

		unhex(V5_DEVICE_PRIVATE_SCALAR_HEX, d);
		mgls_test_set_ephemeral(d);
		{
			size_t dl;

			mgls_b64_decode(V5_DEVICE_NONCE, strlen(V5_DEVICE_NONCE), nonce_q, 16, &dl);
			nonce_set = 1;
		}
		mgj_begin(&w, msgbuf, sizeof(msgbuf));
		mgj_kstr(&w, "action", "pairing_client_hello");
		mgj_kraw(&w, "version", "5");
		mgj_kstr(&w, "pairing_auth", "none");
		mgj_kstr(&w, "pairing_policy", "confirm_press");
		mgj_kstr(&w, "mobile_pub", V5_MOBILE_PUB);
		mgj_kstr(&w, "mobile_nonce", V5_MOBILE_NONCE);
		n = mgj_end(&w);
		write_msg(msgbuf, (size_t)n);
	}
	BEGIN();
	mgls_poll(&s, now += 10);
	END("hello -> pairing_ready (mgls_poll)");
	{
		mgj_w w;
		int n;

		mgj_begin(&w, msgbuf, sizeof(msgbuf));
		mgj_kstr(&w, "action", "pairing_encrypted");
		mgj_kstr(&w, "session_id", V5_SESSION_ID);
		mgj_kstr(&w, "counter", "0");
		mgj_kstr(&w, "ciphertext", V5_CLIENT_FINISHED_CIPHERTEXT);
		mgj_kstr(&w, "tag", V5_CLIENT_FINISHED_TAG);
		n = mgj_end(&w);
		write_msg(msgbuf, (size_t)n);
	}
	BEGIN();
	mgls_poll(&s, now += 10);
	END("client_finished -> confirm_required");
	mgls_poll(&s, now += 10); /* the press is armed at the poll after the prompt went out */
	(void)mgls_press(&s);     /* a first setup: the talk key confirms it at the next poll */
	BEGIN();
	mgls_poll(&s, now += 10);
	END("press -> pairing_confirmed");
	{
		/* provision_v2 with a 600 B and a 1,350 B token, sealed as the phone
		 * does (counter 1). */
		mgj_w w;
		uint8_t txk[32], nn[12] = {0};
		char aad[80], tok[1351];
		int n;

		memset(tok, 'A', sizeof(tok));
		tok[600] = 0;
		mgj_begin(&w, (char *)recbuf, sizeof(recbuf) - 16);
		mgj_kstr(&w, "action", "provision_v2");
		mgj_kstr(&w, "access_token", tok);
		tok[600] = 'A';
		tok[1350] = 0;
		mgj_kstr(&w, "refresh_token", tok);
		mgj_kstr(&w, "token_type", "device");
		n = mgj_end(&w);
		unhex(V5_MOBILE_TX_KEY_HEX, txk);
		nn[11] = 1;
		mgj_begin(&w, aad, sizeof(aad));
		mgj_cstr(&w, "hatch-link ble setup v1|" V5_SESSION_ID "|m2d|1");
		(void)mgj_finish(&w);
		mglsc_gcm_seal(txk, nn, (const uint8_t *)aad, strlen(aad), recbuf, (size_t)n);
		mgj_begin(&w, msgbuf, sizeof(msgbuf));
		mgj_cstr(&w, "{\"action\":\"pairing_encrypted\",\"session_id\":\"" V5_SESSION_ID
			     "\",\"counter\":\"1\",\"ciphertext\":\"");
		w.len += (size_t)mgls_b64_encode(recbuf, (size_t)n, &msgbuf[w.len], sizeof(msgbuf) - w.len);
		mgj_cstr(&w, "\",\"tag\":\"");
		w.len += (size_t)mgls_b64_encode(&recbuf[n], 16, &msgbuf[w.len], sizeof(msgbuf) - w.len);
		mgj_cstr(&w, "\"}");
		n = mgj_finish(&w);
		write_msg(msgbuf, (size_t)n);
	}
	last_len = 0;
	BEGIN();
	mgls_poll(&s, now += 10);
	END("provision_v2 (2.0 KB) -> commit -> auth_ok");
	/* The record is sealed: an error_storage answer is one too, so the core's event tells them apart. */
	if (last_len == 0 || memcmp(last, "{\"type\":\"pairing_encrypted\"", 27) != 0) {
		puts_("  ERROR: no record after provision_v2\n");
	} else if (storage_failed) {
		puts_("  ERROR: error_storage record sent (the commit failed)\n");
	} else if (provisioned) {
		puts_("  auth_ok record sent\n");
	} else {
		puts_("  ERROR: a record sent, but not auth_ok\n");
	}

	mgtp_t tp;
	uint8_t ch[17] = {0};

	mgtp_init(&tp, &tpp);
	BEGIN();
	mgtp_rx(&tp, ch, sizeof(ch));
	END("token proof challenge -> response");
	return 0;
}
