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
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "cJSON.h"
}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define MUSE_DEVICE_FAMILY "gadget"
#define CONFIG_MUSE_DEVICE_MODEL_ID "test-model"
#define CONFIG_MUSE_DEVICE_MODEL_NAME "Test Model"
using noise_ctrl_session_generation_t = uint32_t;
struct pending_result {
    noise_ctrl_session_generation_t session_generation;
    char *request_id;
    cJSON *result;
};
static std::vector<pending_result> results;
static auto *s_result_q = &results;
static int xQueueSend(std::vector<pending_result> *q, pending_result *p, int wait) {
    assert(wait == 100); q->push_back(*p); return pdTRUE;
}
struct esp_app_desc_t { char version[32]; };
static const esp_app_desc_t desc = {"999.0.0"};
static const esp_app_desc_t *esp_app_get_description() { return &desc; }
static const char *s_node_id = "test-node", *s_display_name = "Test";
static char s_wifi_ssid[33] = "wifi", s_register_req_id[40];
static void copy_wifi_ssid(char *out, size_t cap) { snprintf(out, cap, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t cap) { snprintf(out, cap, "test-request"); }
@HEADER@
@FUNCTIONS@
@IMPL@
static int health_calls, guide_calls;
static void send_device_health(uint32_t, const char *) { health_calls++; }
static void send_device_guide(uint32_t, const char *) { guide_calls++; }
static void notify_status(const char *) {}
static cJSON *(*s_command_cb)(const char *, cJSON *, const char *, uint32_t) = nullptr;
@INVOKE@
static void invoke(const char *command) {
    char json[256];
    snprintf(json, sizeof(json), "{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":\"%s\"}", command);
    handle_invoke_request(json, strlen(json), 13);
}
int main() {
    char *registration = build_register_json();
    assert(registration && strlen(registration) < 8192);
    cJSON *root = cJSON_ParseWithLength(registration, strlen(registration));
    cJSON *commands = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "params"), "commands_v2");
    assert(cJSON_GetObjectItem(commands, "device.health"));
    assert(!cJSON_GetObjectItem(commands, "device.ota"));
#ifdef PRODUCT_CONTROL_EXTENSION_HEADER
    assert(cJSON_GetObjectItem(commands, "product.ping"));
    int matches = 0;
    for (cJSON *c = commands->child; c; c = c->next) if (!strcmp(c->string, "product.ping")) matches++;
    assert(matches == 1);
#else
    assert(!cJSON_GetObjectItem(commands, "product.ping"));
#endif
    cJSON_Delete(root); free(registration);
    invoke("product.ping");
    assert(results.size() == 1 && results[0].session_generation == 13);
    assert(!strcmp(results[0].request_id, "r1"));
#ifdef PRODUCT_CONTROL_EXTENSION_HEADER
    assert(cJSON_IsTrue(cJSON_GetObjectItem(results[0].result, "ok")));
#else
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(results[0].result, "ok")));
#endif
    invoke("device.health"); invoke("device.guide");
    assert(health_calls == 1 && guide_calls == 1 && results.size() == 1);
    invoke("other.command");
    assert(results.size() == 2 && !cJSON_IsTrue(cJSON_GetObjectItem(results[1].result, "ok")));
    for (auto &r : results) { free(r.request_id); cJSON_Delete(r.result); }
    puts("product hooks passed");
}
