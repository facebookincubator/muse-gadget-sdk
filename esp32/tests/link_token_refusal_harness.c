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

// Runs app.c's token, unpair and boot-validation paths (extracted verbatim by
// test_link_token_refusal.py into app_token_paths.inc and app_boot_setup.inc)
// against the real config_store.c. NVS, HTTP, BLE and the clock are fakes.
// Build with -DREOPEN_IN_PLACE=0/1 (Link / Muse) and -DWIFI_WITHOUT_PAIRING.

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "vm_api.h"

#define CHECK(cond, ...) do {                                                \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        exit(1);                                                             \
    }                                                                        \
} while (0)

// ---- fake NVS (commits immediately) ----------------------------------------

#define MAX_KEYS 32
static struct { bool used; char key[16]; char value[256]; } s_nvs[MAX_KEYS];

static int nvs_find(const char *key) {
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) return i;
    }
    return -1;
}

static void nvs_put(const char *key, const char *value) {
    int i = nvs_find(key);
    for (int j = 0; i < 0 && j < MAX_KEYS; j++) {
        if (!s_nvs[j].used) i = j;
    }
    CHECK(i >= 0, "fake NVS full");
    s_nvs[i].used = true;
    snprintf(s_nvs[i].key, sizeof(s_nvs[i].key), "%s", key);
    snprintf(s_nvs[i].value, sizeof(s_nvs[i].value), "%s", value);
}

static bool has(const char *key) { return nvs_find(key) >= 0; }

static const char *value_of(const char *key) {
    int i = nvs_find(key);
    return i < 0 ? "" : s_nvs[i].value;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char *part_name) { (void)part_name; return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)ns; (void)mode; *out = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    (void)h;
    int i = nvs_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = strlen(s_nvs[i].value) + 1;
    if (out) {
        if (*len < need) return ESP_ERR_NVS_INVALID_LENGTH;
        memcpy(out, s_nvs[i].value, need);
    }
    *len = need;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    (void)h; nvs_put(key, value); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    int i = nvs_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    s_nvs[i].used = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "err"; }

// ---- fake platform ---------------------------------------------------------

#define TAG "harness"
static void log_sink(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(tag, ...) log_sink(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_sink(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) log_sink(tag, __VA_ARGS__)

typedef void *SemaphoreHandle_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
/* A function, not a comma expression: app.c ignores the result as it may
 * with FreeRTOS's, and gcc -Werror would call a bare pdTRUE no effect. */
static int fake_semaphore_take(SemaphoreHandle_t s, TickType_t t)
{
    (void)s;
    (void)t;
    return pdTRUE;
}
#define xSemaphoreTake(s, t) fake_semaphore_take((s), (t))
#define xSemaphoreGive(s) ((void)(s))
#define xTaskCreate(fn, name, stack, arg, prio, handle) \
    ((void)(fn), (void)(name), (void)(arg), (void)(handle), pdPASS)
static void vTaskDelay(uint32_t ticks) { (void)ticks; }

static int64_t s_now = 1000000;
static int64_t esp_timer_get_time(void) { return s_now; }

#define LED_STATE_ERROR 0
static void led_status_set_state(int state) { (void)state; }
static char s_status[32];
static void ui_set_status(const char *status) { snprintf(s_status, sizeof(s_status), "%s", status); }
static void ui_set_ble(const char *s) { (void)s; }
static void ble_server_send_status(const char *s) { (void)s; }
static void ble_server_stop_advertising(bool full) { (void)full; }
static void ble_server_set_companion_advertising(bool on) { (void)on; }
static void ble_coex_set_active(bool on) { (void)on; }
static void ble_server_full_shutdown(void) {}
static void shutdown_ble_task(void *arg) { (void)arg; }
static bool s_ble_connected;
static bool ble_server_has_connection(void) { return s_ble_connected; }
static void setup_window_lock_take(void) {}
static void setup_window_lock_give(void) {}
static void advance_confirm_generation_locked(void) {}
static bool link_pairing_commit_provisioning(uint32_t generation, bool (*commit)(void)) {
    (void)generation; return commit();
}
static void identity_note_paired_sdk_token(void) {}
static const char *identity_node_id(void) { return "node"; }
static bool wifi_mgr_is_connected(void) { return true; }

static bool s_gate_busy;
static bool operation_gate_take(TickType_t wait, const char *what) {
    (void)wait; (void)what; return !s_gate_busy;
}
static void operation_gate_give(void) {}

static int s_windows_opened, s_restarts, s_disconnects, s_finished_resets;
static void open_setup_window(const char *reason) { (void)reason; s_windows_opened++; }
static void restart_after_setup_reset(void) { s_restarts++; }
static void setup_disconnect_to_clean(void) { s_disconnects++; }
static void finish_setup_reset_with_gate_held(void) { s_finished_resets++; }

static char s_base_url[128], s_noise_host[128];
void vm_api_set_base_url(const char *url) { snprintf(s_base_url, sizeof(s_base_url), "%s", url ? url : ""); }
static void noise_ctrl_set_host(const char *host) {
    snprintf(s_noise_host, sizeof(s_noise_host), "%s", host ? host : "");
}

// Server behaviour per call: 0 ok, VM_API_ERR_AUTH (a 401 or 403), or
// VM_API_ERR_FAILED (network).
static int s_fetch_rc, s_refresh_rc, s_mint_rc;
static int s_fetches, s_refreshes, s_mints;
static const char *s_issued_access = "access-2";
static const char *s_issued_refresh = "refresh-2";

static char *dup(const char *s) {
    char *out = malloc(strlen(s) + 1);
    CHECK(out != NULL, "oom");
    memcpy(out, s, strlen(s) + 1);
    return out;
}
int vm_api_fetch_vms(const char *access, vm_info_t *out, int max) {
    (void)access; (void)out; (void)max;
    s_fetches++;
    return s_fetch_rc == 0 ? 1 : s_fetch_rc;
}
static int issue(int rc, vm_device_tokens_t *tokens) {
    if (rc == 0) {
        tokens->access_token = dup(s_issued_access);
        tokens->refresh_token = dup(s_issued_refresh);
    }
    return rc;
}
int vm_api_refresh_device_token(const char *access, const char *refresh, const char *node,
                                vm_device_tokens_t *tokens, vm_token_refresh_method_t *method) {
    (void)access; (void)refresh; (void)node;
    *method = VM_TOKEN_REFRESH_REFRESH_TOKEN;
    s_refreshes++;
    return issue(s_refresh_rc, tokens);
}
int vm_api_mint_device_token(const char *source, const char *node, vm_device_tokens_t *tokens) {
    (void)source; (void)node;
    s_mints++;
    return issue(s_mint_rc, tokens);
}
void vm_device_tokens_free(vm_device_tokens_t *tokens) {
    free(tokens->access_token);
    free(tokens->refresh_token);
    tokens->access_token = tokens->refresh_token = NULL;
}

// ---- app.c state and code under test ---------------------------------------

#define CONFIG_MUSE_ENABLED REOPEN_IN_PLACE
#define CONFIG_HOMEHUB_BLE_PERSISTENT 0
#define CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS 30
#define CONFIG_HOMEHUB_WIFI_SSID ""

#include "app_defines.inc"

static SemaphoreHandle_t s_auth_lock = NULL;
static int64_t s_last_vm_auth_refresh_attempt_us = 0;
static int64_t s_last_periodic_token_refresh_us = 0;
static int64_t s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
static uint8_t s_vm_auth_refusals = 0;
static bool s_ble_started = false;
static atomic_bool s_pairing_reopen_pending;
static atomic_bool s_vm_credentials_accepted;
static const char *s_setup_stage = "idle";

#include "app_token_paths.inc"

// One heartbeat of app_run()'s loop: the two calls it makes for this.
static void app_tick(void) {
    if (atomic_exchange_explicit(&s_pairing_reopen_pending, false,
                                 memory_order_acq_rel)) {
        reopen_pairing_after_token_refusal();
    }
    close_pairing_after_token_accepted();
}

static bool s_boot_setup_complete, s_boot_provisioned;
static void boot(void) {
    s_vm_auth_refusals = 0;
    s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_last_vm_auth_refresh_attempt_us = 0;
    atomic_store(&s_pairing_reopen_pending, false);
    atomic_store(&s_vm_credentials_accepted, false);
    s_base_url[0] = s_noise_host[0] = '\0';
#include "app_boot_setup.inc"
    s_boot_setup_complete = setup_complete;
    s_boot_provisioned = provisioned;
    s_setup_stage = setup_complete ? "done" : "idle";
    if (!setup_complete) open_setup_window("boot: unpaired");
}

// ---- scenarios --------------------------------------------------------------

static const char *const WIFI_KEYS[] = {
    "ssid", "password", "wifi_channel", "wifi_others", "wifi_hidden",
};
static const char *const PAIRING_KEYS[] = {
    "access_token", "refresh_token", "api_url_v2", "noise_host", "username",
};

static void paired_device(void) {
    memset(s_nvs, 0, sizeof(s_nvs));
    nvs_put("ssid", "home");
    nvs_put("password", "wifi-secret");
    nvs_put("wifi_channel", "6");
    nvs_put("wifi_others", "[]");
    nvs_put("wifi_hidden", "0");
    nvs_put("access_token", "access-1");
    nvs_put("refresh_token", "refresh-1");
    nvs_put("api_url_v2", "https://api.example");
    nvs_put("noise_host", "noise.example");
    nvs_put("username", "user");
    nvs_put("setup_complete", "1");
    s_fetch_rc = s_refresh_rc = s_mint_rc = 0;
    s_fetches = s_refreshes = s_mints = 0;
    s_windows_opened = s_restarts = s_disconnects = s_finished_resets = 0;
    s_ble_connected = s_gate_busy = false;
    s_issued_access = "access-2";
    s_issued_refresh = "refresh-2";
    s_now += 1000000000LL;
    boot();
    CHECK(s_boot_setup_complete && !strcmp(s_setup_stage, "done"), "fixture not paired");
    s_windows_opened = 0;
}

static void expect_wifi_kept(const char *when) {
    for (size_t i = 0; i < sizeof(WIFI_KEYS) / sizeof(WIFI_KEYS[0]); i++) {
        CHECK(has(WIFI_KEYS[i]), "%s: Wi-Fi key %s was erased", when, WIFI_KEYS[i]);
    }
    CHECK(!strcmp(value_of("password"), "wifi-secret"), "%s: Wi-Fi password changed", when);
}

static void expect_pairing_kept(const char *when) {
    for (size_t i = 0; i < sizeof(PAIRING_KEYS) / sizeof(PAIRING_KEYS[0]); i++) {
        CHECK(has(PAIRING_KEYS[i]), "%s: pairing key %s was erased", when, PAIRING_KEYS[i]);
    }
    CHECK(!strcmp(value_of("refresh_token"), "refresh-1"), "%s: refresh token changed", when);
}

static void expect_pairing_gone(const char *when) {
    for (size_t i = 0; i < sizeof(PAIRING_KEYS) / sizeof(PAIRING_KEYS[0]); i++) {
        CHECK(!has(PAIRING_KEYS[i]), "%s: pairing key %s survived", when, PAIRING_KEYS[i]);
    }
    CHECK(!has("setup_complete"), "%s: setup marker survived", when);
}

// The VM fetch is refused, forcing a refresh that is itself refused (a 401 or
// a 403: vm_api maps both to VM_API_ERR_AUTH, see test_link_vm_api).
static void refused_refresh_reopens_pairing_and_erases_nothing(void) {
    paired_device();
    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_AUTH;
    vm_info_t vms[1];
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1, "refresh not attempted");
    expect_pairing_kept("refresh refused");
    expect_wifi_kept("refresh refused");
    CHECK(has("setup_complete"), "marker cleared before the app task ran");
    CHECK(atomic_load(&s_pairing_reopen_pending), "pairing reopen not requested");

    // A busy operation gate defers the reopen to a later tick.
    s_gate_busy = true;
    app_tick();
    CHECK(has("setup_complete") && atomic_load(&s_pairing_reopen_pending), "busy gate");
    s_gate_busy = false;
    app_tick();
    CHECK(!has("setup_complete"), "setup marker kept; pairing stays refused");
    expect_pairing_kept("pairing reopened");
    expect_wifi_kept("pairing reopened");
#if REOPEN_IN_PLACE
    CHECK(s_windows_opened == 1 && s_restarts == 0, "Muse reopens pairing in place");
#else
    CHECK(s_restarts == 1 && s_windows_opened == 0, "Link restarts to bring BLE back");
#endif
    CHECK(s_vm_auth_refusals == 1, "refusal not counted for backoff");
}

static void transient_network_failures_change_nothing(void) {
    paired_device();
    vm_info_t vms[1];
    s_fetch_rc = VM_API_ERR_FAILED;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_FAILED, "fetch rc");
    CHECK(s_refreshes == 0, "a network failure must not force a refresh");

    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_FAILED;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1, "refresh not attempted");
    app_tick();
    CHECK(has("setup_complete"), "network failure reopened pairing");
    CHECK(!atomic_load(&s_pairing_reopen_pending), "network failure requested a reopen");
    expect_pairing_kept("refresh network failure");
    expect_wifi_kept("refresh network failure");

    // The heartbeat's proactive refresh tolerates refusals and failures alike.
    s_now += (int64_t)CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS * 24 * 3600 * 1000000LL;
    s_refresh_rc = VM_API_ERR_AUTH;
    CHECK(ensure_access_token_ready_with_gate_held(false, true), "stale token not kept");
    app_tick();
    CHECK(has("setup_complete") && s_windows_opened == 0 && s_restarts == 0,
          "proactive refresh refusal reopened pairing");
    expect_pairing_kept("proactive refresh refused");
}

static void refused_mint_keeps_its_token_and_backs_off(void) {
    paired_device();
    CHECK(config_erase_key("refresh_token"), "fixture");
    s_mint_rc = VM_API_ERR_AUTH;
    CHECK(!ensure_access_token_ready_with_gate_held(false, true), "mint reported ok");
    CHECK(s_mints == 1 && has("access_token"), "refused mint erased its source token");
    CHECK(atomic_load(&s_pairing_reopen_pending), "mint refusal did not reopen pairing");
    // The heartbeat calls back within seconds: no new mint until the backoff ends.
    s_now += 5 * 1000000LL;
    CHECK(!ensure_access_token_ready_with_gate_held(false, true), "backoff");
    CHECK(s_mints == 1, "mint retried inside the refusal backoff");
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_mint_rc = 0;
    CHECK(ensure_access_token_ready_with_gate_held(false, true), "mint after backoff");
    CHECK(s_mints == 2 && !strcmp(value_of("refresh_token"), "refresh-2"), "minted pair");
    expect_wifi_kept("mint");
}

static void accepted_token_closes_reopened_pairing(void) {
    paired_device();
    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_AUTH;
    vm_info_t vms[1];
    fetch_vms_with_refresh_with_gate_held(vms, 1);
    app_tick();
    CHECK(!has("setup_complete"), "pairing not reopened");
    s_setup_stage = "advertising";

    // Retries inside the refusal backoff (Muse reconnects at least every
    // minute) don't ask the refresh endpoint again.
    s_restarts = s_windows_opened = 0;
    s_now += 30 * 1000000LL;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1 && s_vm_auth_refusals == 1, "refresh not paced by the backoff");
    // Once it has passed, a second refusal while pairing is open only widens it.
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    fetch_vms_with_refresh_with_gate_held(vms, 1);
    app_tick();
    CHECK(s_refreshes == 2, "refresh not retried after the backoff");
    CHECK(s_windows_opened == 0 && s_restarts == 0, "reopened twice");
    CHECK(s_vm_auth_refusals == 2
          && s_vm_auth_refresh_interval_us == 2 * VM_AUTH_REFRESH_RETRY_INTERVAL_US,
          "backoff did not widen");

    // The outage ends while a phone is connected for pairing: leave it be.
    s_fetch_rc = 0;
    s_ble_connected = true;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == 1, "fetch");
    app_tick();
    CHECK(!has("setup_complete"), "closed pairing under a connected phone");
    s_ble_connected = false;
    app_tick();
    CHECK(has("setup_complete") && !strcmp(s_setup_stage, "done"), "setup not completed again");
    CHECK(s_vm_auth_refusals == 0, "backoff not reset");
    expect_pairing_kept("token accepted again");
    expect_wifi_kept("token accepted again");
    app_tick();
    CHECK(has("setup_complete"), "idempotent");
}

static void acceptance_before_the_reopen_cancels_it(void) {
    paired_device();
    s_fetch_rc = s_refresh_rc = VM_API_ERR_AUTH;
    vm_info_t vms[1];
    fetch_vms_with_refresh_with_gate_held(vms, 1);
    CHECK(atomic_load(&s_pairing_reopen_pending), "reopen not requested");
    // The service is back before the app task gets to the reopen.
    s_fetch_rc = 0;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == 1, "fetch");
    app_tick();
    CHECK(has("setup_complete") && s_windows_opened == 0 && s_restarts == 0,
          "pairing reopened after the token was accepted again");
    expect_pairing_kept("accepted before the reopen");
    expect_wifi_kept("accepted before the reopen");
}

static void explicit_unpair_clears_pairing_but_keeps_wifi(void) {
    paired_device();
    CHECK(reset_setup_with_gate_held("BLE unpair"), "unpair");
    CHECK(s_disconnects == 1 && s_finished_resets == 1, "unpair did not reset");
    expect_pairing_gone("BLE unpair");
    expect_wifi_kept("BLE unpair");

    paired_device();
    CHECK(clear_setup_credentials(), "server unpair pre-clear");
    expect_pairing_gone("server unpair");
    expect_wifi_kept("server unpair");

    // Setup failure cleanup uses the same path: a failed re-pair keeps Wi-Fi.
    paired_device();
    CHECK(setup_wipe_to_clean(), "setup failure cleanup");
    expect_pairing_gone("setup failure");
    expect_wifi_kept("setup failure");
}

static void reboot_keeps_each_state(void) {
    // After a refusal: tokens and Wi-Fi, no marker. Boot keeps everything,
    // restores the endpoints and opens pairing, without re-marking setup.
    paired_device();
    CHECK(config_clear_setup_complete(), "fixture");
    s_windows_opened = 0;
    boot();
    CHECK(!s_boot_setup_complete && s_boot_provisioned, "kept credentials not usable");
    CHECK(!has("setup_complete"), "boot marked setup complete before a token was accepted");
    CHECK(!strcmp(s_base_url, "https://api.example") && !strcmp(s_noise_host, "noise.example"),
          "kept credentials lost their endpoints");
    CHECK(s_windows_opened == 1, "boot did not open pairing");
    expect_pairing_kept("reboot after refusal");
    expect_wifi_kept("reboot after refusal");

    // A device that really was revoked stays pairable across the reboot: the
    // refusals that follow change nothing and never restart it again.
    s_restarts = 0;
    s_fetch_rc = s_refresh_rc = VM_API_ERR_AUTH;
    vm_info_t vms[1];
    for (int i = 0; i < 3; i++) {
        fetch_vms_with_refresh_with_gate_held(vms, 1);
        app_tick();
    }
    CHECK(s_restarts == 0, "reboot loop");
    CHECK(!has("setup_complete"), "pairing closed while still refused");
    expect_pairing_kept("refused after reboot");

    // ...and a transient fault clears on the first accepted token.
    s_fetch_rc = 0;
    s_now += 60 * 60 * 1000000LL;
    fetch_vms_with_refresh_with_gate_held(vms, 1);
    app_tick();
    CHECK(has("setup_complete"), "reboot after a transient refusal did not recover");

    // After an explicit unpair: Wi-Fi only. Boot keeps it.
    paired_device();
    CHECK(clear_setup_credentials(), "unpair");
    boot();
    CHECK(!s_boot_setup_complete && !s_boot_provisioned, "unpaired boot state");
    expect_wifi_kept("reboot after unpair");
    CHECK(!s_base_url[0], "endpoints restored without pairing");

    // An unpair cut short by power loss after erasing the credentials leaves
    // only the marker: boot finishes the unpair, so pairing is possible again.
    paired_device();
    CHECK(config_clear_pairing(), "fixture");
    boot();
    CHECK(!s_boot_setup_complete && !has("setup_complete"), "stuck paired without credentials");
    expect_wifi_kept("interrupted unpair");

    // A half-written pair (tokens, no Wi-Fi, no marker) is cleared.
    paired_device();
    for (size_t i = 0; i < sizeof(WIFI_KEYS) / sizeof(WIFI_KEYS[0]); i++) {
        config_erase_key(WIFI_KEYS[i]);
    }
    CHECK(config_clear_setup_complete(), "fixture");
    boot();
    CHECK(!s_boot_provisioned && !has("access_token"), "half-written pair kept");

    // Muse forgot its only network after pairing: unpair so it can pair again.
    paired_device();
    CHECK(config_erase_key("ssid"), "fixture");
    boot();
#if WIFI_WITHOUT_PAIRING
    CHECK(!s_boot_setup_complete && !has("access_token") && !has("setup_complete"),
          "Muse without Wi-Fi stays stuck paired");
    CHECK(has("password") && has("wifi_others"), "unpair erased Wi-Fi keys");
#else
    CHECK(s_boot_setup_complete && has("access_token"), "Link without saved Wi-Fi unpaired");
#endif
}

static void re_pairing_replaces_kept_credentials(void) {
    // Genuine revocation: pairing reopens with the old tokens kept...
    paired_device();
    s_fetch_rc = s_refresh_rc = VM_API_ERR_AUTH;
    vm_info_t vms[1];
    fetch_vms_with_refresh_with_gate_held(vms, 1);
    app_tick();
    CHECK(!has("setup_complete"), "pairing not reopened");
    // ...and the phone pairs it again with new ones.
    s_ble_connected = true;
    CHECK(accept_pairing_credentials("access-new", "refresh-new", "user2"), "re-pair");
    CHECK(complete_setup_and_stop_ble("provision", 0), "complete");
    s_ble_connected = false;
    app_tick();
    CHECK(has("setup_complete") && !strcmp(value_of("refresh_token"), "refresh-new"),
          "re-pair did not take");
    expect_wifi_kept("re-pair after revocation");

    // From an explicit unpair (Wi-Fi only) too.
    paired_device();
    CHECK(clear_setup_credentials(), "unpair");
    CHECK(accept_pairing_credentials("access-new", "refresh-new", "user2"), "re-pair");
    CHECK(complete_setup_and_stop_ble("provision", 0), "complete");
    CHECK(has("setup_complete") && has("access_token"), "re-pair after unpair");
    expect_wifi_kept("re-pair after unpair");

    // A refused mint during pairing leaves cleanup to setup failure, which
    // keeps Wi-Fi.
    paired_device();
    CHECK(clear_setup_credentials(), "unpair");
    s_mint_rc = VM_API_ERR_AUTH;
    s_setup_stage = "auth";
    CHECK(!accept_pairing_credentials("sdk-token", NULL, "user2"), "mint refused");
    CHECK(!atomic_load(&s_pairing_reopen_pending), "pairing session reopened pairing");
    CHECK(setup_wipe_to_clean(), "setup failure");
    expect_wifi_kept("refused mint while pairing");
}

int main(void) {
    // Fakes only one build profile's code paths reach.
    (void)ble_server_stop_advertising;
    (void)ble_server_set_companion_advertising;
    (void)ble_coex_set_active;
    (void)ble_server_full_shutdown;
    (void)restart_after_setup_reset;
    refused_refresh_reopens_pairing_and_erases_nothing();
    transient_network_failures_change_nothing();
    refused_mint_keeps_its_token_and_backs_off();
    accepted_token_closes_reopened_pairing();
    acceptance_before_the_reopen_cancels_it();
    explicit_unpair_clears_pairing_but_keeps_wifi();
    reboot_keeps_each_state();
    re_pairing_replaces_kept_credentials();
    puts("token refusal harness: PASS");
    return 0;
}
