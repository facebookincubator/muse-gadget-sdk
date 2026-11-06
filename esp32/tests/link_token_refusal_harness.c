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

// Runs app.c's token, unpair, reset, refresh-scheduler and boot-validation
// paths (extracted verbatim by test_link_token_refusal.py into
// app_token_paths.inc, app_heartbeat.inc and app_boot_setup.inc) against the
// real config_store.c. NVS, HTTP, BLE, tasks and the clock are fakes; a
// created task runs to completion inline. Build with -DMUSE_BUILD=0/1 (Link /
// Muse) and -DWIFI_WITHOUT_PAIRING.

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
// A created task runs to completion before xTaskCreate returns.
static bool s_task_create_fails;
static int s_tasks_started;
static int fake_task_create(void (*fn)(void *), void *arg) {
    if (s_task_create_fails) return 0;
    s_tasks_started++;
    fn(arg);
    return pdPASS;
}
#define xTaskCreate(fn, name, stack, arg, prio, handle) \
    ((void)(name), (void)(handle), fake_task_create((fn), (arg)))
static void vTaskDelay(uint32_t ticks) { (void)ticks; }
static void vTaskDelete(void *task) { (void)task; }
static void stack_monitor_record(void *state) { (void)state; }

static int64_t s_now = 1000000;
static int64_t esp_timer_get_time(void) { return s_now; }

#define LED_STATE_ERROR 0
static void led_status_set_state(int state) { (void)state; }
static char s_status[32];
static void ui_set_status(const char *status) { snprintf(s_status, sizeof(s_status), "%s", status); }
static void ui_set_ble(const char *s) { (void)s; }
static void ui_set_vm(const char *s) { (void)s; }
static void ble_server_send_status(const char *s) { (void)s; }
static void ble_server_stop_advertising(bool full) { (void)full; }
static void ble_server_set_companion_advertising(bool on) { (void)on; }
static void ble_coex_set_active(bool on) { (void)on; }
static void ble_server_full_shutdown(void) {}
static void shutdown_ble_task(void *arg) { (void)arg; }
static bool s_ble_connected;
static bool ble_server_has_connection(void) { return s_ble_connected; }
static int s_ble_disconnects;
static void ble_server_disconnect_client(void) { s_ble_disconnects++; }
static void setup_window_lock_take(void) {}
static void setup_window_lock_give(void) {}
static void advance_confirm_generation_locked(void) {}
static bool link_pairing_commit_provisioning(uint32_t generation, bool (*commit)(void)) {
    (void)generation; return commit();
}
static bool link_pairing_provisioning_session_valid(uint32_t generation) {
    (void)generation; return true;
}
static void identity_note_paired_sdk_token(void) {}
static const char *identity_node_id(void) { return "node"; }
static bool wifi_mgr_is_connected(void) { return true; }
static bool s_noise_running;
static bool noise_ctrl_is_running(void) { return s_noise_running; }
static void muse_keep_vm_session(void) {}

static SemaphoreHandle_t s_operation_gate = (SemaphoreHandle_t)1;
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

// Server behaviour per call: 0 ok, VM_API_ERR_REVOKED (a 401 on a refresh or
// mint), VM_API_ERR_AUTH (a 403 on them, or a 401/403 from fetch_vms), or
// VM_API_ERR_FAILED (network or 5xx).
static int s_fetch_rc, s_refresh_rc, s_mint_rc;
static int s_fetch_vm_count = 1;  // VMs a successful fetch lists
static int s_fetches, s_refreshes, s_mints, s_connects;
static const char *s_issued_access = "access-2";
static const char *s_issued_refresh = "refresh-2";

static char *dup(const char *s) {
    char *out = malloc(strlen(s) + 1);
    CHECK(out != NULL, "oom");
    memcpy(out, s, strlen(s) + 1);
    return out;
}
static char s_vm_url[] = "wss://vm.example/";
static char s_vm_name[] = "vm";
int vm_api_fetch_vms(const char *access, vm_info_t *out, int max) {
    (void)access;
    s_fetches++;
    if (s_fetch_rc != 0) return s_fetch_rc;
    if (s_fetch_vm_count == 0) return 0;
    CHECK(max > 0, "fetch max");
    memset(out, 0, sizeof(*out));
    out->vm_url = s_vm_url;
    out->vm_name = s_vm_name;
    out->is_default = true;
    return 1;
}
void vm_list_free(vm_info_t *vms, int count) { (void)vms; (void)count; }
const vm_info_t *vm_find_by_url(const vm_info_t *vms, int count, const char *url) {
    (void)vms; (void)count; (void)url; return NULL;
}
const vm_info_t *vm_find_default(const vm_info_t *vms, int count) {
    return count > 0 ? &vms[0] : NULL;
}
static bool connect_vm_target(const vm_info_t *target) {
    (void)target; s_connects++; return true;
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

#define CONFIG_MUSE_ENABLED MUSE_BUILD
#define CONFIG_HOMEHUB_BLE_PERSISTENT 0
#define CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS 30
#define CONFIG_HOMEHUB_WIFI_SSID ""

#include "app_defines.inc"

static SemaphoreHandle_t s_auth_lock = NULL;
static int64_t s_last_vm_auth_refresh_attempt_us = 0;
static int64_t s_last_vm_auth_refusal_us = 0;
static int64_t s_last_link_session_retry_us = 0;
static int64_t s_last_periodic_token_refresh_us = 0;
static int64_t s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
static uint8_t s_vm_auth_refusals = 0;
static bool s_vm_auth_refresh_pending = false;
static atomic_bool s_setup_reset_pending;
static atomic_bool s_unpair_pending;
static bool s_ble_started = false;
static const char *s_setup_stage = "idle";

#include "app_token_paths.inc"

// One heartbeat of app_run()'s loop: its deferred resets and, on Link
// builds, the paired-but-disconnected retry, verbatim.
static void app_tick(void) {
#include "app_heartbeat.inc"
}

static bool s_boot_setup_complete, s_boot_provisioned;
static void boot(void) {
    s_vm_auth_refusals = 0;
    s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_last_vm_auth_refresh_attempt_us = 0;
    s_last_vm_auth_refusal_us = 0;
    s_vm_auth_refresh_pending = false;
    atomic_store(&s_setup_reset_pending, false);
    atomic_store(&s_unpair_pending, false);
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

static void reset_counters(void) {
    s_fetches = s_refreshes = s_mints = s_connects = 0;
    s_windows_opened = s_restarts = s_disconnects = s_finished_resets = 0;
    s_ble_disconnects = s_tasks_started = 0;
}

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
    s_fetch_vm_count = 1;
    s_last_link_session_retry_us = 0;
    s_ble_connected = s_gate_busy = s_task_create_fails = false;
    s_noise_running = true;
    s_issued_access = "access-2";
    s_issued_refresh = "refresh-2";
    s_now += 1000000000LL;
    boot();
    CHECK(s_boot_setup_complete && !strcmp(s_setup_stage, "done"), "fixture not paired");
    reset_counters();
}

static void expect_wifi_kept(const char *when) {
    for (size_t i = 0; i < sizeof(WIFI_KEYS) / sizeof(WIFI_KEYS[0]); i++) {
        CHECK(has(WIFI_KEYS[i]), "%s: Wi-Fi key %s was erased", when, WIFI_KEYS[i]);
    }
    CHECK(!strcmp(value_of("password"), "wifi-secret"), "%s: Wi-Fi password changed", when);
}

static void expect_wifi_gone(const char *when) {
    for (size_t i = 0; i < sizeof(WIFI_KEYS) / sizeof(WIFI_KEYS[0]); i++) {
        CHECK(!has(WIFI_KEYS[i]), "%s: Wi-Fi key %s survived", when, WIFI_KEYS[i]);
    }
}

static void expect_pairing_kept(const char *when) {
    for (size_t i = 0; i < sizeof(PAIRING_KEYS) / sizeof(PAIRING_KEYS[0]); i++) {
        CHECK(has(PAIRING_KEYS[i]), "%s: pairing key %s was erased", when, PAIRING_KEYS[i]);
    }
    CHECK(!strcmp(value_of("refresh_token"), "refresh-1"), "%s: refresh token changed", when);
    CHECK(has("setup_complete"), "%s: setup marker cleared", when);
}

static void expect_pairing_gone(const char *when) {
    for (size_t i = 0; i < sizeof(PAIRING_KEYS) / sizeof(PAIRING_KEYS[0]); i++) {
        CHECK(!has(PAIRING_KEYS[i]), "%s: pairing key %s survived", when, PAIRING_KEYS[i]);
    }
    CHECK(!has("setup_complete"), "%s: setup marker survived", when);
}

// The VM fetch is refused, forcing a refresh the token service refuses with a
// 401: the credential is dead. Unpair, keep Wi-Fi, and stop asking.
static void revoked_refresh_unpairs_but_keeps_wifi(void) {
    paired_device();
    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_REVOKED;
    vm_info_t vms[1];
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1, "refresh not attempted");
    expect_pairing_gone("refresh revoked");
    expect_wifi_kept("refresh revoked");
    CHECK(atomic_load(&s_unpair_pending), "unpair not handed to the app task");
    CHECK(!atomic_load(&s_setup_reset_pending), "a revoked token asked for a factory reset");
    CHECK(!strcmp(s_status, "auth_failed"), "status=%s", s_status);

    app_tick();
    CHECK(s_disconnects == 1 && s_finished_resets == 1 && s_restarts == 1,
          "deferred unpair did not reset and restart");
    expect_pairing_gone("deferred unpair");
    expect_wifi_kept("deferred unpair");

    // Next boot: unpaired with Wi-Fi, pairing open, and nothing left to retry.
    reset_counters();
    boot();
    CHECK(!s_boot_setup_complete && !s_boot_provisioned, "revoked device still paired");
    CHECK(s_windows_opened == 1, "pairing not reopened");
    CHECK(!s_base_url[0] && !s_noise_host[0], "endpoints restored without pairing");
    expect_wifi_kept("reboot after revocation");
    for (int i = 0; i < 5; i++) {
        s_now += VM_AUTH_REFRESH_MAX_INTERVAL_US;
        CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_FAILED, "fetch");
        CHECK(!ensure_access_token_ready_with_gate_held(true, false), "ensure");
        s_noise_running = false;
        app_tick();
    }
    CHECK(s_refreshes == 0 && s_mints == 0 && s_fetches == 0 && s_restarts == 0,
          "a dead token kept being retried");

    // Re-pairing needs only the app: Wi-Fi is already there.
    s_ble_connected = true;
    CHECK(accept_pairing_credentials("access-new", "refresh-new", "user2"), "re-pair");
    CHECK(complete_setup_and_stop_ble("provision", 0), "complete");
    CHECK(has("setup_complete") && !strcmp(value_of("refresh_token"), "refresh-new"),
          "re-pair did not take");
    expect_wifi_kept("re-pair after revocation");
}

static void revoked_mint_unpairs_but_keeps_wifi(void) {
    // An access token stored without a refresh token is minted into a pair.
    paired_device();
    CHECK(config_erase_key("refresh_token"), "fixture");
    s_mint_rc = VM_API_ERR_REVOKED;
    CHECK(!ensure_access_token_ready_with_gate_held(false, true), "mint reported ok");
    CHECK(s_mints == 1 && !has("access_token") && !has("setup_complete"),
          "revoked mint source kept");
    expect_wifi_kept("mint revoked");
    CHECK(atomic_load(&s_unpair_pending), "unpair not handed to the app task");

    // A legacy auth_token is migrated the same way.
    paired_device();
    CHECK(config_erase_key("access_token") && config_erase_key("refresh_token"), "fixture");
    nvs_put("auth_token", "legacy");
    s_mint_rc = VM_API_ERR_REVOKED;
    CHECK(!ensure_access_token_ready_with_gate_held(false, true), "mint reported ok");
    CHECK(!has("auth_token") && !has("setup_complete"), "revoked legacy token kept");
    expect_wifi_kept("legacy mint revoked");
    CHECK(atomic_load(&s_unpair_pending), "unpair not handed to the app task");
}

// A 403 on the refresh does not prove the credential is dead: keep everything
// and back off, on the refusal's own clock.
static void refused_refresh_keeps_everything_and_backs_off(void) {
    paired_device();
    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_AUTH;
    vm_info_t vms[1];
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1, "refresh not attempted");
    expect_pairing_kept("refresh refused");
    expect_wifi_kept("refresh refused");
    CHECK(!atomic_load(&s_unpair_pending) && !atomic_load(&s_setup_reset_pending),
          "a 403 asked for a reset");
    CHECK(s_vm_auth_refusals == 1, "refusal not counted for backoff");
    app_tick();
    CHECK(s_restarts == 0 && s_disconnects == 0, "a 403 reset the device");

    // Inside the backoff (Muse reconnects at least every minute) the refresh
    // endpoint isn't asked again...
    s_now += 30 * 1000000LL;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1, "refresh not paced by the backoff");
    // ...and once it has passed, a second refusal widens it.
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    fetch_vms_with_refresh_with_gate_held(vms, 1);
    CHECK(s_refreshes == 2, "refresh not retried after the backoff");
    CHECK(s_vm_auth_refusals == 2
          && s_vm_auth_refresh_interval_us == 2 * VM_AUTH_REFRESH_RETRY_INTERVAL_US,
          "backoff did not widen");
    expect_pairing_kept("refused twice");

    // The service recovers: the token is accepted and the backoff resets.
    s_now += 2 * VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_refresh_rc = 0;
    s_fetch_rc = 0;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == 1, "fetch");
    CHECK(s_vm_auth_refusals == 0 && !s_last_vm_auth_refusal_us, "backoff not reset");
    expect_wifi_kept("recovered");
}

static void transient_failures_change_nothing(void) {
    paired_device();
    vm_info_t vms[1];
    s_fetch_rc = VM_API_ERR_FAILED;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_FAILED, "fetch rc");
    CHECK(s_refreshes == 0, "a network failure must not force a refresh");

    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_FAILED;
    CHECK(fetch_vms_with_refresh_with_gate_held(vms, 1) == VM_API_ERR_AUTH, "fetch rc");
    CHECK(s_refreshes == 1, "refresh not attempted");
    CHECK(s_vm_auth_refusals == 0, "a 5xx counted as a refusal");
    CHECK(!atomic_load(&s_unpair_pending), "a 5xx asked for an unpair");
    expect_pairing_kept("refresh network failure");
    expect_wifi_kept("refresh network failure");

    // The heartbeat's proactive refresh keeps the working token whatever the
    // refresh says, a 401 included: nothing has refused the access token.
    int results[] = {VM_API_ERR_AUTH, VM_API_ERR_REVOKED, VM_API_ERR_FAILED};
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        s_now += (int64_t)CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS * 24 * 3600 * 1000000LL;
        s_refresh_rc = results[i];
        int before = s_refreshes;
        CHECK(ensure_access_token_ready_with_gate_held(false, true), "stale token not kept");
        CHECK(s_refreshes == before + 1, "proactive refresh not attempted");
        CHECK(!atomic_load(&s_unpair_pending), "proactive refresh asked for an unpair");
        expect_pairing_kept("proactive refresh refused");
    }
}

static void refused_mint_keeps_its_token_and_backs_off(void) {
    paired_device();
    CHECK(config_erase_key("refresh_token"), "fixture");
    s_mint_rc = VM_API_ERR_AUTH;
    CHECK(!ensure_access_token_ready_with_gate_held(false, true), "mint reported ok");
    CHECK(s_mints == 1 && has("access_token") && has("setup_complete"),
          "a 403 mint erased its source token");
    CHECK(!atomic_load(&s_unpair_pending), "a 403 mint asked for an unpair");
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

// The refresh scheduler (the tunnel's auth-failure path, and Link's retry
// while disconnected) stamps its attempt just before its worker runs. The
// refusal backoff must not read that stamp as a refusal, or every scheduled
// refresh would skip itself once one refusal had been counted.
static void scheduled_refresh_is_not_blocked_by_its_own_stamp(void) {
    paired_device();
    // The refresh succeeds but returns a token every endpoint rejects.
    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = 0;
    schedule_vm_auth_refresh();
    CHECK(s_tasks_started == 1 && s_fetches == 2 && s_refreshes == 1,
          "first scheduled refresh: tasks=%d fetches=%d refreshes=%d",
          s_tasks_started, s_fetches, s_refreshes);
    CHECK(s_vm_auth_refusals == 1, "dead token not counted");

    // Too soon: the scheduler's floor holds it back.
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US / 2;
    schedule_vm_auth_refresh();
    CHECK(s_tasks_started == 1, "refresh not paced");

    // Once the floor has passed, the worker refreshes again.
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    schedule_vm_auth_refresh();
    CHECK(s_tasks_started == 2, "refresh not scheduled after the floor");
    CHECK(s_refreshes == 2, "scheduled refresh skipped by its own attempt stamp");
    CHECK(s_vm_auth_refusals == 2, "second dead token not counted");

    // The token service mends: the next scheduled refresh recovers.
    s_now += 2 * VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_issued_access = "access-3";
    int fetches = s_fetches;
    s_fetch_rc = 0;
    schedule_vm_auth_refresh();
    CHECK(s_tasks_started == 3 && s_fetches == fetches + 1 && s_connects == 1,
          "recovered fetch did not connect");
    CHECK(s_vm_auth_refusals == 0, "backoff not reset");
    expect_wifi_kept("scheduled refresh");
}

// The same through a 403: refused, then refused again once the backoff ends,
// then accepted, all driven by schedule_vm_auth_refresh().
static void scheduled_refresh_retries_a_refusal_after_its_backoff(void) {
    paired_device();
    s_fetch_rc = VM_API_ERR_AUTH;
    s_refresh_rc = VM_API_ERR_AUTH;
    schedule_vm_auth_refresh();
    CHECK(s_refreshes == 1 && s_vm_auth_refusals == 1, "first refusal");
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    schedule_vm_auth_refresh();
    CHECK(s_tasks_started == 2 && s_refreshes == 2, "refusal not retried after its backoff");
    CHECK(s_vm_auth_refusals == 2, "second refusal not counted");
    s_now += s_vm_auth_refresh_interval_us;
    s_refresh_rc = 0;
    s_fetch_rc = 0;
    schedule_vm_auth_refresh();
    CHECK(s_tasks_started == 3 && s_connects == 1 && s_vm_auth_refusals == 0, "recovery");
    expect_wifi_kept("scheduled 403");
}

// Link builds retry a paired device's VM session from the heartbeat when it
// isn't running (Muse keeps its own session), paced by the scheduler.
static void link_heartbeat_retries_a_disconnected_pairing(void) {
    paired_device();
    s_noise_running = false;
    s_fetch_rc = VM_API_ERR_FAILED;
    app_tick();
    app_tick();
#if MUSE_BUILD
    CHECK(s_tasks_started == 0, "Muse heartbeat scheduled a Link refresh");
#else
    CHECK(s_tasks_started == 1 && s_fetches == 1, "heartbeat retry: tasks=%d fetches=%d",
          s_tasks_started, s_fetches);
    // A fetch that succeeds but lists no VM accepts the credentials, which
    // clears the scheduler's floor: the retry keeps its own.
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_fetch_rc = 0;
    s_fetch_vm_count = 0;
    app_tick();
    CHECK(s_tasks_started == 2 && s_fetches == 2 && s_connects == 0, "empty list retry");
    for (int i = 0; i < 6; i++) {
        s_now += 5 * 1000000LL;
        app_tick();
    }
    CHECK(s_fetches == 2, "heartbeat refetched every tick after an empty list: %d", s_fetches);
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_fetch_vm_count = 1;
    app_tick();
    CHECK(s_tasks_started == 3 && s_connects == 1, "heartbeat retry did not reconnect");
    // Not while unpaired: nothing to retry.
    CHECK(clear_setup_credentials(SETUP_RESET_KEEP_WIFI), "unpair");
    s_now += VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    app_tick();
    CHECK(s_tasks_started == 3, "heartbeat retried an unpaired device");
#endif
}

static void explicit_unpairs_keep_wifi(void) {
    paired_device();
    on_unpair();
    CHECK(s_disconnects == 1 && s_finished_resets == 1, "BLE unpair did not reset");
    expect_pairing_gone("BLE unpair");
    expect_wifi_kept("BLE unpair");

    paired_device();
    ws_unpaired_task(NULL);
    CHECK(s_restarts == 1, "node.unpaired did not restart");
    expect_pairing_gone("node.unpaired");
    expect_wifi_kept("node.unpaired");

    // device.unpair clears before acknowledging, through the same helper.
    paired_device();
    CHECK(clear_setup_credentials(SETUP_RESET_KEEP_WIFI), "server unpair pre-clear");
    expect_pairing_gone("device.unpair");
    expect_wifi_kept("device.unpair");

    // Setup failure cleanup uses the same scope: a failed re-pair keeps Wi-Fi.
    paired_device();
    CHECK(setup_wipe_to_clean(SETUP_RESET_KEEP_WIFI), "setup failure cleanup");
    expect_pairing_gone("setup failure");
    expect_wifi_kept("setup failure");
}

// The user's own factory reset on the device forgets Wi-Fi too.
static void user_factory_reset_forgets_wifi(void) {
    paired_device();
    on_button_long_press();
    CHECK(s_restarts == 1 && s_finished_resets == 1, "5 s hold did not reset");
    expect_pairing_gone("5 s hold");
    expect_wifi_gone("5 s hold");

    paired_device();
    app_reset_setup_async();
    CHECK(s_restarts == 1, "Muse menu reset did not restart");
    expect_pairing_gone("Muse menu reset");
    expect_wifi_gone("Muse menu reset");

    // Deferred to the app task when its own task can't start.
    paired_device();
    s_task_create_fails = true;
    app_reset_setup_async();
    CHECK(atomic_load(&s_setup_reset_pending) && has("ssid"), "reset not deferred");
    s_task_create_fails = false;
    app_tick();
    CHECK(s_restarts == 1, "deferred reset did not restart");
    expect_pairing_gone("deferred Muse menu reset");
    expect_wifi_gone("deferred Muse menu reset");

    // A factory reset queued beside an unpair wins.
    paired_device();
    atomic_store(&s_unpair_pending, true);
    atomic_store(&s_setup_reset_pending, true);
    app_tick();
    CHECK(s_restarts == 1 && !atomic_load(&s_unpair_pending), "both resets ran");
    expect_wifi_gone("factory reset beside an unpair");

    // After it, the device boots unprovisioned.
    reset_counters();
    boot();
    CHECK(!s_boot_setup_complete && !s_boot_provisioned && s_windows_opened == 1,
          "factory reset boot state");
}

static void reboot_handles_each_state(void) {
    // A legacy pair (tokens and Wi-Fi, no marker) is marked complete.
    paired_device();
    CHECK(config_clear_setup_complete(), "fixture");
    boot();
    CHECK(s_boot_setup_complete && has("setup_complete") && s_boot_provisioned,
          "legacy pair not migrated");
    CHECK(!strcmp(s_base_url, "https://api.example") && !strcmp(s_noise_host, "noise.example"),
          "legacy pair lost its endpoints");
    expect_pairing_kept("legacy pair");

    // After an unpair: Wi-Fi only. Boot keeps it and opens pairing.
    paired_device();
    CHECK(clear_setup_credentials(SETUP_RESET_KEEP_WIFI), "unpair");
    boot();
    CHECK(!s_boot_setup_complete && !s_boot_provisioned && s_windows_opened == 1,
          "unpaired boot state");
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

static void revoked_mint_while_pairing_leaves_cleanup_to_setup_failure(void) {
    paired_device();
    CHECK(clear_setup_credentials(SETUP_RESET_KEEP_WIFI), "unpair");
    s_mint_rc = VM_API_ERR_REVOKED;
    s_setup_stage = "auth";
    CHECK(!accept_pairing_credentials("sdk-token", NULL, "user2"), "mint refused");
    CHECK(!atomic_load(&s_unpair_pending), "pairing session asked for an unpair");
    CHECK(setup_wipe_to_clean(SETUP_RESET_KEEP_WIFI), "setup failure");
    expect_wifi_kept("refused mint while pairing");
}

int main(void) {
    // Fakes only one build profile's code paths reach.
    (void)ble_server_stop_advertising;
    (void)ble_server_set_companion_advertising;
    (void)ble_coex_set_active;
    (void)ble_server_full_shutdown;
    (void)muse_keep_vm_session;
    (void)noise_ctrl_is_running;
    revoked_refresh_unpairs_but_keeps_wifi();
    revoked_mint_unpairs_but_keeps_wifi();
    refused_refresh_keeps_everything_and_backs_off();
    transient_failures_change_nothing();
    refused_mint_keeps_its_token_and_backs_off();
    scheduled_refresh_is_not_blocked_by_its_own_stamp();
    scheduled_refresh_retries_a_refusal_after_its_backoff();
    link_heartbeat_retries_a_disconnected_pairing();
    explicit_unpairs_keep_wifi();
    user_factory_reset_forgets_wifi();
    reboot_handles_each_state();
    revoked_mint_while_pairing_leaves_cleanup_to_setup_failure();
    puts("token refusal harness: PASS");
    return 0;
}
