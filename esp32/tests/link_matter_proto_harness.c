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

// Host harness for components/esp32_matter_controller/esp32_matter_controller_proto.c: parameter
// checks, setup codes and the node list. Built and run by
// tests/test_link_matter.py.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp32_matter_controller_proto.h"

static cJSON *params(const char *json) {
    cJSON *p = cJSON_Parse(json);
    assert(p);
    return p;
}

static void test_results(void) {
    cJSON *ok = mc_ok(NULL);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(ok, "ok")));
    assert(cJSON_IsObject(cJSON_GetObjectItem(ok, "payload")));
    cJSON_Delete(ok);
    cJSON *err = mc_error("busy", "another %s is running", "operation");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(err, "ok")));
    cJSON *e = cJSON_GetObjectItem(err, "error");
    assert(strcmp(cJSON_GetObjectItem(e, "code")->valuestring, "busy") == 0);
    assert(strcmp(cJSON_GetObjectItem(e, "message")->valuestring, "another operation is running") == 0);
    cJSON_Delete(err);
}

static void test_params(void) {
    char err[96];
    uint64_t v;
    cJSON *p = params("{\"node_id\":104,\"endpoint\":65535,\"neg\":-1,\"half\":1.5,\"big\":4294967296,"
                      "\"s\":\"x\",\"yes\":true}");
    assert(mc_param_uint(p, "node_id", 1ULL << 53, true, 0, &v, err, sizeof(err)) && v == 104);
    assert(mc_param_uint(p, "endpoint", 0xFFFF, true, 0, &v, err, sizeof(err)) && v == 0xFFFF);
    assert(!mc_param_uint(p, "neg", 0xFFFF, true, 0, &v, err, sizeof(err)));
    assert(strstr(err, "neg must be an integer"));
    assert(!mc_param_uint(p, "half", 0xFFFF, true, 0, &v, err, sizeof(err)));
    assert(!mc_param_uint(p, "big", 0xFFFFFFFF, true, 0, &v, err, sizeof(err)));
    assert(!mc_param_uint(p, "s", 0xFFFF, true, 0, &v, err, sizeof(err)));
    assert(!mc_param_uint(p, "missing", 0xFFFF, true, 0, &v, err, sizeof(err)));
    assert(strcmp(err, "missing is required") == 0);
    assert(mc_param_uint(p, "missing", 0xFFFF, false, 300, &v, err, sizeof(err)) && v == 300);
    assert(mc_param_uint(NULL, "missing", 0xFFFF, false, 7, &v, err, sizeof(err)) && v == 7);
    assert(strcmp(mc_param_str(p, "s"), "x") == 0 && !mc_param_str(p, "node_id") && !mc_param_str(NULL, "s"));
    assert(mc_param_bool(p, "yes", false) && !mc_param_bool(p, "s", false) && mc_param_bool(p, "missing", true));
    cJSON_Delete(p);
}

static void test_codes(void) {
    char out[128];
    assert(mc_code_normalize("MT:Y.K9042C00KA0648G00", out, sizeof(out)) && strcmp(out, "MT:Y.K9042C00KA0648G00") == 0);
    // Manual codes as CHIP's parser takes them: digits only.
    assert(mc_code_normalize("34970112332", out, sizeof(out)) && strcmp(out, "34970112332") == 0);
    assert(mc_code_normalize("3497-011-2332", out, sizeof(out)) && strcmp(out, "34970112332") == 0);
    assert(mc_code_normalize("3497 011 2332", out, sizeof(out)) && strcmp(out, "34970112332") == 0);
    assert(mc_code_normalize("749701123365521327694", out, sizeof(out)) && strlen(out) == 21);
    assert(!mc_code_normalize(NULL, out, sizeof(out)) && !mc_code_normalize("", out, sizeof(out)));
    assert(!mc_code_normalize("MT:", out, sizeof(out)));
    assert(!mc_code_normalize("MT:y.k9042", out, sizeof(out)));     // base38 is upper case
    assert(!mc_code_normalize("1234567890", out, sizeof(out)));     // 10 digits
    assert(!mc_code_normalize("349701123321", out, sizeof(out)));   // 12 digits
    assert(!mc_code_normalize("3497011233a", out, sizeof(out)));
    assert(!mc_code_normalize("34970112332", out, 11));             // no room for the terminator
    assert(!mc_code_normalize("MT:Y.K9042C00KA0648G00", out, 10));
}

static void test_nodes(void) {
    cJSON *nodes = cJSON_CreateArray();
    assert(mc_nodes_put(nodes, 100, "lamp", 3));
    assert(mc_nodes_put(nodes, 101, "", 3));
    assert(mc_nodes_find(nodes, 101) == 1 && mc_nodes_find(nodes, 7) == -1);
    assert(!cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, 1), "label"));
    assert(mc_nodes_put(nodes, 100, "desk lamp", 3));  // replaced, moved to the end
    assert(cJSON_GetArraySize(nodes) == 2 && mc_nodes_find(nodes, 100) == 1);
    assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, 1), "label")->valuestring, "desk lamp") == 0);
    assert(mc_nodes_put(nodes, 102, "a", 3) && mc_nodes_put(nodes, 103, "b", 3));
    assert(cJSON_GetArraySize(nodes) == 3 && mc_nodes_find(nodes, 101) == -1);  // the oldest went
    assert(mc_nodes_drop(nodes, 102) && !mc_nodes_drop(nodes, 102));
    assert(cJSON_GetArraySize(nodes) == 2);
    cJSON_Delete(nodes);
}

int main(void) {
    test_results();
    test_params();
    test_codes();
    test_nodes();
    puts("esp32_matter_controller_proto: ok");
    return 0;
}
