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
#include "muse_image_id.h"

static esp_app_desc_t running = { .version = "1.2.0+g1234abcd" };
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
/* A Muse Gadget build: Wi-Fi OTA refused by policy, or (the opt-in) only an
 * image of this board, checked before anything is written. */
#define GADGET_REFUSES (CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD)
#define SAME_BOARD (CONFIG_MUSE_GADGET_BLE_AUDIO && CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD \
                    && CONFIG_HOMEHUB_OTA_ENABLED)
/* A Home Link build: ota.c reads the download's first bytes for the device model check too. */
#define IMAGE_HEAD (SAME_BOARD || (!CONFIG_MUSE_GADGET_BLE_AUDIO && CONFIG_HOMEHUB_OTA_ENABLED))
/* The test sends real image heads (a board description) to a Home Link build: LINK_MODEL_TEST,
 * with this build's model in muse_image_id.c (MUSE_DEVICE_MODEL_HASH). */
#ifndef LINK_MODEL_TEST
#define LINK_MODEL_TEST 0
#endif
#define STREAMS (SAME_BOARD || LINK_MODEL_TEST)

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
 * `factory`, its records in `sysmeta`. Each piece can go missing. The test
 * names the subtype for Vibe Mode 0.2 (app/test). */
#ifndef VIBE_SUBTYPE
#define VIBE_SUBTYPE ESP_PARTITION_SUBTYPE_APP_FACTORY
#endif
static const esp_partition_t factory_part = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = VIBE_SUBTYPE,
    .address = 0x10000, .label = "factory" };
static const esp_partition_t sysmeta_part = {
    .type = ESP_PARTITION_TYPE_DATA, .subtype = ESP_PARTITION_SUBTYPE_DATA_NVS,
    .address = 0xc000, .label = "sysmeta" };
static bool has_factory = true, has_sysmeta = true, factory_app_valid = true;
static esp_ota_img_states_t running_state = ESP_OTA_IMG_PENDING_VERIFY;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label) {
    if (type == ESP_PARTITION_TYPE_APP && subtype == VIBE_SUBTYPE && !label) {
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

#if IMAGE_HEAD
/* The HTTP client's side: the event handler esp_https_ota hands it, which
 * sees each response body's bytes (HTTP_EVENT_ON_DATA) as they're read. */
static esp_http_client_event_cb_t http_cb;
static void *http_user;
static int http_status;
int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    (void)client;
    return http_status;
}
#endif
#if STREAMS
static uint8_t streamed[1024];   /* the image's first 1 KB, as get_img_desc reads it */
static bool redirect_first;      /* a 302 with a body before the image */
static const uint8_t *image_model;   /* the board description's device model (8 bytes), or none */
static void http_event(esp_http_client_event_id_t id, const void *data, int len) {
    esp_http_client_event_t ev = { .event_id = id, .user_data = http_user,
                                   .data = (void *)data, .data_len = len };
    http_cb(&ev);
}

/* The image the server sends: a header naming `chip`, incoming's
 * description (version, ELF SHA-256) and a board description, or none. */
static void make_image(uint16_t chip, const char *product, const char *board, uint8_t flags) {
    memset(streamed, 0x5a, sizeof(streamed));
    streamed[0] = 0xE9;
    streamed[12] = (uint8_t)chip;
    streamed[13] = (uint8_t)(chip >> 8);
    const uint32_t app_magic = 0xABCD5432u;
    memcpy(streamed + 32, &app_magic, 4);
    memcpy(streamed + 32 + 16, incoming.version, sizeof(incoming.version));
    memcpy(streamed + 32 + 144, incoming.app_elf_sha256, sizeof(incoming.app_elf_sha256));
    if (product) {
        muse_board_desc_t d = { .magic = MUSE_BOARD_DESC_MAGIC, .layout = 1, .flags = flags, .chip_id = chip };
        snprintf(d.product_code, sizeof(d.product_code), "%s", product);
        snprintf(d.board_id, sizeof(d.board_id), "%s", board);
        if (image_model) {
            d.flags |= MUSE_BOARD_DESC_F_MODEL;
            memcpy(d.model_hash, image_model, sizeof(d.model_hash));
        }
        memcpy(streamed + MUSE_IMAGE_BOARD_DESC_OFFSET, &d, sizeof(d));
    }
}
#endif

static const char *expected_url = "https://example.com/update.bin";

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config,
                             esp_https_ota_handle_t *handle) {
    assert(strcmp(config->http_config->url, expected_url) == 0);
#if IMAGE_HEAD
    http_cb = config->http_config->event_handler;
    http_user = config->http_config->user_data;
    assert(http_cb && http_user);
#endif
    starts++;
    *handle = &incoming;
    return ESP_OK;
}

esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle,
                                    esp_app_desc_t *desc) {
    assert(handle == &incoming);
#if STREAMS
    if (redirect_first) {   /* its body is not the image's */
        http_status = 302;
        http_event(HTTP_EVENT_HEADERS_SENT, NULL, 0);
        static const uint8_t page[400] = { 0xE9 };
        http_event(HTTP_EVENT_ON_DATA, page, sizeof(page));
    }
    http_status = 200;
    http_event(HTTP_EVENT_HEADERS_SENT, NULL, 0);
    for (int off = 0; off < (int)sizeof(streamed); off += 150) {   /* in pieces, as TLS reads come */
        int n = (int)sizeof(streamed) - off < 150 ? (int)sizeof(streamed) - off : 150;
        http_event(HTTP_EVENT_ON_DATA, streamed + off, n);
    }
#endif
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
#if SAME_BOARD
    make_image(9, "W175", "waveshare_s3_175c", 3);   /* this board's */
#endif
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

/* One update at a time with the Bluetooth update (MCUmgr SMP). */
static void check_claim(void) {
    assert(ota_owner() == OTA_OWNER_NONE);
    assert(ota_claim(OTA_OWNER_BLE) && ota_claim(OTA_OWNER_BLE));   /* its holder may claim again */
    callbacks = 0;
    ota_start("https://example.com/update.bin", false, on_result, &callbacks);
    assert(!pending_task);   /* refused at once: no download task, nothing queued */
    assert(callbacks == 1 && last_result == OTA_RESULT_FAILED);
    assert(strstr(last_detail, "busy"));
    ota_release(OTA_OWNER_WIFI);   /* not its claim: nothing changes */
    assert(ota_owner() == OTA_OWNER_BLE);
    ota_release(OTA_OWNER_BLE);
    /* A download holds it until it ends; the Bluetooth update is refused meanwhile. */
    snprintf(incoming.version, sizeof(incoming.version), "%s", "1000.0.0");
    finish_result = ESP_FAIL;
    callbacks = 0;
    ota_start("https://example.com/update.bin", false, on_result, &callbacks);
    assert(pending_task && ota_owner() == OTA_OWNER_WIFI);
    assert(!ota_claim(OTA_OWNER_BLE));
    TaskFunction_t task = pending_task;
    pending_task = NULL;
    /* A second Wi-Fi OTA meanwhile: refused at once, the first keeps the slot. */
    ota_start("https://example.com/update.bin", false, on_result, &callbacks);
    assert(!pending_task && callbacks == 1 && last_result == OTA_RESULT_FAILED && strstr(last_detail, "busy"));
    assert(ota_owner() == OTA_OWNER_WIFI);
    callbacks = 0;
    task(pending_arg);   /* fails: the slot is free again */
    assert(callbacks == 1 && last_result == OTA_RESULT_FAILED);
    assert(ota_owner() == OTA_OWNER_NONE && ota_claim(OTA_OWNER_BLE));
    ota_release(OTA_OWNER_BLE);
    finish_result = ESP_OK;
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
    /* ota_start's own update keeps its health check. */
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

#if STREAMS
/* One attempt with a real image head (the same-board opt-in, or a Home Link
 * build's model check): the check runs before esp_https_ota_perform
 * (esp_ota_begin and the first write), so a refused image never reaches flash. */
static void same_board_attempt(uint16_t chip, const char *product, const char *board, uint8_t flags,
                               ota_result_t expected, const char *detail) {
    snprintf(incoming.version, sizeof(incoming.version), "%s", "1000.0.0");
    memset(incoming.app_elf_sha256, 0x3c, sizeof(incoming.app_elf_sha256));
    make_image(chip, product, board, flags);
    record[0] = '\0';
    starts = downloads = finishes = aborts = reboots = callbacks = 0;
    ota_start("https://example.com/update.bin", false, on_result, &callbacks);
    assert(pending_task);
    TaskFunction_t task = pending_task;
    pending_task = NULL;
    task(pending_arg);
    assert(callbacks == 1 && last_result == expected);
    assert(starts == 1);
    assert(downloads == (expected == OTA_RESULT_APPLIED));   /* no esp_ota_begin, no write */
    assert(aborts == (expected != OTA_RESULT_APPLIED));
    assert(reboots == (expected == OTA_RESULT_APPLIED));
    if (detail) {
        assert(strstr(last_detail, detail));
    }
    printf("attempt: %s\n", last_detail);
}
#endif

#if SAME_BOARD
static void check_same_board(void) {
    assert(ota_is_enabled() && !ota_wifi_refused());
    /* The default Home Link image, built for the C5. */
    same_board_attempt(23, "", "none", 0, OTA_RESULT_REFUSED, "another chip");
    /* A Home Link image for this chip: it would pass ESP-IDF's chip id check. */
    same_board_attempt(9, "", "none", 0, OTA_RESULT_REFUSED, "another board");
    same_board_attempt(9, "SCW1", "sensecap_watcher", 3, OTA_RESULT_REFUSED, "another board");
    same_board_attempt(9, "W175", "waveshare_s3_175", 3, OTA_RESULT_REFUSED, "another board");
    same_board_attempt(9, NULL, NULL, 0, OTA_RESULT_REFUSED, "no board description");
    same_board_attempt(9, "W175", "waveshare_s3_175c", 1, OTA_RESULT_REFUSED, "no Bluetooth update");
    same_board_attempt(9, "W175", "waveshare_s3_175c", 3, OTA_RESULT_APPLIED, NULL);
    /* A redirect's body before the image's doesn't count as the image. */
    redirect_first = true;
    same_board_attempt(9, "W175", "waveshare_s3_175c", 3, OTA_RESULT_APPLIED, NULL);
    same_board_attempt(23, "", "none", 0, OTA_RESULT_REFUSED, "another chip");
    redirect_first = false;
    /* What the client delivered must be what ESP-IDF read: else refused. */
    make_image(9, "W175", "waveshare_s3_175c", 3);
    streamed[32 + 144] ^= 1;
    record[0] = '\0';
    starts = downloads = finishes = aborts = reboots = callbacks = 0;
    ota_start("https://example.com/update.bin", false, on_result, &callbacks);
    TaskFunction_t task = pending_task;
    pending_task = NULL;
    task(pending_arg);
    assert(callbacks == 1 && last_result == OTA_RESULT_REFUSED && downloads == 0 && aborts == 1);
}
#endif

#if LINK_MODEL_TEST
/* A Home Link build (this one: an S3 with no product code, board "none", its own device model):
 * another model's image is refused before anything is written; its own model's, an image without
 * a model (an older build) and one without a board description install; another chip's is refused. */
static const uint8_t other_model[8] = { 0x6f, 0x74, 0x68, 0x65, 0x72, 0x21, 0x21, 0x21 };
static void check_link_model(void) {
    assert(ota_is_enabled() && !ota_wifi_refused());
    assert(muse_board_desc.flags & MUSE_BOARD_DESC_F_MODEL);
    image_model = other_model;
    same_board_attempt(9, "", "none", MUSE_BOARD_DESC_F_MODEL, OTA_RESULT_REFUSED, "another device model");
    image_model = muse_board_desc.model_hash;
    same_board_attempt(9, "", "none", MUSE_BOARD_DESC_F_MODEL, OTA_RESULT_APPLIED, NULL);
    same_board_attempt(23, "", "none", MUSE_BOARD_DESC_F_MODEL, OTA_RESULT_REFUSED, "another chip");
    image_model = NULL;
    same_board_attempt(9, "", "none", 0, OTA_RESULT_APPLIED, NULL);     /* no model: an older image */
    same_board_attempt(9, NULL, NULL, 0, OTA_RESULT_APPLIED, NULL);     /* no board description at all */
    /* A redirect's body before the image's doesn't count as the image. */
    redirect_first = true;
    image_model = other_model;
    same_board_attempt(9, "", "none", MUSE_BOARD_DESC_F_MODEL, OTA_RESULT_REFUSED, "another device model");
    redirect_first = false;
    image_model = NULL;
}
#endif

int main(void) {
#if GADGET_REFUSES
    /* Refused at once: no task, no download, nothing written; logged. */
    assert(ota_wifi_refused());
    callbacks = 0;
    ota_start("https://user:pw@ota.example.com:8443/fw/esp32c5/homelink-1.3.0.bin"
              "?X-Amz-Signature=secret-token-value#frag", true, on_result, &callbacks);
    assert(callbacks == 1 && last_result == OTA_RESULT_UNSUPPORTED);
    assert(strstr(last_detail, "unsupported"));
#if CONFIG_HOMEHUB_OTA_ENABLED
    assert(!pending_task && starts == 0 && ota_owner() == OTA_OWNER_NONE);
#endif
    ota_start("https://example.com/update.bin", false, NULL, NULL);
    puts("OTA harness passed");
    return 0;
#endif
#if LINK_MODEL_TEST
    check_link_model();
    puts("OTA harness passed");
    return 0;
#endif
#if SAME_BOARD
    check_same_board();
    attempt("1.0.1", false, OTA_RESULT_SKIPPED);   /* this board's image, older: the version gate still applies */
    puts("OTA harness passed");
    return 0;
#endif
#if CONFIG_HOMEHUB_OTA_ENABLED
    assert(ota_is_enabled());
    attempt("1.0.1", false, OTA_RESULT_SKIPPED);
    attempt("1.2.0+g1234abcd", false, OTA_RESULT_SKIPPED);   /* the same image */
    attempt("1000.0.0", false, OTA_RESULT_APPLIED);
    attempt("1.0.1", true, OTA_RESULT_APPLIED);
    check_record();
    check_claim();
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
