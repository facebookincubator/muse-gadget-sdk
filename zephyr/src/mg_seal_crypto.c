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

/* The primitives the vendored seal (portable/mg_device_session,
 * mg_session_seal.c) takes from the portable setup core's crypto backend,
 * mglsc_hmac_sha256(), mglsc_ct_equal() and, for MGSEAL_EXT_SHA, the
 * incremental mglsc_sha256_init / _update / _final (off here: PSA's
 * multipart hash would cost more than the seal's own SHA-256, which
 * --gc-sections then drops these for), on this SDK's PSA Crypto
 * (mg_psa.c; mgls_crypto.h's MGLS_CRYPTO_PSA context). The rest of
 * portable/mg_link_setup isn't built here: this app has its own Link setup
 * and token proof. */

#include "mg_psa.h"
#include "mgls_crypto.h"

int mglsc_init(int (*rng)(void *ctx, uint8_t *out, size_t len), void *ctx)
{
	/* PSA brings its own RNG: only its init (idempotent). */
	(void)rng;
	(void)ctx;
	return mg_psa_init() ? -1 : 0;
}

int mglsc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		      const size_t lens[], int n, uint8_t out[MGLSC_HASH_LEN])
{
	/* psa_crypto_init() is idempotent; main() has normally run it. */
	if (mg_psa_init()) {
		return -1;
	}
	return mg_psa_hmac_sha256(key, key_len, parts, lens, n, out) ? -1 : 0;
}

bool mglsc_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	return mg_psa_ct_equal(a, b, len);
}

int mglsc_sha256_init(mglsc_sha256_ctx_t *ctx)
{
	ctx->op = psa_hash_operation_init();
	if (mg_psa_init()) {
		return -1;
	}
	return psa_hash_setup(&ctx->op, PSA_ALG_SHA_256) == PSA_SUCCESS ? 0 : -1;
}

int mglsc_sha256_update(mglsc_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
	return psa_hash_update(&ctx->op, data, len) == PSA_SUCCESS ? 0 : -1;
}

int mglsc_sha256_final(mglsc_sha256_ctx_t *ctx, uint8_t out[MGLSC_HASH_LEN])
{
	size_t n = 0;

	if (psa_hash_finish(&ctx->op, out, MGLSC_HASH_LEN, &n) == PSA_SUCCESS &&
	    n == MGLSC_HASH_LEN) {
		return 0;
	}
	/* A failed finish leaves the operation to release. */
	(void)psa_hash_abort(&ctx->op);
	mg_psa_wipe(out, MGLSC_HASH_LEN);
	return -1;
}
