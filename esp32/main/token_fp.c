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

#include "token_fp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"

bool token_fp_parts(const char *secret, unsigned *len, char hex[TOKEN_FP_HEX_LEN]) {
    *len = 0;
    hex[0] = 0;
    if (!secret || !secret[0]) return false;
    *len = (unsigned)strlen(secret);
    uint8_t hash[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md && mbedtls_md(md, (const unsigned char *)secret, *len, hash) == 0) {
        snprintf(hex, TOKEN_FP_HEX_LEN, "%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3]);
    }
    memset(hash, 0, sizeof(hash));
    return true;
}

const char *token_fp_text(const char *secret, char out[TOKEN_FP_TEXT_LEN]) {
    unsigned len;
    char hex[TOKEN_FP_HEX_LEN];
    if (!token_fp_parts(secret, &len, hex)) {
        snprintf(out, TOKEN_FP_TEXT_LEN, "none");
    } else if (hex[0]) {
        snprintf(out, TOKEN_FP_TEXT_LEN, "%u chars, sha256 %s", len, hex);
    } else {
        snprintf(out, TOKEN_FP_TEXT_LEN, "%u chars", len);
    }
    return out;
}
