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

#ifndef MG_SESSION_SEAL_H_
#define MG_SESSION_SEAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The sealed device session (protocols/mgcommands.h, Device session, "Sealed
 * credentials" and "Sealed data"): its credentials, credentials_update,
 * credentials_saved and data messages.
 * Layer 1 of portable/mg_device_session: the keys, the keystream, the tag and
 * the counter check. Built on the token proof's crypto backend
 * (portable/mg_link_setup, mglsc_hmac_sha256); the streaming tag adds a small
 * incremental SHA-256 of its own (MGSEAL_STREAM), or with MGSEAL_EXT_SHA uses
 * the backend's (mglsc_sha256_init / _update / _final).
 *
 *   enc_key || mac_key = HKDF-SHA256(salt "mg session seal v1", IKM K,
 *                                    info client_nonce || device_nonce || node_id, 64)
 *   keystream block i  = HMAC-SHA256(enc_key, "mg session seal v1 stream"
 *                                    || dir || counter || i)
 *   tag                = HMAC-SHA256(mac_key, "mg session seal v1 tag" || dir
 *                                    || counter || len || ciphertext)[0:16]
 *   sealed             = counter || ciphertext || tag
 *
 * counter, i and len are big-endian uint32. dir MGSEAL_DIR_DEVICE (1) for
 * credentials and the device's data, MGSEAL_DIR_CLIENT (2) for
 * credentials_update and the client's data. Each direction has one counter,
 * shared by every sealed message in it: it starts at 0 after every sealed
 * hello and goes up by one per sealed message; a receiver takes only the next
 * one. K and the nonces are the token proof's (portable/mg_link_setup).
 * Vectors: protocols/test-vectors/mg-session-seal-v1.json (sealed_data: data
 * both ways interleaved with credentials and a credentials_update).
 *
 * ONE SEALED SESSION PER TOKEN PROOF. The keys depend only on K, the proof's
 * nonces and node_id, and the counters restart at every hello, so two sealed
 * sessions under one proof would use the same keystream (a sniffer on the
 * unencrypted link XORs them). Take the nonces with mgtp_take_nonces(),
 * which hands them out once and wipes them, keep them for that one session,
 * and refuse a second sealed hello with proof_required until a new proof
 * matches. Never keep or copy them past the session's end.
 *
 * Two ways to seal, same bytes:
 *   - in place: mgseal_seal() over a contiguous plaintext;
 *   - streaming, for a device without a plaintext buffer: mgseal_tag_stream()
 *     reads the plaintext in pieces once to make the tag, then the sender
 *     makes each fragment with mgseal_xor_at() (random access) as it goes.
 * Two ways to open:
 *   - in place (credentials_update): mgseal_open() checks the counter and the
 *     tag, in constant time, before it decrypts anything;
 *   - as fragments arrive, without a message buffer (data):
 *     mgseal_tag_begin() at the first fragment (the counter checked there),
 *     mgseal_tag_update() with each fragment's ciphertext, mgseal_tag_check()
 *     with the tag at the last; decrypt (mgseal_xor_at) only what a receiver
 *     keeps, and only once the tag matched.
 *
 * No allocation, no logging. Every function runs to completion on the
 * caller's stack: one HMAC (~0.5 KB with the builtin backend), plus ~0.2 KB
 * for mgseal_tag_stream(). The incremental tag's state (mgseal_tag_t, about
 * 110 bytes with the seal's own SHA-256) is the caller's, kept between calls.
 * Call it from one context (main loop or one task).
 */

#ifndef MGSEAL_STREAM
#define MGSEAL_STREAM 1 /* mgseal_tag_stream() and its SHA-256 */
#endif
/* 1: mgseal_tag_stream() hashes with the crypto backend's incremental SHA-256
 * (mglsc_sha256_init / _update / _final) instead of its own copy: about
 * 0.7-1.0 KB less code, the same bytes out. A board that links
 * portable/mg_link_setup's backend (builtin or PSA) sets it, so SHA-256 is
 * compiled once. 0 (the default): its own copy, for a port whose seal backend
 * offers only the two primitives (the ESP32's), or where the
 * backend's SHA-256 costs more than the copy (the Zephyr port, on PSA). */
#ifndef MGSEAL_EXT_SHA
#define MGSEAL_EXT_SHA 0
#endif

#define MGSEAL_KEY_LEN   32u
#define MGSEAL_NONCE_LEN 16u
#define MGSEAL_TAG_LEN   16u
#define MGSEAL_OVERHEAD  (4u + MGSEAL_TAG_LEN) /* counter + tag */
#define MGSEAL_DIR_DEVICE 1u /* device -> client: credentials */
#define MGSEAL_DIR_CLIENT 2u /* client -> device: credentials_update */

enum {
	MGSEAL_OK = 0,
	MGSEAL_E_CRYPTO = -1,  /* the backend (or the read callback) failed */
	MGSEAL_E_LEN = -2,     /* too short to be sealed, or a length out of range */
	MGSEAL_E_COUNTER = -3, /* not the counter the receiver expects */
	MGSEAL_E_TAG = -4,     /* the tag doesn't match: nothing was decrypted */
};

typedef struct mgseal_keys {
	uint8_t enc[MGSEAL_KEY_LEN];
	uint8_t mac[MGSEAL_KEY_LEN];
} mgseal_keys_t;

/* Derives both keys. node_id as in the proof: UTF-8, not NUL-terminated. On
 * failure the keys are wiped. */
int mgseal_keys(mgseal_keys_t *keys, const uint8_t k[MGSEAL_KEY_LEN],
		const uint8_t client_nonce[MGSEAL_NONCE_LEN],
		const uint8_t device_nonce[MGSEAL_NONCE_LEN], const char *node_id,
		size_t node_len);

/* In place: buf[4 .. 4 + len) holds the plaintext; afterwards buf[0 .. len +
 * MGSEAL_OVERHEAD) is the sealed message (counter in front, tag appended). */
int mgseal_seal(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, uint8_t *buf,
		size_t len);

/* In place: buf[0 .. sealed_len) is a sealed message. The counter must be
 * `expected` and the tag must match (checked before decrypting); then
 * buf[4 .. 4 + *len) is the plaintext. On failure nothing is decrypted
 * (*len = 0); wipe buf. */
int mgseal_open(const mgseal_keys_t *keys, uint8_t dir, uint32_t expected, uint8_t *buf,
		size_t sealed_len, size_t *len);

/* XORs the keystream for plaintext offsets [off, off + n) into data (any
 * offset: a fragment's ciphertext, or decrypting in pieces). */
int mgseal_xor_at(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, uint32_t off,
		  uint8_t *data, size_t n);

#if MGSEAL_STREAM
#if MGSEAL_EXT_SHA
#include "mgls_crypto.h"
typedef struct {
	mglsc_sha256_ctx_t s;
	int err; /* the backend's first failure */
} mgseal_sha_t;
#else
typedef struct {
	uint32_t h[8];
	uint64_t bytes;
	uint8_t block[64];
	size_t fill;
} mgseal_sha_t;
#endif

/* An incremental tag (HMAC-SHA256 with the mac key, its inner hash between
 * calls): for a receiver that checks a sealed message as its fragments arrive. */
typedef struct mgseal_tag {
	mgseal_sha_t sha;
} mgseal_tag_t;

/* Starts the tag of a message with this direction, counter and ciphertext
 * length (sealed length - MGSEAL_OVERHEAD). */
int mgseal_tag_begin(mgseal_tag_t *t, const mgseal_keys_t *keys, uint8_t dir, uint32_t counter,
		     uint32_t len);
/* Adds the next n ciphertext bytes (exactly len in all, in order). */
void mgseal_tag_update(mgseal_tag_t *t, const uint8_t *ct, size_t n);
/* MGSEAL_OK when tag matches (constant time), MGSEAL_E_TAG when it doesn't,
 * MGSEAL_E_CRYPTO on a backend failure. t is wiped either way. */
int mgseal_tag_check(mgseal_tag_t *t, const mgseal_keys_t *keys, const uint8_t tag[MGSEAL_TAG_LEN]);
/* Abandons a begun tag (begin called, check not), even after a failed begin: releases the backend's hash
 * operation (MGSEAL_EXT_SHA) and wipes t. */
void mgseal_tag_abort(mgseal_tag_t *t);

/* Fills buf with plaintext bytes [off, off + n), n <= 32. 0 on success. It
 * must return the same bytes when asked again (the sender reads each piece
 * twice: for the tag, then for the fragment). */
typedef int (*mgseal_read_fn)(void *ctx, uint32_t off, uint8_t *buf, size_t n);

/* The tag of a message whose plaintext (len bytes) comes from read(). */
int mgseal_tag_stream(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, uint32_t len,
		      mgseal_read_fn read, void *ctx, uint8_t tag[MGSEAL_TAG_LEN]);

/* A sealed credentials_saved's plaintext (mgcommands.h
 * mg_session_credentials_saved; vectors: sealed_credentials_saved): saved
 * (1 stored durably, 0 not), then the first MGSEAL_SAVED_HASH_LEN bytes of
 * SHA-256 over the update's UTF-8 refresh_token, all zero when not saved
 * (refresh may then be NULL). The device seals it under its next
 * device-to-client counter (MGSEAL_DIR_DEVICE, the counter its credentials
 * and data share) and frames it as credentials (flags first|last|sealed,
 * total_len MGSEAL_SAVED_SEALED), only after the commit. MGSEAL_OK, or
 * MGSEAL_E_CRYPTO (the backend's SHA-256 failed, MGSEAL_EXT_SHA): out is
 * then the not-saved plaintext, which the device sends instead. */
#define MGSEAL_SAVED_HASH_LEN 8u /* MG_SESSION_SAVED_HASH_LEN */
#define MGSEAL_SAVED_LEN      (1u + MGSEAL_SAVED_HASH_LEN)
#define MGSEAL_SAVED_SEALED   (MGSEAL_SAVED_LEN + MGSEAL_OVERHEAD) /* 29: its total_len */
int mgseal_saved_plaintext(uint8_t out[MGSEAL_SAVED_LEN], bool saved, const uint8_t *refresh,
			   size_t refresh_len);
#endif

void mgseal_wipe(void *p, size_t n);

/* A receiver's counter (two messages in flight): next, the next
 * counter, and hole, 0 or next minus the one counter a message refused at its
 * tag left open after a later counter was taken (data and credentials_update
 * reassemble separately, so two can be in flight). Shared by layer 2 and the
 * boards with their own session code, so every gadget keeps the same rule.
 *
 * Counters never wrap: once c = 0xFFFFFFFF is taken (next wraps to 0) a
 * receiver takes no other counter that is "next"; these helpers don't keep
 * that, so the caller does (layer 2: mgds_t.rx_top), and refuses c == next
 * from then on (a hole near the top still works: next - hole wraps back).
 *
 * mgseal_rx_ok(): may a first fragment carry counter c (the next one, or the
 * hole)? */
static inline bool mgseal_rx_ok(uint32_t next, uint8_t hole, uint32_t c)
{
	return c == next || (hole != 0u && c == next - hole);
}

/* Takes c (mgseal_rx_ok() said yes): used from here (an abort, a drop, a state
 * change), unless mgseal_rx_unuse() gives it back. The hole is accepted once. */
static inline void mgseal_rx_take(uint32_t *next, uint8_t *hole, uint32_t c)
{
	if (c == *next) {
		(*next)++;
		*hole = *hole == 0u || *hole == 255u ? 0u : (uint8_t)(*hole + 1u);
	} else {
		*hole = 0;
	}
}

/* A message that took c was refused at its tag: c isn't used. Given back when
 * nothing later took a counter, else left open once as the hole (one hole: a
 * newer one replaces it, the older then counts as used). */
static inline void mgseal_rx_unuse(uint32_t *next, uint8_t *hole, uint32_t c)
{
	if (*next == c + 1u) {
		*next = c;
		*hole = *hole > 1u ? (uint8_t)(*hole - 1u) : 0u;
	} else if (c < *next && *next - c <= 255u) {
		*hole = (uint8_t)(*next - c);
	}
}

#ifdef __cplusplus
}
#endif

#endif
