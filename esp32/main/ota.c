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

#include "muse_restart.h"
#include "ota.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG __attribute__((unused)) = "link.ota";   // a build may compile its logs out

// Muse Gadget builds refuse Wi-Fi OTA by policy (ota.h), unless the
// same-board opt-in lets an image of this board through.
#if CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD
#define OTA_GADGET_REFUSES 1
#else
#define OTA_GADGET_REFUSES 0
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO && CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD && CONFIG_HOMEHUB_OTA_ENABLED
#define OTA_SAME_BOARD 1
#else
#define OTA_SAME_BOARD 0
#endif
// A Home Link build (no musegadgets BLE audio) checks the device model only: an image for another
// model with the same chip (another Home Link board) is refused before anything is written.
#if !CONFIG_MUSE_GADGET_BLE_AUDIO && CONFIG_HOMEHUB_OTA_ENABLED
#define OTA_MODEL_CHECK 1
#else
#define OTA_MODEL_CHECK 0
#endif
// Either check reads the download's first bytes (muse_image_id.h).
#define OTA_IMAGE_HEAD (OTA_SAME_BOARD || OTA_MODEL_CHECK)
#if OTA_IMAGE_HEAD
#include "muse_image_id.h"
#endif

static int s_ota_owner;   // ota_owner_t, changed only by compare-and-swap

bool ota_claim(ota_owner_t who) {
    int free_ = OTA_OWNER_NONE;
    // The Bluetooth update is one transfer that claims again at each begin; every Wi-Fi OTA is its own
    // download task, so a second one is refused like the other kind.
    return __atomic_compare_exchange_n(&s_ota_owner, &free_, (int)who, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
           || (who == OTA_OWNER_BLE && free_ == (int)who);
}

void ota_release(ota_owner_t who) {
    int mine = (int)who;
    __atomic_compare_exchange_n(&s_ota_owner, &mine, OTA_OWNER_NONE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

ota_owner_t ota_owner(void) {
    return (ota_owner_t)__atomic_load_n(&s_ota_owner, __ATOMIC_ACQUIRE);
}

bool ota_is_enabled(void) {
#if CONFIG_HOMEHUB_OTA_ENABLED
    return true;
#else
    return false;
#endif
}

bool ota_wifi_refused(void) {
    return OTA_GADGET_REFUSES;
}

static bool token_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
           || c == '_' || c == '-';
}

// Appends [p, p + n) to out (at *len), printable only; false once full.
static bool put(char *out, size_t cap, size_t *len, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (*len + 1 >= cap) return false;
        char c = p[i];
        out[(*len)++] = c >= 0x20 && c < 0x7F ? c : '?';
    }
    out[*len] = '\0';
    return true;
}

void ota_url_for_log(const char *url, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!url || !*url) {
        snprintf(out, cap, "(no URL)");
        return;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t auth_len = strcspn(p, "/?#");
    const char *host = p;
    for (size_t i = 0; i < auth_len; i++) {
        if (p[i] == '@') host = p + i + 1;   // userinfo (user:password@) dropped
    }
    size_t len = 0;
    bool fits = put(out, cap, &len, host, (size_t)(p + auth_len - host));
    const char *path = p + auth_len;
    size_t path_len = strcspn(path, "?#");
    for (size_t i = 0; fits && i < path_len;) {
        if (path[i] == '/') {
            fits = put(out, cap, &len, "/", 1);
            i++;
            continue;
        }
        size_t seg = strcspn(path + i, "/?#");
        bool tokenish = seg >= 32;
        for (size_t k = 0; tokenish && k < seg; k++) tokenish = token_char(path[i + k]);
        if (tokenish) {
            char masked[24];
            int m = snprintf(masked, sizeof(masked), "<%u chars>", (unsigned)seg);
            fits = put(out, cap, &len, masked, (size_t)m);
        } else {
            fits = put(out, cap, &len, path + i, seg);
        }
        i += seg;
    }
    if (fits && path[path_len] != '\0') {
        fits = put(out, cap, &len, " (query dropped)", 16);
    }
    if (!fits && cap > 4) {
        memcpy(out + cap - 4, "...", 4);
    }
}

void ota_log_refusal(const char *via, const char *url, const char *offered_version) {
    char target[128];
    ota_url_for_log(url, target, sizeof(target));
    char version[33] = "unknown (nothing downloaded)";
    if (offered_version && *offered_version) {
        size_t n = 0;
        put(version, sizeof(version), &n, offered_version, strnlen(offered_version, sizeof(version) - 1));
    }
    (void)via;   // used only by the log line, which a build may compile out
    ESP_LOGW(TAG, "%s refused: this Muse Gadget updates over Bluetooth (MCUmgr: the Muse app or a web updater), "
                  "not Wi-Fi OTA; offered version %s, from %s",
             via ? via : "OTA", version, target);
}

// A refusal by policy, reported the way ota_start reports everything else.
static bool refuse_by_policy(const char *url, ota_status_cb cb, void *user) {
    if (!ota_wifi_refused()) return false;
    ota_log_refusal("OTA", url, NULL);
    if (cb) {
        ota_event_t ev = {
            .result = OTA_RESULT_UNSUPPORTED,
            .detail = "unsupported: this Muse Gadget updates over Bluetooth (the Muse app or a web updater)",
            .new_version = NULL,
            .running_version = esp_app_get_description()->version,
        };
        cb(&ev, user);
    }
    return true;
}

#if CONFIG_MUSE_BOARD_ESP_MOSAICO && CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY
#include "esp_ota_ops.h"
#include "esp_partition.h"
#endif

bool ota_installed_by_vibe_mode(void) {
#if CONFIG_MUSE_BOARD_ESP_MOSAICO && CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY
    // Only an ESP-Mosaico build installed through its Vibe Mode firmware
    // (the ESP-Iris overlay, which links Espressif's app-side Recovery
    // support). On the board, Vibe Mode sits in `factory` (Vibe Mode 0.1,
    // Recovery ABI 1) or `vibe_mode`, app/test (0.2, ABI 2), beside its
    // `sysmeta` records and writes apps into an OTA slot: check all of that
    // is there, so nothing else that boots PENDING_VERIFY passes for it.
    const esp_partition_t *running = esp_ota_get_running_partition();
#if CONFIG_ESP_IRIS_RECOVERY_ABI >= 2
    const esp_partition_subtype_t vibe_subtype = ESP_PARTITION_SUBTYPE_APP_TEST;
#else
    const esp_partition_subtype_t vibe_subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY;
#endif
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, vibe_subtype, NULL);
    const esp_partition_t *sysmeta = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "sysmeta");
    esp_app_desc_t vibe_mode;
    esp_ota_img_states_t state;
    return running && factory && sysmeta
        && running->type == ESP_PARTITION_TYPE_APP
        && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN
        && running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX
        && running->address != factory->address
        && esp_ota_get_partition_description(factory, &vibe_mode) == ESP_OK
        && esp_ota_get_state_partition(running, &state) == ESP_OK
        && state == ESP_OTA_IMG_PENDING_VERIFY
        // ota_start's own update, if one ever lands here, keeps its check.
        && !ota_verify_recorded_for_running();
#else
    return false;
#endif
}

#if CONFIG_HOMEHUB_OTA_ENABLED
#include "stack_monitor.h"
#include "config_store.h"
#include "esp_ota_ops.h"
#include <inttypes.h>
#include <stdint.h>

#include <stdlib.h>

#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_HTTP_TIMEOUT_MS        30000
#define OTA_MAX_REDIRECTS          5
#define OTA_HTTP_TX_BUFFER_BYTES   4096
#define OTA_USER_AGENT_BYTES       64


typedef struct {
    char *url;
    bool force;
    ota_status_cb cb;
    void *user;
#if OTA_IMAGE_HEAD
    // The download's first bytes, as the HTTP client delivers them
    // (HTTP_EVENT_ON_DATA), for the image check before anything is written.
    uint16_t head_n;
    uint8_t head[MUSE_IMAGE_HEAD_BYTES];
#endif
} ota_ctx_t;

// Parse a leading "[v]MAJOR.MINOR.PATCH" into three components. Non-numeric
// suffixes (e.g. git-describe "-15-gabc-dirty") are ignored.
static void parse_ver(const char *s, long out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3 && *s; i++) {
        char *end;
        out[i] = strtol(s, &end, 10);
        if (end == s) break;
        s = end;
        if (*s == '.') s++; else break;
    }
}

// True if `cand` is a newer version than `cur`. Numeric semver components win;
// if they're equal, any string difference (a release X.Y.Z and its dev builds
// X.Y.Z+g<commit>[.dirty], cmake/version.cmake) counts as newer, and an
// identical string counts as not-newer. 999.0.0 was a placeholder every build
// carried before version.txt held a real release, so
// a device running it takes any version.
static bool version_is_newer(const char *cand, const char *cur) {
    if (cur && strcmp(cur, "999.0.0") == 0) return cand && strcmp(cand, cur) != 0;
    long a[3], b[3];
    parse_ver(cand, a);
    parse_ver(cur, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    if (cand && cur && strcmp(cand, cur) != 0) return true;
    return false;
}

static void emit(ota_ctx_t *ctx, ota_result_t result, const char *detail,
                 const char *new_version, const char *running_version) {
    if (!ctx->cb) return;
    ota_event_t ev = {
        .result = result,
        .detail = detail,
        .new_version = new_version,
        .running_version = running_version,
    };
    ctx->cb(&ev, ctx->user);
}

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt) {
    const char *phase = "ota download";
#if OTA_IMAGE_HEAD
    ota_ctx_t *ctx = (ota_ctx_t *)evt->user_data;
#endif
    switch (evt->event_id) {
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGI(TAG, "%s request headers sent", phase);
#if OTA_IMAGE_HEAD
            if (ctx) ctx->head_n = 0;   // a new request (a redirect): its body starts again
#endif
            break;
#if OTA_IMAGE_HEAD
        case HTTP_EVENT_ON_DATA: {
            // Only the image's own body: not a redirect's or an error page's.
            int status = esp_http_client_get_status_code(evt->client);
            if (ctx && status >= 200 && status < 300 && evt->data && evt->data_len > 0
                && ctx->head_n < sizeof(ctx->head)) {
                size_t k = sizeof(ctx->head) - ctx->head_n;
                if ((size_t)evt->data_len < k) k = (size_t)evt->data_len;
                memcpy(ctx->head + ctx->head_n, evt->data, k);
                ctx->head_n = (uint16_t)(ctx->head_n + k);
            }
            break;
        }
#endif
        case HTTP_EVENT_ON_HEADER:
#if CONFIG_HOMEHUB_DEV_BUILD
            ESP_LOGI(TAG, "%s response header: %s", phase,
                     evt->header_key ? evt->header_key : "<unknown>");
#endif
            break;
        case HTTP_EVENT_REDIRECT: {
            esp_http_client_redirect_event_data_t *redirect =
                (esp_http_client_redirect_event_data_t *)evt->data;
            ESP_LOGI(TAG, "%s redirect status=%d", phase,
                     redirect ? redirect->status_code : 0);
            break;
        }
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "%s http event error", phase);
            break;
        default:
            break;
    }
    return ESP_OK;
}

#if CONFIG_HOMEHUB_DEV_BUILD
static void log_request_headers(const char *phase, const char *method,
                                const char *url, const char *user_agent) {
    (void)url;
    ESP_LOGI(TAG, "%s request: %s", phase, method);
    ESP_LOGI(TAG, "%s request header: User-Agent: %s",
             phase, user_agent ? user_agent : "");
    ESP_LOGI(TAG, "%s request header: Accept: <unset>", phase);
    ESP_LOGI(TAG, "%s request header: Range: <unset>", phase);
}
#else
static void log_request_headers(const char *phase, const char *method,
                                const char *url, const char *user_agent) {
    (void)phase;
    (void)method;
    (void)url;
    (void)user_agent;
}
#endif

// The record of an installed update (ota.h): "<partition address>:<ELF
// SHA-256>" in hex.
#define OTA_VERIFY_KEY "ota_verify"
#define OTA_VERIFY_LEN (8 + 1 + 64)

static void ota_verify_format(uint32_t address, const uint8_t sha[32], char out[OTA_VERIFY_LEN + 1]) {
    int n = snprintf(out, OTA_VERIFY_LEN + 1, "%08" PRIx32 ":", address);
    for (int i = 0; i < 32; i++) {
        n += snprintf(out + n, (size_t)(OTA_VERIFY_LEN + 1 - n), "%02x", sha[i]);
    }
}

bool ota_verify_recorded_for_running(void) {
    const esp_partition_t *part = esp_ota_get_running_partition();
    char want[OTA_VERIFY_LEN + 1], have[OTA_VERIFY_LEN + 2];
    if (!part || !config_get_str(OTA_VERIFY_KEY, have, sizeof(have))) {
        return false;
    }
    ota_verify_format(part->address, esp_app_get_description()->app_elf_sha256, want);
    return strcmp(have, want) == 0;
}

void ota_verify_forget(void) {
    config_erase_key(OTA_VERIFY_KEY);
}

#if OTA_IMAGE_HEAD
#ifdef ESP_PLATFORM
_Static_assert(offsetof(esp_app_desc_t, version) == 16, "esp_app_desc_t.version");
_Static_assert(offsetof(esp_app_desc_t, app_elf_sha256) == 144, "esp_app_desc_t.app_elf_sha256");
#endif
#define HEAD_VERSION (MUSE_IMAGE_APP_DESC_OFFSET + 16)
#define HEAD_ELF_SHA (MUSE_IMAGE_APP_DESC_OFFSET + 144)

static muse_image_verdict_t image_verdict(const ota_ctx_t *ctx, const esp_app_desc_t *desc,
                                          muse_image_info_t *info) {
    if (ctx->head_n < MUSE_IMAGE_HEAD_BYTES
        || memcmp(ctx->head + HEAD_VERSION, desc->version, sizeof(desc->version)) != 0
        || memcmp(ctx->head + HEAD_ELF_SHA, desc->app_elf_sha256, sizeof(desc->app_elf_sha256)) != 0) {
        memset(info, 0, sizeof(*info));
        ESP_LOGW(TAG, "the download's first bytes weren't seen whole (%u bytes)", (unsigned)ctx->head_n);
        return MUSE_IMAGE_SHORT;
    }
#if OTA_SAME_BOARD
    return muse_image_check(ctx->head, ctx->head_n, true, info);
#else
    return muse_image_check_model(ctx->head, ctx->head_n, info);
#endif
}

#if OTA_SAME_BOARD
static const char *who_short(const muse_image_info_t *info) {
    return info->product_code[0] ? info->product_code : info->board_id;
}
#endif
#endif

// Streaming OTA on a dedicated task (callers are often on the WS event-handler
// or BLE-host thread, which must not block). Reads the incoming descriptor,
// applies the version gate, then downloads/verifies/installs and reboots.
static void ota_task(void *arg) {
    ota_ctx_t *ctx = (ota_ctx_t *)arg;
    char running[sizeof(esp_app_get_description()->version) + 1];
    memcpy(running, esp_app_get_description()->version, sizeof(running) - 1);
    running[sizeof(running) - 1] = '\0';
    char user_agent[OTA_USER_AGENT_BYTES];
    snprintf(user_agent, sizeof(user_agent), "HatchLink/%s", running);
    ESP_LOGI(TAG, "begin OTA (force=%d, running=%s)", ctx->force, running);

    const char *ota_url = ctx->url;
    log_request_headers("ota download", "GET", ota_url, user_agent);

    esp_http_client_config_t http_cfg = {
        .url = ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = user_agent,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .max_redirection_count = OTA_MAX_REDIRECTS,
        .buffer_size_tx = OTA_HTTP_TX_BUFFER_BYTES,
        .event_handler = ota_http_event_handler,
#if OTA_IMAGE_HEAD
        .user_data = ctx,
#endif
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || !handle) {
        char msg[96];
        snprintf(msg, sizeof(msg), "begin failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        emit(ctx, OTA_RESULT_FAILED, msg, NULL, running);
        goto done;
    }

    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not read image descriptor: %s",
                 esp_err_to_name(err));
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, "could not read image descriptor", NULL, running);
        goto done;
    }
    char new_version[sizeof(new_desc.version) + 1];
    memcpy(new_version, new_desc.version, sizeof(new_desc.version));
    new_version[sizeof(new_desc.version)] = '\0';
    ESP_LOGI(TAG, "incoming version=%s", new_version);

#if OTA_IMAGE_HEAD
    // Before esp_https_ota_perform (esp_ota_begin and the first write): the
    // image must be this board's (the same-board opt-in) or, on a Home Link
    // build, not another device model's. get_img_desc read its first 1 KB,
    // which the HTTP client also handed the event handler; the copy must
    // match what ESP-IDF read (its version and ELF SHA-256), or it isn't
    // trusted.
    {
        muse_image_info_t info;
        muse_image_verdict_t v = image_verdict(ctx, &new_desc, &info);
#if OTA_SAME_BOARD
        bool refuse = v != MUSE_IMAGE_OK;
#else
        // A Home Link build refuses only what it knows is wrong: another chip
        // or another device model. An image without a model (an older build)
        // or whose first bytes weren't seen installs as before.
        bool refuse = v == MUSE_IMAGE_WRONG_CHIP || v == MUSE_IMAGE_WRONG_MODEL;
#endif
        if (refuse) {
            char who[112], target[128];
            muse_image_describe(&info, who, sizeof(who));
            ota_url_for_log(ctx->url, target, sizeof(target));
            ESP_LOGW(TAG, "Wi-Fi OTA refused before writing: %s (offered %s; this is chip %u, %s %s), from %s",
                     muse_image_verdict_text(v), who, (unsigned)muse_board_desc.chip_id,
                     muse_board_desc.product_code, muse_board_desc.board_id, target);
            esp_https_ota_abort(handle);
            char msg[96];
            snprintf(msg, sizeof(msg), "refused: %s", muse_image_verdict_text(v));
            emit(ctx, OTA_RESULT_REFUSED, msg, new_version, running);
            goto done;
        }
#if OTA_SAME_BOARD
        ESP_LOGI(TAG, "image is this board's (%s)", who_short(&info));
#else
        ESP_LOGI(TAG, "image check: %s", muse_image_verdict_text(v));
#endif
    }
#endif

    if (!ctx->force && !version_is_newer(new_version, running)) {
        ESP_LOGW(TAG, "skipping: %s not newer than %s", new_version, running);
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_SKIPPED, "image not newer than running version",
             new_version, running);
        goto done;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        // streaming; yield briefly between reads
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "download failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, msg, new_version, running);
        goto done;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "incomplete image received");
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, "incomplete image received", new_version, running);
        goto done;
    }

    // The next boot holds this image to the health check (app.c), so record it
    // before finish() can switch the boot partition.
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    char record[OTA_VERIFY_LEN + 1];
    if (target) {
        ota_verify_format(target->address, new_desc.app_elf_sha256, record);
    }
    if (!target || !config_set_str(OTA_VERIFY_KEY, record)) {
        ESP_LOGE(TAG, "could not record the update for its health check");
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, "could not record the update", new_version, running);
        goto done;
    }

    // finish() validates SHA-256 + signature (when enabled) and sets the boot
    // partition. Do NOT abort after calling finish.
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ota_verify_forget();
        char msg[96];
        snprintf(msg, sizeof(msg), "verify/finish failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        emit(ctx, OTA_RESULT_FAILED, msg, new_version, running);
        goto done;
    }

    ESP_LOGI(TAG, "applied %s -> %s, rebooting", running, new_version);
    emit(ctx, OTA_RESULT_APPLIED, "applied", new_version, running);
    // Let any status notification flush before the reboot tears down the link.
    // The slot's claim (ota_claim) isn't given back: the restart ends it.
    vTaskDelay(pdMS_TO_TICKS(750));
    stack_monitor_record(NULL);
    muse_restart(MUSE_RESTART_OTA);

done:
    ota_release(OTA_OWNER_WIFI);   // a failed or skipped download: the slot is free again
    free(ctx->url);
    free(ctx);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

void ota_start(const char *url, bool force, ota_status_cb cb, void *user) {
    if (refuse_by_policy(url, cb, user)) return;
    ota_ctx_t *ctx = url && *url ? calloc(1, sizeof(*ctx)) : NULL;
    if (ctx) {
        ctx->url = strdup(url);
        ctx->force = force;
        ctx->cb = cb;
        ctx->user = user;
    }
    if (!ctx || !ctx->url) {
        ESP_LOGE(TAG, "could not start OTA: %s",
                 url && *url ? "out of memory" : "empty URL");
        if (ctx) free(ctx);
        if (cb) {
            ota_event_t ev = { .result = OTA_RESULT_FAILED,
                               .detail = url && *url ? "out of memory" : "empty url" };
            cb(&ev, user);
        }
        return;
    }
    if (!ota_claim(OTA_OWNER_WIFI)) {   // one update at a time, refused not queued
        bool wifi = ota_owner() == OTA_OWNER_WIFI;
        ESP_LOGW(TAG, "OTA refused: a %s update is running", wifi ? "Wi-Fi" : "Bluetooth");
        free(ctx->url);
        free(ctx);
        if (cb) {
            ota_event_t ev = { .result = OTA_RESULT_FAILED,
                               .detail = wifi ? "busy: a Wi-Fi update is running"
                                              : "busy: a Bluetooth update is running" };
            cb(&ev, user);
        }
        return;
    }
    // 16 KB stack: TLS handshake + flash writes during the OTA download.
    if (xTaskCreate(ota_task, "ota", 16384, ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start OTA task");
        ota_release(OTA_OWNER_WIFI);
        free(ctx->url);
        free(ctx);
        if (cb) {
            ota_event_t ev = { .result = OTA_RESULT_FAILED,
                               .detail = "could not start ota task" };
            cb(&ev, user);
        }
    }
}

#else

bool ota_verify_recorded_for_running(void) {
    return false;
}

void ota_verify_forget(void) {
}

void ota_start(const char *url, bool force, ota_status_cb cb, void *user) {
    (void)force;
    if (refuse_by_policy(url, cb, user)) return;
    if (cb) {
        ota_event_t ev = {
            .result = OTA_RESULT_SKIPPED,
            .detail = "OTA disabled in this build",
            .new_version = NULL,
            .running_version = esp_app_get_description()->version,
        };
        cb(&ev, user);
    }
}

#endif
