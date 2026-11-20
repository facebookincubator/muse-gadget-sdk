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
#include <string.h>

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
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *avatar = cJSON_GetObjectItemCaseSensitive(result, "avatar");
    cJSON *exists = cJSON_GetObjectItemCaseSensitive(avatar, "image_exists");
    cJSON *path = cJSON_GetObjectItemCaseSensitive(avatar, "image_path");
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(avatar, "assets_revision");
    int status = -1;
    avatar_identity_t candidate = {0};
    if (cJSON_IsTrue(ok) && cJSON_IsObject(avatar) && cJSON_IsBool(exists)) {
        if (cJSON_IsFalse(exists)) status = 0;
        else if (cJSON_IsString(path) && path->valuestring[0] &&
                 strlen(path->valuestring) < sizeof(candidate.path) &&
                 (!revision || (cJSON_IsString(revision) && strlen(revision->valuestring) < sizeof(candidate.revision)))) {
            memcpy(candidate.path, path->valuestring, strlen(path->valuestring) + 1);
            if (revision) memcpy(candidate.revision, revision->valuestring, strlen(revision->valuestring) + 1);
            status = 1;
        }
    }
    cJSON_Delete(root);
    if (status >= 0) *out = candidate;
    return status;
}
