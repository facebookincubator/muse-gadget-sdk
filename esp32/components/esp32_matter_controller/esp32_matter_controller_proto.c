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

#include "esp32_matter_controller_proto.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

cJSON *mc_ok(cJSON *payload) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload ? payload : cJSON_CreateObject());
    return result;
}

cJSON *mc_error(const char *code, const char *fmt, ...) {
    char message[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

bool mc_param_uint(const cJSON *params, const char *key, uint64_t max, bool required, uint64_t def,
                   uint64_t *out, char *err, size_t err_len) {
    const cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    if (!item) {
        if (required) {
            snprintf(err, err_len, "%s is required", key);
            return false;
        }
        *out = def;
        return true;
    }
    // JSON numbers are doubles: integers are exact up to 2^53, which covers
    // every Matter ID (node IDs are given out from 100 up).
    double v = item->valuedouble;
    if (!cJSON_IsNumber(item) || v < 0 || v > (double)max || v != (double)(uint64_t)v) {
        snprintf(err, err_len, "%s must be an integer from 0 to %llu", key, (unsigned long long)max);
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

const char *mc_param_str(const cJSON *params, const char *key) {
    const cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

bool mc_param_bool(const cJSON *params, const char *key, bool def) {
    const cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : def;
}

bool mc_code_normalize(const char *code, char *out, size_t out_len) {
    if (!code || !out || out_len == 0) return false;
    size_t len = strlen(code);
    if (strncmp(code, "MT:", 3) == 0) {
        // Base38: digits, capital letters, '-' and '.'.
        if (len < 4 || len >= out_len) return false;
        for (size_t i = 3; i < len; i++) {
            char c = code[i];
            if (!(isdigit((unsigned char)c) || (c >= 'A' && c <= 'Z') || c == '-' || c == '.')) return false;
        }
        memcpy(out, code, len + 1);
        return true;
    }
    size_t digits = 0;
    for (size_t i = 0; i < len; i++) {
        if (isdigit((unsigned char)code[i])) {
            if (digits + 1 >= out_len) return false;
            out[digits++] = code[i];
        } else if (code[i] != '-' && code[i] != ' ') {
            return false;
        }
    }
    out[digits] = '\0';
    return digits == 11 || digits == 21;
}

int mc_nodes_find(const cJSON *nodes, uint64_t node_id) {
    int n = cJSON_GetArraySize(nodes);
    for (int i = 0; i < n; i++) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(nodes, i), "node_id");
        if (cJSON_IsNumber(id) && (uint64_t)id->valuedouble == node_id) return i;
    }
    return -1;
}

bool mc_nodes_put(cJSON *nodes, uint64_t node_id, const char *label, int max) {
    int i = mc_nodes_find(nodes, node_id);
    if (i >= 0) cJSON_DeleteItemFromArray(nodes, i);
    while (max > 0 && cJSON_GetArraySize(nodes) >= max) cJSON_DeleteItemFromArray(nodes, 0);
    cJSON *node = cJSON_CreateObject();
    if (!node) return false;
    cJSON_AddNumberToObject(node, "node_id", (double)node_id);
    if (label && *label) cJSON_AddStringToObject(node, "label", label);
    return cJSON_AddItemToArray(nodes, node);
}

bool mc_nodes_drop(cJSON *nodes, uint64_t node_id) {
    int i = mc_nodes_find(nodes, node_id);
    if (i < 0) return false;
    cJSON_DeleteItemFromArray(nodes, i);
    return true;
}
