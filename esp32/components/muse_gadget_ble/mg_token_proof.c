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

#include "mg_token_proof.h"

#include <string.h>

#include "psa/crypto.h"

#define SALT "mg token proof v1"
#define DEVICE_LABEL "mg token proof v1 device"
#define CLIENT_LABEL "mg token proof v1 client"

void mg_token_proof_wipe(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--) {
        *b++ = 0;
    }
}

bool mg_token_proof_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

/* HMAC-SHA256 over n pieces (the sealed device session's, mg_seal_crypto.c). */
bool mg_hmac_sha256_v(const uint8_t *key, size_t klen, const uint8_t *const parts[], const size_t lens[], int n,
                      uint8_t out[32])
{
    if (n < 0 || psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    if (psa_import_key(&attr, key, klen, &id) != PSA_SUCCESS) {
        return false;
    }
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t len = 0;
    bool ok = psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS;
    for (int i = 0; ok && i < n; i++) {
        ok = !lens[i] || psa_mac_update(&op, parts[i], lens[i]) == PSA_SUCCESS;
    }
    ok = ok && psa_mac_sign_finish(&op, out, 32, &len) == PSA_SUCCESS && len == 32;
    if (!ok) {
        psa_mac_abort(&op);
    }
    psa_destroy_key(id);
    return ok;
}

/* HMAC-SHA256 over up to three pieces. */
bool mg_hmac_sha256_3(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen,
                  const void *c, size_t clen, uint8_t out[32])
{
    const uint8_t *const parts[3] = { a, b, c };
    const size_t lens[3] = { alen, blen, clen };
    return mg_hmac_sha256_v(key, klen, parts, lens, 3, out);
}

bool mg_token_proof_key(const char *access_token, const char *node_id, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    if (!access_token || !*access_token || !node_id) {
        return false;
    }
    /* RFC 5869 with one output block: PRK = HMAC(salt, IKM); K = HMAC(PRK, info || 0x01). */
    static const uint8_t one = 1;
    uint8_t prk[32];
    bool ok = mg_hmac_sha256_3((const uint8_t *)SALT, strlen(SALT), access_token, strlen(access_token), NULL, 0, NULL, 0, prk)
              && mg_hmac_sha256_3(prk, sizeof(prk), node_id, strlen(node_id), &one, 1, NULL, 0, k);
    mg_token_proof_wipe(prk, sizeof(prk));
    if (!ok) {
        mg_token_proof_wipe(k, MG_TOKEN_PROOF_KEY_LEN);
    }
    return ok;
}

bool mg_token_proof_mac(const uint8_t k[MG_TOKEN_PROOF_KEY_LEN], bool device,
                        const uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                        const uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN], uint8_t mac[MG_TOKEN_PROOF_MAC_LEN])
{
    const char *label = device ? DEVICE_LABEL : CLIENT_LABEL;
    return mg_hmac_sha256_3(k, MG_TOKEN_PROOF_KEY_LEN, label, strlen(label), client_nonce, MG_TOKEN_PROOF_NONCE_LEN,
                 device_nonce, MG_TOKEN_PROOF_NONCE_LEN, mac);
}
