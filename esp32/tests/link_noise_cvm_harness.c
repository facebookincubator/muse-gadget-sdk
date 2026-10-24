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

#include <assert.h>
#include <string.h>

#include "noise_cvm.h"

// Append one length-delimited protobuf field (fields 1, 3, 4 of
// NoiseHandshakeMessage2) holding `size` filler bytes.
static size_t put_bytes_field(uint8_t *out, unsigned field, size_t size) {
    size_t n = 0;
    out[n++] = (uint8_t)((field << 3) | 2u);
    out[n++] = (uint8_t)size;  // all sizes used here are < 128
    memset(out + n, 0xAB, size);
    n += size;
    return n;
}

// Append the state field (field 2, varint).
static size_t put_state_field(uint8_t *out, uint64_t state) {
    out[0] = (uint8_t)((2u << 3) | 0u);
    out[1] = (uint8_t)state;
    return 2;
}

int main(void) {
    uint8_t buf[256];
    size_t n = 0;

    // No payload (non-CVM VMs send an empty legacy payload): not a CVM.
    assert(noise_cvm_classify_msg2(NULL, 0) == NOISE_CVM_MSG2_NOT_CVM);
    assert(noise_cvm_classify_msg2(buf, 0) == NOISE_CVM_MSG2_NOT_CVM);
    assert(!noise_cvm_unlock_required(NOISE_CVM_MSG2_NOT_CVM));

    // A legacy attestation blob is not protobuf NoiseHandshakeMessage2:
    // 0xFF is not a valid field key sequence that yields state + nonce.
    memset(buf, 0xFF, 64);
    assert(noise_cvm_classify_msg2(buf, 64) == NOISE_CVM_MSG2_NOT_CVM);

    // CVM RV-auth, unprovisioned: attestation(1) || state=1(2) ||
    // vm_nonce(3, 32 bytes). This is the locked-CVM answer.
    n = 0;
    n += put_bytes_field(buf + n, 1, 16);
    n += put_state_field(buf + n, 1);
    n += put_bytes_field(buf + n, 3, 32);
    assert(noise_cvm_classify_msg2(buf, n) == NOISE_CVM_MSG2_UNPROVISIONED);
    assert(noise_cvm_unlock_required(NOISE_CVM_MSG2_UNPROVISIONED));

    // CVM RV-auth, provisioned: same plus vm_proof(4, 32 bytes). The RV
    // is open server-side, so "unlock" is not the user's action here.
    n = 0;
    n += put_bytes_field(buf + n, 1, 16);
    n += put_state_field(buf + n, 2);
    n += put_bytes_field(buf + n, 3, 32);
    n += put_bytes_field(buf + n, 4, 32);
    assert(noise_cvm_classify_msg2(buf, n) == NOISE_CVM_MSG2_PROVISIONED);
    assert(!noise_cvm_unlock_required(NOISE_CVM_MSG2_PROVISIONED));

    // State present but vm_nonce missing/short: not the CVM shape.
    n = 0;
    n += put_bytes_field(buf + n, 1, 16);
    n += put_state_field(buf + n, 1);
    assert(noise_cvm_classify_msg2(buf, n) == NOISE_CVM_MSG2_NOT_CVM);
    n = 0;
    n += put_state_field(buf + n, 1);
    n += put_bytes_field(buf + n, 3, 16);
    assert(noise_cvm_classify_msg2(buf, n) == NOISE_CVM_MSG2_NOT_CVM);

    // UNKNOWN state is not actionable.
    n = 0;
    n += put_state_field(buf + n, 0);
    n += put_bytes_field(buf + n, 3, 32);
    assert(noise_cvm_classify_msg2(buf, n) == NOISE_CVM_MSG2_NOT_CVM);

    // Truncated / malformed payloads fall back to not-a-CVM, never to a
    // false locked verdict.
    n = 0;
    n += put_state_field(buf + n, 1);
    n += put_bytes_field(buf + n, 3, 32);
    assert(noise_cvm_classify_msg2(buf, n - 1) == NOISE_CVM_MSG2_NOT_CVM);
    assert(noise_cvm_classify_msg2(buf, 1) == NOISE_CVM_MSG2_NOT_CVM);
    buf[0] = 0x07;  // field 0 is invalid protobuf
    assert(noise_cvm_classify_msg2(buf, n) == NOISE_CVM_MSG2_NOT_CVM);

    return 0;
}
