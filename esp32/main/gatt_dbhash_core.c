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
#include "gatt_dbhash_core.h"

#include <string.h>

#include "psa/crypto.h"

static bool hashed_with_value(uint16_t type)
{
    return (type >= 0x2800 && type <= 0x2803) || type == 0x2900;
}

static bool hashed_without_value(uint16_t type)
{
    return type >= 0x2901 && type <= 0x2905;
}

size_t gatt_dbhash_serialize(const gatt_dbhash_attr_t *attrs, size_t n, uint8_t *out, size_t cap)
{
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        const gatt_dbhash_attr_t *a = &attrs[i];
        bool with = hashed_with_value(a->type);
        if (!with && !hashed_without_value(a->type)) {
            continue;
        }
        size_t need = 4 + (with ? a->len : 0);
        if (at + need > cap || a->len > GATT_DBHASH_VALUE_MAX) {
            return 0;
        }
        out[at++] = (uint8_t)a->handle;
        out[at++] = (uint8_t)(a->handle >> 8);
        out[at++] = (uint8_t)a->type;
        out[at++] = (uint8_t)(a->type >> 8);
        if (with) {
            memcpy(out + at, a->value, a->len);
            at += a->len;
        }
    }
    return at;
}

bool gatt_dbhash_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t out[16])
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
    psa_key_id_t id = 0;
    if (psa_import_key(&attr, key, 16, &id) != PSA_SUCCESS) {
        return false;
    }
    uint8_t mac[16];
    size_t got = 0;
    bool ok = psa_mac_compute(id, PSA_ALG_CMAC, msg, len, mac, sizeof(mac), &got) == PSA_SUCCESS && got == 16;
    psa_destroy_key(id);
    if (ok) {
        for (int i = 0; i < 16; i++) {
            out[i] = mac[15 - i];   /* the CMAC's most significant octet first -> the characteristic's LSO first */
        }
    }
    return ok;
}
