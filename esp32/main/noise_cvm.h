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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Classification of the Noise handshake msg2 payload, split out so host tests
// can exercise it without the ESP-IDF/TLS transport.
//
// Two msg2 payload shapes exist on /v1/noise:
//
// - Legacy owner: the raw SEV-SNP attestation report bytes (possibly empty).
//   No structure the device may rely on; the connection proceeds exactly as
//   it always has.
// - CVM RV-auth: a protobuf NoiseHandshakeMessage2
//   (hatch.noise.handshake) carrying attestation(1), state(2), vm_nonce(3)
//   and optionally vm_proof(4). Ingress uses this shape only when the VM is
//   a confidential VM whose Noise plane requires the RV-key exchange in
//   msg3 (notary_token + kr.fresh / kr.proof). This device holds no RV key
//   and cannot answer that exchange, so the handshake can never complete:
//   when the vault reports UNPROVISIONED (no RV key in its ramfs — the
//   volume is locked or was never enrolled) the only fix is the owner
//   unlocking the CVM from their Muse app, and the session loop surfaces
//   that instead of retrying a handshake that cannot succeed.
//
// A payload is classified as CVM RV-auth only when it parses cleanly as
// that message AND carries a recognised state AND a 32-byte vm_nonce.
// Anything else — empty, legacy attestation bytes, or a malformed buffer —
// is NOT_CVM, so a parse hiccup can never block a connection that works
// today.

typedef enum {
    NOISE_CVM_MSG2_NOT_CVM = 0,
    NOISE_CVM_MSG2_UNPROVISIONED,
    NOISE_CVM_MSG2_PROVISIONED,
} noise_cvm_msg2_t;

// Read one protobuf varint. Returns false (and writes nothing) when the
// buffer ends mid-varint or the varint overflows 64 bits.
static inline bool noise_cvm_read_varint(const uint8_t *buf, size_t len,
                                         size_t *pos, uint64_t *out) {
    uint64_t value = 0;
    unsigned shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t byte = buf[(*pos)++];
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

// Skip one field body of the given wire type. Supports the wire types the
// handshake messages use (0 varint, 1 fixed64, 2 length-delimited, 5
// fixed32); group wire types 3/4 and the reserved 6/7 are malformed here.
static inline bool noise_cvm_skip_field(const uint8_t *buf, size_t len,
                                        size_t *pos, unsigned wire_type,
                                        uint64_t *varint_out) {
    switch (wire_type) {
        case 0: {
            uint64_t value = 0;
            if (!noise_cvm_read_varint(buf, len, pos, &value)) return false;
            if (varint_out) *varint_out = value;
            return true;
        }
        case 1:
            if (len - *pos < 8) return false;
            *pos += 8;
            return true;
        case 2: {
            uint64_t size = 0;
            if (!noise_cvm_read_varint(buf, len, pos, &size)) return false;
            if (size > len - *pos) return false;
            *pos += (size_t)size;
            return true;
        }
        case 5:
            if (len - *pos < 4) return false;
            *pos += 4;
            return true;
        default:
            return false;
    }
}

static inline noise_cvm_msg2_t noise_cvm_classify_msg2(const uint8_t *payload,
                                                         size_t len) {
    if (!payload) return NOISE_CVM_MSG2_NOT_CVM;

    uint64_t state = 0;
    bool have_state = false;
    bool have_vm_nonce = false;
    size_t pos = 0;
    while (pos < len) {
        uint64_t key = 0;
        if (!noise_cvm_read_varint(payload, len, &pos, &key)) {
            return NOISE_CVM_MSG2_NOT_CVM;
        }
        uint64_t field = key >> 3;
        unsigned wire_type = (unsigned)(key & 0x7u);
        if (field == 0) return NOISE_CVM_MSG2_NOT_CVM;

        // Field 3 (vm_nonce) is the one length-delimited field whose size
        // the classification depends on, so note it before skipping.
        bool is_vm_nonce = field == 3 && wire_type == 2;
        size_t body_start = pos;
        uint64_t varint = 0;
        if (!noise_cvm_skip_field(payload, len, &pos, wire_type,
                                  wire_type == 0 ? &varint : NULL)) {
            return NOISE_CVM_MSG2_NOT_CVM;
        }
        if (field == 2 && wire_type == 0) {
            state = varint;
            have_state = true;
        } else if (is_vm_nonce) {
            // Recompute the body length from the length prefix.
            size_t p = body_start;
            uint64_t size = 0;
            if (!noise_cvm_read_varint(payload, len, &p, &size)) {
                return NOISE_CVM_MSG2_NOT_CVM;
            }
            if (size == 32) have_vm_nonce = true;
        }
    }

    if (!have_state || !have_vm_nonce) return NOISE_CVM_MSG2_NOT_CVM;
    if (state == 1) return NOISE_CVM_MSG2_UNPROVISIONED;  // UNPROVISIONED
    if (state == 2) return NOISE_CVM_MSG2_PROVISIONED;    // PROVISIONED
    return NOISE_CVM_MSG2_NOT_CVM;                        // UNKNOWN
}

// True when the msg2 says the VM is a CVM whose RV is not open. The user
// must unlock it from the Muse app; this device cannot.
static inline bool noise_cvm_unlock_required(noise_cvm_msg2_t msg2) {
    return msg2 == NOISE_CVM_MSG2_UNPROVISIONED;
}
