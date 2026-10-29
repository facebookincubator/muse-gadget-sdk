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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "ota.h"
#include "esp_app_desc.h"

static esp_app_desc_t running = { .version = "999.0.0" };
static int callbacks;
static ota_result_t last_result;
static char last_detail[96];

const esp_app_desc_t *esp_app_get_description(void) { return &running; }

static void on_result(const ota_event_t *event, void *user) {
    assert(user == &callbacks);
    callbacks++;
    last_result = event->result;
    snprintf(last_detail, sizeof(last_detail), "%s", event->detail);
    if (event->running_version) {
        assert(strcmp(event->running_version, running.version) == 0);
    }
}

#define VIBE_MODE_BUILD (CONFIG_MUSE_BOARD_ESP_MOSAICO && CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY)

#if CONFIG_HOMEHUB_OTA_ENABLED || VIBE_MODE_BUILD
#include "esp_ota_ops.h"
static const esp_partition_t ota_0 = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
    .address = 0x20000, .label = "ota_0" };
static const esp_partition_t *running_part = &ota_0;
const esp_partition_t *esp_ota_get_running_partition(void) { return running_part; }
#endif

#if VIBE_MODE_BUILD
/* The ESP-Mosaico's retained layout (partitions_mosaico.csv): Vibe Mode in
 * `factory`, its records in `sysmeta`. Each piece can go missing. */
static const esp_partition_t factory_part = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY,
    .address = 0x10000, .label = "factory" };
static const esp_partition_t sysmeta_part = {
    .type = ESP_PARTITION_TYPE_DATA, .subtype = ESP_PARTITION_SUBTYPE_DATA_NVS,
    .address = 0xc000, .label = "sysmeta" };
static bool has_factory = true, has_sysmeta = true, factory_app_valid = true;
static esp_ota_img_states_t running_state = ESP_OTA_IMG_PENDING_VERIFY;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label) {
    if (type == ESP_PARTITION_TYPE_APP && subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY && !label) {
        return has_factory ? &factory_part : NULL;
    }
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS);
    assert(label && strcmp(label, "sysmeta") == 0);
    return has_sysmeta ? &sysmeta_part : NULL;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,
                                           esp_app_desc_t *app_desc) {
    assert(partition == &factory_part);
    memset(app_desc, 0, sizeof(*app_desc));
    return factory_app_valid ? ESP_OK : -1;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                      esp_ota_img_states_t *state) {
    assert(partition == running_part);
    *state = running_state;
    return ESP_OK;
}
#endif

#if CONFIG_HOMEHUB_OTA_ENABLED
#include "esp_https_ota.h"
#include "freertos/task.h"
#include "stack_monitor.h"

/* NVS: just the update record. */
static char record[128];
static bool record_set_fails;
bool config_get_str(const char *key, char *out, size_t cap) {
    assert(strcmp(key, "ota_verify") == 0);
    snprintf(out, cap, "%s", record);
    return record[0] != '\0';
}
bool config_set_str(const char *key, const char *value) {
    assert(strcmp(key, "ota_verify") == 0);
    if (record_set_fails) return false;
    snprintf(record, sizeof(record), "%s", value);
    return true;
}
bool config_erase_key(const char *key) {
    assert(strcmp(key, "ota_verify") == 0);
    record[0] = '\0';
    return true;
}
static const esp_partition_t ota_1 = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
    .address = 0x620000, .label = "ota_1" };
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from) {
    assert(start_from == NULL);
    return running_part == &ota_0 ? &ota_1 : &ota_0;
}

void *esp_crt_bundle_attach;
static esp_app_desc_t incoming = { .version = "1.0.1" };
static TaskFunction_t pending_task;
static void *pending_arg;
static int starts, downloads, finishes, aborts, reboots;
static esp_err_t finish_result;

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       unsigned stack_depth, void *params,
                       unsigned priority, TaskHandle_t *out_handle) {
    (void)name; (void)stack_depth; (void)priority; (void)out_handle;
    assert(!pending_task);
    pending_task = task;
    pending_arg = params;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(int ticks) { (void)ticks; }
void esp_restart(void) { reboots++; }
void stack_monitor_record(stack_monitor_t *state) { (void)state; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake error"; }

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config,
                             esp_https_ota_handle_t *handle) {
    assert(strcmp(config->http_config->url, "https://example.com/update.bin") == 0);
    starts++;
    *handle = &incoming;
    return ESP_OK;
}

esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle,
                                    esp_app_desc_t *desc) {
    assert(handle == &incoming);
    *desc = incoming;
    return ESP_OK;
}

esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    downloads++;
    return ESP_OK;
}

bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    return true;
}

esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    finishes++;
    return finish_result;
}

esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    aborts++;
    return ESP_OK;
}

static void attempt(const char *version, bool force, ota_result_t expected) {
    snprintf(incoming.version, sizeof(incoming.version), "%s", version);
    record[0] = '\0';
    starts = downloads = finishes = aborts = reboots = callbacks = 0;
    ota_start("https://example.com/update.bin", force, on_result, &callbacks);
    assert(callbacks == 0);
    assert(pending_task);
    TaskFunction_t task = pending_task;
    pending_task = NULL;
    task(pending_arg);
    assert(callbacks == 1);
    assert(last_result == expected);
    assert(starts == 1);
    assert(downloads == (expected != OTA_RESULT_SKIPPED));
    assert(finishes == (expected != OTA_RESULT_SKIPPED));
    assert(aborts == (expected == OTA_RESULT_SKIPPED));
    assert(reboots == (expected == OTA_RESULT_APPLIED));
    /* Only an applied update leaves a record, naming its partition and image. */
    assert((record[0] != '\0') == (expected == OTA_RESULT_APPLIED));
}

/* The boot after an update: its record names the running image only. */
static void check_record(void) {
    memset(incoming.app_elf_sha256, 0xa5, sizeof(incoming.app_elf_sha256));
    incoming.app_elf_sha256[31] = 0x01;
    running_part = &ota_0;
    attempt("1000.0.0", false, OTA_RESULT_APPLIED);
    assert(strcmp(record, "00620000:a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
                          "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a501") == 0);
    assert(!ota_verify_recorded_for_running());   /* still the old image */
    running_part = &ota_1;                         /* rebooted into it */
    assert(!ota_verify_recorded_for_running());   /* another image there */
    running.app_elf_sha256[31] = 0x01;
    memset(running.app_elf_sha256, 0xa5, 31);
    assert(ota_verify_recorded_for_running());
    ota_verify_forget();
    assert(!ota_verify_recorded_for_running());
    memset(running.app_elf_sha256, 0, sizeof(running.app_elf_sha256));
    running_part = &ota_0;

    /* No record, no install: finish() never switches the boot partition. */
    record_set_fails = true;
    starts = downloads = finishes = aborts = reboots = callbacks = 0;
    ota_start("https://example.com/update.bin", false, on_result, &callbacks);
    TaskFunction_t task = pending_task;
    pending_task = NULL;
    task(pending_arg);
    assert(callbacks == 1 && last_result == OTA_RESULT_FAILED);
    assert(finishes == 0 && aborts == 1 && reboots == 0);
    record_set_fails = false;
}
#endif

/* Only a Vibe Mode install, on the ESP-Mosaico's Vibe Mode build, passes. */
static void check_vibe_mode(void) {
#if VIBE_MODE_BUILD
    running_part = &ota_0;
    assert(ota_installed_by_vibe_mode());
    has_factory = false;                       /* no Vibe Mode partition */
    assert(!ota_installed_by_vibe_mode());
    has_factory = true;
    factory_app_valid = false;                 /* no app in it */
    assert(!ota_installed_by_vibe_mode());
    factory_app_valid = true;
    has_sysmeta = false;                       /* not the retained layout */
    assert(!ota_installed_by_vibe_mode());
    has_sysmeta = true;
    running_part = &factory_part;              /* Vibe Mode itself */
    assert(!ota_installed_by_vibe_mode());
    running_part = &ota_0;
    running_state = ESP_OTA_IMG_VALID;         /* already accepted */
    assert(!ota_installed_by_vibe_mode());
    running_state = ESP_OTA_IMG_PENDING_VERIFY;
#if CONFIG_HOMEHUB_OTA_ENABLED
    /* device.ota's own update keeps its health check. */
    assert(config_set_str("ota_verify", "00020000:"
                          "0000000000000000000000000000000000000000000000000000000000000000"));
    assert(ota_verify_recorded_for_running());
    assert(!ota_installed_by_vibe_mode());
    ota_verify_forget();
#endif
    assert(ota_installed_by_vibe_mode());
#else
    assert(!ota_installed_by_vibe_mode());
#endif
}

int main(void) {
#if CONFIG_HOMEHUB_OTA_ENABLED
    assert(ota_is_enabled());
    attempt("1.0.1", false, OTA_RESULT_SKIPPED);
    attempt("999.0.0", false, OTA_RESULT_SKIPPED);
    attempt("1000.0.0", false, OTA_RESULT_APPLIED);
    attempt("1.0.1", true, OTA_RESULT_APPLIED);
    check_record();
    finish_result = ESP_FAIL;
    attempt("1000.0.0", false, OTA_RESULT_FAILED);   /* and forgets the record */
#else
    // No network, task or flash fakes are linked in this configuration.
    // A dependency on any of those operations would fail the link.
    assert(!ota_is_enabled());
    const char *urls[] = { "https://example.com/update.bin", "", NULL };
    for (unsigned i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        for (int force = 0; force <= 1; force++) {
            callbacks = 0;
            ota_start(urls[i], force, on_result, &callbacks);
            assert(callbacks == 1);
            assert(last_result == OTA_RESULT_SKIPPED);
            assert(strcmp(last_detail, "OTA disabled in this build") == 0);
            ota_start(urls[i], force, NULL, NULL);
        }
    }
    assert(!ota_verify_recorded_for_running());
    ota_verify_forget();
#endif
    check_vibe_mode();
    puts("OTA harness passed");
    return 0;
}
