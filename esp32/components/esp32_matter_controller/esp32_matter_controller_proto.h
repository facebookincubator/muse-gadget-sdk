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

// Parameter checks, setup codes and the node list: the parts of esp32_matter_controller
// that need no Matter stack, so the host tests can run them
// (tests/test_link_matter.py).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// A setup code (QR text or manual code) and a device's label, with their NULs.
#define MC_CODE_MAX 128
#define MC_LABEL_MAX 48

// {"ok":true,"payload":payload} (an empty payload when NULL; takes ownership).
cJSON *mc_ok(cJSON *payload);
// {"ok":false,"error":{"code":code,"message":...}}. Only the message reaches
// the Muse, so it says what went wrong on its own.
cJSON *mc_error(const char *code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// An integer parameter in [0, max]. Absent: *out = def, or an error when
// required. On error returns false and writes a message to err.
bool mc_param_uint(const cJSON *params, const char *key, uint64_t max, bool required, uint64_t def,
                   uint64_t *out, char *err, size_t err_len);
// A string parameter, or NULL when absent or not a string.
const char *mc_param_str(const cJSON *params, const char *key);
bool mc_param_bool(const cJSON *params, const char *key, bool def);

// A Matter setup code: the QR text (MT:...), or an 11- or 21-digit manual code
// with dashes and spaces allowed. Writes it to out as CHIP parses it (a manual
// code's digits only); false when it isn't shaped like one. Check digits are
// left to CHIP's parser.
bool mc_code_normalize(const char *code, char *out, size_t out_len);

// The node list: a JSON array of {"node_id", "label"}.
int mc_nodes_find(const cJSON *nodes, uint64_t node_id);
// Adds or replaces node_id; drops the oldest entries beyond max (callers
// refuse a new device when the list is full).
bool mc_nodes_put(cJSON *nodes, uint64_t node_id, const char *label, int max);
bool mc_nodes_drop(cJSON *nodes, uint64_t node_id);

#ifdef __cplusplus
}
#endif
