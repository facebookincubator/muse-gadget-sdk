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
 * The two primitives the vendored seal (portable/mg_device_session,
 * mg_session_seal.c, used unchanged) takes from the portable setup core's
 * crypto backend: mglsc_hmac_sha256() and mglsc_ct_equal(), on this SDK's
 * token proof crypto (mg_token_proof.c: PSA Crypto on the device, the host
 * tests' reference on the host). The rest of portable/mg_link_setup isn't
 * built here: this SDK has its own Link setup and token proof.
 */

#include "mgls_crypto.h"

#include "mg_token_proof.h"

int mglsc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[], const size_t lens[], int n,
                      uint8_t out[MGLSC_HASH_LEN])
{
    return mg_hmac_sha256_v(key, key_len, parts, lens, n, out) ? 0 : -1;
}

bool mglsc_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    return mg_token_proof_equal(a, b, len);
}
