/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "avatar_identity.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The Gadget API names each avatar file by a URL under this prefix. Anything
 * else in a reply is refused rather than requested. */
bool avatar_identity_url_ok(const char *url, size_t cap) {
    static const char prefix[] = AVATAR_IDENTITY_FILE_PREFIX;
    size_t n = url ? strlen(url) : 0;
    if (n <= sizeof(prefix) - 1 || n >= cap || strncmp(url, prefix, sizeof(prefix) - 1)) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= ' ' || c >= 0x7f || c == '"' || c == '\\' || c == '#') return false;
    }
    return true;
}

/* The light variant when it is a valid URL, else the dark one. */
const char *avatar_identity_pick_url(const cJSON *urls, size_t cap) {
    static const char *const appearances[] = {"light", "dark"};
    for (size_t i = 0; i < sizeof(appearances) / sizeof(appearances[0]); i++) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(urls, appearances[i]);
        if (cJSON_IsString(url) && avatar_identity_url_ok(url->valuestring, cap)) return url->valuestring;
    }
    return NULL;
}

/* updated_ms as decimal digits, without floating-point printf. */
static void revision_digits(const cJSON *updated, char *out, size_t cap) {
    double ms = updated ? updated->valuedouble : -1;
    if (!(ms >= 0 && ms < 9007199254740992.0)) return;  /* exact integers only */
    uint64_t v = (uint64_t)ms;
    char digits[24];
    size_t n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < sizeof(digits));
    if (n >= cap) return;
    for (size_t i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    out[n] = 0;
}

int avatar_identity_parse(const char *json, size_t len, avatar_identity_t *out) {
    if (!out || !json || !len || len > 8192 || memchr(json, 0, len)) return -1;
    unsigned nodes = 0, depth = 0;
    bool quoted = false, escape = false;
    for (size_t i = 0; i < len; i++) {
        char c = json[i];
        if (quoted) {
            if (escape) { escape = false; continue; }
            if (c == '\\') {
                if (i + 5 < len && !memcmp(json + i, "\\u0000", 6)) return -1;
                escape = true;
            } else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 12 || ++nodes > 256) return -1; }
        else if (c == '}' || c == ']') { if (!depth) return -1; depth--; }
        else if ((c == ',' || c == ':') && ++nodes > 256) return -1;
    }
    if (quoted || depth) return -1;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    while (end && end < json + len && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) end++;
    if (!root || end != json + len) { cJSON_Delete(root); return -1; }
    /* GET /api/gadget/identity: {"ok":true,"result":{..., "updated_ms":N,
     * "avatar":{"image":{"light":URL,"dark":URL}, ...}}}. */
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *avatar = cJSON_GetObjectItemCaseSensitive(result, "avatar");
    cJSON *image = cJSON_GetObjectItemCaseSensitive(avatar, "image");
    cJSON *updated = cJSON_GetObjectItemCaseSensitive(result, "updated_ms");
    int status = -1;
    avatar_identity_t candidate = {0};
    if (cJSON_IsTrue(ok) && cJSON_IsObject(result) && (!updated || cJSON_IsNumber(updated))) {
        /* No avatar, or no still yet: it can be still being made, which does
         * not prove the person chose the built-in character. */
        if (!avatar || cJSON_IsNull(avatar) || (cJSON_IsObject(avatar) && (!image || cJSON_IsNull(image)))) status = 0;
        else if (cJSON_IsObject(image)) {
            const char *url = avatar_identity_pick_url(image, sizeof(candidate.path));
            if (url) {
                memcpy(candidate.path, url, strlen(url) + 1);
                /* Changes whenever the identity or any avatar file does. */
                revision_digits(updated, candidate.revision, sizeof(candidate.revision));
                status = 1;
            }
        }
    }
    cJSON_Delete(root);
    if (status >= 0) *out = candidate;
    return status;
}
