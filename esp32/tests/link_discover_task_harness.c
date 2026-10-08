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

// Host harness for app.c's discover_task: device.discover always gets an
// answer, even when the scan or the reply runs out of memory, and nothing
// leaks. The runner extracts the production task into discover_task.inc.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW ESP_LOGI

typedef uint64_t noise_ctrl_session_generation_t;
static const char *TAG = "test";

// cJSON allocations: fail the one numbered fail_at (0 is the first after the
// scan), or every one from fail_from on. live counts what is still allocated.
static int live, allocs, fail_at = -1, fail_from = -1;
static void *test_malloc(size_t size) {
    int n = allocs++;
    if (n == fail_at || (fail_from >= 0 && n >= fail_from)) return NULL;
    live++;
    return malloc(size);
}
static void test_free(void *p) {
    if (p) live--;
    free(p);
}

static cJSON *scan;
static cJSON *net_discovery_run(cJSON *params) {
    (void)params;
    return scan;
}

static int sent;
static char reply[256];
static void noise_ctrl_send_command_result(noise_ctrl_session_generation_t generation,
                                           const char *request_id, cJSON *result) {
    assert(generation == 7 && strcmp(request_id, "req-1") == 0);
    sent++;
    reply[0] = '\0';
    if (!result) return;
    fail_at = fail_from = -1;
    char *json = cJSON_PrintUnformatted(result);
    assert(json && strlen(json) < sizeof(reply));
    strcpy(reply, json);
    cJSON_free(json);
    cJSON_Delete(result);
}

static void stack_monitor_record(const char *name) { (void)name; }
static void vTaskDelete(void *task) { (void)task; }

#include "discover_task.inc"

static const char *OK_REPLY = "{\"ok\":true,\"payload\":{\"ok\":true,\"observations\":[]}}";
static const char *OOM_REPLY =
    "{\"error\":{\"code\":\"out_of_memory\","
    "\"message\":\"not enough memory for the discovery results\"},\"ok\":false}";

static void run(bool scanned, int at, int from) {
    fail_at = fail_from = -1;
    discover_task_args_t *args = calloc(1, sizeof(*args));
    args->session_generation = 7;
    strcpy(args->request_id, "req-1");
    args->params = cJSON_CreateObject();
    scan = scanned ? cJSON_Parse("{\"ok\":true,\"observations\":[]}") : NULL;
    allocs = 0;
    fail_at = at;
    fail_from = from;
    sent = 0;
    discover_task(args);
    assert(sent == 1);
    assert(live == 0);
}

int main(void) {
    cJSON_Hooks hooks = {test_malloc, test_free};
    cJSON_InitHooks(&hooks);

    run(true, -1, -1);
    assert(strcmp(reply, OK_REPLY) == 0);

    // net_discovery_run returns NULL only when it ran out of memory.
    run(false, -1, -1);
    assert(strcmp(reply, OOM_REPLY) == 0);

    // Whichever allocation for the reply fails, the Muse hears back.
    int failures = 0;
    for (int at = 0; at < 16; at++) {
        run(true, at, -1);
        if (strcmp(reply, OK_REPLY) != 0) {
            assert(strcmp(reply, OOM_REPLY) == 0);
            failures++;
        }
    }
    assert(failures > 0);

    // With no memory left at all there is nothing to send, and nothing leaks.
    run(true, -1, 0);
    assert(reply[0] == '\0');
    run(false, -1, 0);
    assert(reply[0] == '\0');

    printf("discover_task: ok\n");
    return 0;
}
