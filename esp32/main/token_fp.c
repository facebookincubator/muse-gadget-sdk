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

const char *token_fp_text(const char *secret, char out[TOKEN_FP_TEXT_LEN]) {
    if (!secret || !secret[0]) {
        snprintf(out, TOKEN_FP_TEXT_LEN, "none");
        return out;
    }
    size_t len = strlen(secret);
    uint8_t hash[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md && mbedtls_md(md, (const unsigned char *)secret, len, hash) == 0) {
        snprintf(out, TOKEN_FP_TEXT_LEN, "%u chars, sha256 %02x%02x%02x%02x", (unsigned)len, hash[0],
                 hash[1], hash[2], hash[3]);
    } else {
        snprintf(out, TOKEN_FP_TEXT_LEN, "%u chars", (unsigned)len);
    }
    memset(hash, 0, sizeof(hash));
    return out;
}
