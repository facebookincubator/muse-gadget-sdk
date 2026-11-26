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
#include "ble_server.h"
#include "adv_pace.h"
#include "muse_local_press.h"
#include "muse_wdt.h"
#include "stack_monitor.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_hosted_misc.h"
#else
#include "esp_bt.h"
#endif
#include "esp_app_desc.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nvs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cJSON.h"
#include "config_store.h"
#include "link_pairing.h"
#include "resetup_gap.h"
#include "app.h"
#include "factory_test.h"
#if CONFIG_MUSE_GATT_DBHASH
#include "gatt_dbhash.h"
#endif
#include "helper_task.h"
#include "muse_server_hosts.h"

static const char *TAG = "link.ble";

// Service UUID 7fdd3d1c-38ea-46cf-8b46-314ecf5f240c (LE byte order)
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x0c, 0x24, 0x5f, 0xcf, 0x4e, 0x31, 0x46, 0x8b,
    0xcf, 0x46, 0xea, 0x38, 0x1c, 0x3d, 0xdd, 0x7f);

// RX UUID 4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x01, 0x9b, 0x8f, 0x5e, 0x2d, 0x3c, 0xf0, 0xa1,
    0x6e, 0x4a, 0xa2, 0x28, 0x29, 0x30, 0x59, 0x4d);

// TX UUID d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0a, 0x8f,
    0x9c, 0x4e, 0x2b, 0x7b, 0xca, 0xc4, 0x5d, 0xd7);

// Link setup's manufacturer data: the test company id 0xFFFF, then one
// flags byte (protocols/README.md, Gadget setup v2). Firmware without
// gadget support sends 0x00 or 0x01, as it always has.
#define LINK_ADV_SET_UP     0x01   // setup is complete
#define LINK_ADV_BLE_GADGET 0x02   // a Muse Gadget over Bluetooth: serves musegadgets, needs no Wi-Fi

#define MAX_NOTIFY_CHUNK   160
#define CHUNK_HEADER_BYTES 3
#define CHUNK_MAGIC        0xFE
#define MAX_RX_TOTAL_BYTES 8192

static ble_callbacks_t s_cb = {0};
static const char *s_device_name = NULL;

static uint16_t s_rx_handle = 0;
static uint16_t s_tx_handle = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#if CONFIG_MUSE_GADGET_BLE_AUDIO   /* a gadget's re-setup only (resetup_gap.h) */
static volatile uint32_t s_conn_seq;   /* every connection: the re-setup prompt's per-connection gap */
#endif
static bool s_subscribed = false;
static bool s_started = false;
static bool s_gatt_failed;   // a service didn't register: never advertise
static volatile bool s_ever_reachable;   // advertised or connected this boot (the boot guard's health)
static bool s_bench_silent;   // ble_server_bench_silence: not paused on purpose, so unreachable
// Bench (>ble.pause=SECS): no link and no advertising until this time (0: not paused).
static volatile int64_t s_bench_pause_until_us;
// ...or (>ble.pause=SECS,ADDR) advertising on, connectable only from that address (the accept list).
static bool s_bench_pause_only;
static ble_addr_t s_bench_pause_peer[2];   // the address as public and as random static: either type
static struct ble_npl_callout s_bench_pause_co;
static bool s_bench_pause_co_ready;
// The companion's advertising interval, fast for 30 s after a trigger,
// then 417.5 ms (adv_pace.h). s_adv_paced: the running advertisement is the
// paced one (the companion's, set up), at s_adv_paced_fast.
static adv_pace_t s_adv_pace;
static bool s_adv_paced, s_adv_paced_fast;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
static struct ble_npl_callout s_adv_pace_co;
static bool s_adv_pace_co_ready, s_adv_pace_logged_fast;
#endif
static void adv_pace_fast_now(const char *why);
// SAFE mode can't serve Muse, so it advertises as someone else: a static
// random address derived from the public one, the name
// "SAFE-<product>-<tail>" and no service UUIDs, so neither a phone's saved
// reconnect (by address or identifier) nor a scan for the musegadgets or Link
// setup UUID lands on it. The GATT table is unchanged: the bench recovers it
// over the Nordic UART console (safe.off) and SMP, found by name.
static bool s_safe_public;     // SAFE on the public address (both random addresses refused): SAFE name, no UUIDs, Muse GATT refused
static bool s_stall_quiet;     // a companion's protocol is stalled: the normal identity doesn't advertise
static bool s_safe_identity;   // advertising as SAFE's identity (above)
static bool s_shutting_down = false;
static bool s_synced = false;
static bool s_advertising_enabled = false;
#if CONFIG_HOMEHUB_BLE_PERSISTENT
static int s_companion_interval_ms = CONFIG_HOMEHUB_BLE_PERSISTENT_ADV_INTERVAL_MS;
#endif
static bool s_advertising_active = false;
// What the running advertisement carries (see advertising_wanted()).
static bool s_adv_link = false;
static const ble_companion_t *s_adv_other = NULL;
static bool s_plaintext_status_blocked = false;
static void send_hello_answer_plaintext(const char *status);   /* the re-setup rate's answer */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
static resetup_gap_t s_prompt_gap;   /* the re-setup prompt's connection and last dismiss (resetup_gap.h) */
#endif
#if CONFIG_MUSE_GADGET_TEST_BUILD || CONFIG_MUSE_GADGET_AUTOMATION
#define RESETUP_EXEMPT true   /* test and bench builds: exempt from the dismiss rule only */
#else
#define RESETUP_EXEMPT false
#endif
#define MAX_COMPANIONS 2
static ble_companion_t s_companions[MAX_COMPANIONS];
static int s_ncompanions = 0;
static bool s_companion_advertising = false;
static volatile bool s_quiet = false;     // ble_server_set_quiet
// While Link's own advertising and a companion's UUID are both wanted, the
// payload alternates between them every ADV_TURN_MS (legacy advertising fits
// one 128-bit UUID).
#define ADV_TURN_MS 1500
static bool s_adv_turn = false;

static SemaphoreHandle_t s_tx_mutex = NULL;
static SemaphoreHandle_t s_rx_mutex = NULL;
static char s_last_status[64] = "idle";
static uint16_t s_mtu = 23;  // BLE default; bumped via BLE_GAP_EVENT_MTU

// RX reassembly buffer (single in-flight message)
static uint8_t *s_rx_buf = NULL;
static size_t s_rx_len = 0;
static size_t s_rx_cap = 0;
static uint8_t s_rx_total = 0;
static uint8_t s_rx_count = 0;
static uint8_t s_rx_next_idx = 0;

static void start_advertising(void);
static const ble_companion_t *advertised_companion(void);

// ---- Worker tasks for command dispatch -------------------------------------

static atomic_bool s_device_info_pending = ATOMIC_VAR_INIT(false);

typedef struct {
    char *ssid;
    char *password;
    char *access_token;
    char *refresh_token;
    char *username;
    char *ota_url;
    bool ota_force;
    char *api_url;
    char *api_url_v2;
    char *noise_host;
    bool proxy_present;
    network_proxy_config_t proxy;
    uint32_t session_generation;
} provision_args_t;

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

static void secure_free_str(char *s) {
    if (!s) return;
    mbedtls_platform_zeroize(s, strlen(s));
    free(s);
}

static void wipe_json_strings(cJSON *item) {
    for (cJSON *cur = item; cur; cur = cur->next) {
        if (cur->valuestring) {
            mbedtls_platform_zeroize(cur->valuestring, strlen(cur->valuestring));
        }
        if (cur->child) wipe_json_strings(cur->child);
    }
}

static void delete_command_json(cJSON *root, bool decrypted) {
    if (decrypted) wipe_json_strings(root);
    cJSON_Delete(root);
}

static void provision_task(void *arg) {
    provision_args_t *a = (provision_args_t *)arg;
    if (s_cb.on_provision
        && link_pairing_provisioning_session_valid(a->session_generation)) {
        s_cb.on_provision(a->ssid ? a->ssid : "",
                          a->password ? a->password : "",
                          a->access_token ? a->access_token : "",
                          a->refresh_token ? a->refresh_token : "",
                          a->username ? a->username : "",
                          a->ota_url ? a->ota_url : "",
                          a->ota_force,
                          a->api_url ? a->api_url : "",
                          a->api_url_v2 ? a->api_url_v2 : "",
                          a->noise_host ? a->noise_host : "",
                          a->proxy_present ? &a->proxy : NULL,
                          a->session_generation);
    }
    secure_free_str(a->ssid);
    secure_free_str(a->password);
    secure_free_str(a->access_token);
    secure_free_str(a->refresh_token);
    secure_free_str(a->username);
    secure_free_str(a->ota_url);
    secure_free_str(a->api_url);
    secure_free_str(a->api_url_v2);
    secure_free_str(a->noise_host);
    network_proxy_wipe(&a->proxy, sizeof(a->proxy));
    free(a);
    stack_monitor_record(NULL);
    helper_task_delete(NULL);
}

static void scan_task(void *arg) {
    if (s_cb.on_wifi_scan) s_cb.on_wifi_scan();
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

typedef struct {
    char *url;
    bool force;
} ota_args_t;

static void ble_ota_task(void *arg) {
    ota_args_t *a = (ota_args_t *)arg;
    if (s_cb.on_ota) {
        s_cb.on_ota(a->url ? a->url : "", a->force);
    }
    free(a->url);
    free(a);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static const char *optional_ota_url(cJSON *root) {
    cJSON *u = cJSON_GetObjectItem(root, "ota_url");
    if (!cJSON_IsString(u)) {
        u = cJSON_GetObjectItem(root, "url");
    }
    return (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "";
}

static bool optional_ota_force(cJSON *root) {
    cJSON *f = cJSON_GetObjectItem(root, "ota_force");
    if (!cJSON_IsBool(f)) {
        f = cJSON_GetObjectItem(root, "force");
    }
    return cJSON_IsTrue(f);
}

static const char *optional_str(cJSON *root, const char *key) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

// Setup may name the hosts the device sends its credentials to; a host this
// build doesn't allow (muse_server_hosts.h) refuses the whole command before
// any of it is applied.
static bool setup_hosts_allowed(cJSON *root) {
    return muse_server_api_allowed(optional_str(root, "api_url"))
           && muse_server_api_allowed(optional_str(root, "api_url_v2"))
           && muse_server_noise_allowed(optional_str(root, "noise_host"));
}

static void unpair_task(void *arg) {
    if (s_cb.on_unpair) s_cb.on_unpair();
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void device_info_task(void *arg) {
    if (s_cb.on_get_device_info) s_cb.on_get_device_info();
    atomic_store(&s_device_info_pending, false);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void client_connected_task(void *arg) {
    if (s_cb.on_client_connected) s_cb.on_client_connected();
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void pairing_client_finished_task(void *arg) {
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    if (link_pairing_session_is_current(generation) && s_cb.on_pairing_client_finished) {
        s_cb.on_pairing_client_finished(generation);
    }
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void delayed_disconnect_task(void *arg) {
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "delayed disconnect after %lu ms", (unsigned long)ms);
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

void ble_server_delayed_disconnect(uint32_t ms) {
    xTaskCreate(delayed_disconnect_task, "ble_dc",
                2048, (void *)(uintptr_t)ms, 4, NULL);
}

// ---- Command dispatch ------------------------------------------------------

#if CONFIG_MUSE_GADGET_BLE_STANDALONE
#define PROVISION_STACK 4096
#else
#define PROVISION_STACK 8192
#endif

static bool is_sensitive_setup_action(const char *act) {
    return strcmp(act, "provision") == 0
           || strcmp(act, "provision_v2") == 0
           || strcmp(act, "wifi_scan") == 0
           || strcmp(act, "ota") == 0
           || strcmp(act, "device.ota") == 0
           || strcmp(act, "unpair") == 0
           || strcmp(act, "set_wifi") == 0
           || strcmp(act, "set_auth") == 0;
}

static bool is_exact_client_finished(cJSON *root) {
    cJSON *only = cJSON_IsObject(root) ? root->child : NULL;
    return only && !only->next
           && only->string && strcmp(only->string, "action") == 0
           && cJSON_IsString(only) && only->valuestring
           && strcmp(only->valuestring, "pairing_client_finished") == 0;
}

static bool command_json_depth_valid(const uint8_t *data, size_t len) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 32) return false;
        } else if ((c == '}' || c == ']') && depth) {
            depth--;
        }
    }
    return true;
}

static void dispatch_command_ex(const uint8_t *data, size_t len, bool decrypted) {
    if (!command_json_depth_valid(data, len)) {
        ble_server_send_status("error_invalid_command");
        return;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!root) {
        ESP_LOGW(TAG, "RX invalid JSON (%d bytes)", (int)len);
        ble_server_send_status("error_invalid_command");
        return;
    }
    cJSON *action = cJSON_GetObjectItem(root, "action");
    const char *act = (cJSON_IsString(action) && action->valuestring) ? action->valuestring : "";
    ESP_LOGI(TAG, "RX action: %s", act);

    if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && config_setup_complete()
        && !link_pairing_mgcommands()) {
        // BLE outlives setup only for a companion service (Muse phone setup).
        // Re-pairing goes through unpair/reset, never a new session here.
        // A gadget takes a new setup any time instead (Gadget setup v2,
        // re-provisioning): the same handshake and press, and its commit
        // replaces the old tokens and proof key (app.c).
        ble_server_send_status("error_pairing_unavailable");
#if CONFIG_MUSE_GADGET_BLE_AUDIO   // a gadget's re-setup: without it, a set-up device took the branch above
    } else if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && config_setup_complete()
               && !app_resetup_open()) {
        /* A set-up gadget whose only key is its talk key takes a re-setup only in its setup mode, and none in
         * SAFE: nothing shows. */
        ESP_LOGW(TAG, "pairing_client_hello: a re-setup outside setup mode (press the key at power-on): refused");
        send_hello_answer_plaintext("error_pairing_unavailable");
    } else if (!decrypted && strcmp(act, "pairing_client_hello") == 0
               && resetup_hello(&s_prompt_gap, RESETUP_EXEMPT, config_setup_complete(), s_conn_seq,
                                esp_timer_get_time()) == RESETUP_BUSY) {
        /* Another connection's hello while the prompt is up, or this connection's within 10 s of its dismiss:
         * nothing shows. */
        ESP_LOGW(TAG, "pairing_client_hello: the re-setup prompt is busy (another connection's, or dismissed in "
                      "the last 10 s on this one)");
        send_hello_answer_plaintext("error_operation_in_progress");
#endif
    } else if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
        uint32_t replaced = link_pairing_session_generation();   /* the session this hello starts over from */
        char *reply = NULL;
        const char *err = link_pairing_handle_client_hello(root, &reply);
        if (err && replaced) {
            /* A hello that fails on a link with a session (a takeover's included): it's answered, under that
             * session's keys while they're there (refused on its own fields or keys, before any wipe), else in
             * plaintext (failed after the wipe); then the session ends at once, and a prompt up for it with the
             * link, as a dismissed one's does. */
            ESP_LOGW(TAG, "pairing_client_hello failed with a session up: it ends, and the link drops");
            if (link_pairing_session_is_current(replaced)) {
                ble_server_send_status(err);
                (void)link_pairing_reset_session(replaced);
            } else {
                send_hello_answer_plaintext(err);
            }
            ble_server_delayed_disconnect(300);
        } else if (err) {
            ble_server_send_status(err);
        } else if (reply) {
            s_plaintext_status_blocked = true;
            ble_server_send_chunked(reply);
            free(reply);
        }
    } else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {
        char *plain = NULL;
        const char *err = link_pairing_decrypt_command(root, &plain);
        if (err) {
            ble_server_send_status(err);
            ble_server_delayed_disconnect(300);
        } else if (plain) {
            dispatch_command_ex((const uint8_t *)plain, strlen(plain), true);
            mbedtls_platform_zeroize(plain, strlen(plain));
            free(plain);
        }
    } else if (!decrypted && s_plaintext_status_blocked) {
        ESP_LOGW(TAG, "plaintext command suppressed after pairing ready: %s", act);
    } else if (!decrypted && is_sensitive_setup_action(act)
               && link_pairing_plaintext_setup_blocked()) {
        ble_server_send_status("error_encryption_required");
    } else if (decrypted && strcmp(act, "pairing_client_finished") == 0) {
        uint32_t generation = is_exact_client_finished(root)
            ? link_pairing_handle_client_finished() : 0;
        if (generation) {
            if (xTaskCreate(pairing_client_finished_task, "pair_confirm", 4096,
                            (void *)(uintptr_t)generation, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "failed to dispatch pairing-confirm callback");
                ble_server_send_pairing_status("error_pairing_unavailable", generation);
                ble_server_disconnect_pairing_session(generation);
            }
        } else {
            ble_server_send_status("error_pairing_decrypt");
            link_pairing_reset();
            ble_server_delayed_disconnect(300);
        }
    } else if (decrypted && is_sensitive_setup_action(act)
               && !link_pairing_session_confirmed()) {
        ble_server_send_status("error_pairing_confirm_required");
    } else if (decrypted && strcmp(act, "wifi_scan") == 0) {
        if (xTaskCreate(scan_task, "scan", 4096, NULL, 5, NULL) != pdPASS) {
            ble_server_send_status("error_operation_in_progress");
        }
    } else if (decrypted && strcmp(act, "device.ota") == 0) {
        {
            cJSON *u = cJSON_GetObjectItem(root, "url");
            cJSON *f = cJSON_GetObjectItem(root, "force");
            const char *url = (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "";
            if (!*url) {
                ble_server_send_status("error_missing_url");
            } else {
                ota_args_t *a = calloc(1, sizeof(*a));
                if (!a) {
                    ble_server_send_status("error_operation_in_progress");
                    delete_command_json(root, decrypted);
                    return;
                }
                a->url = dup_str(url);
                a->force = cJSON_IsTrue(f);
                if (!a->url) {
                    free(a);
                    ble_server_send_status("error_operation_in_progress");
                    delete_command_json(root, decrypted);
                    return;
                }
                if (xTaskCreate(ble_ota_task, "ble_ota", 4096, a, 5, NULL) != pdPASS) {
                    free(a->url);
                    free(a);
                    ble_server_send_status("error_operation_in_progress");
                }
            }
        }
    } else if (decrypted && strcmp(act, "unpair") == 0) {
        if (xTaskCreate(unpair_task, "unpair", 4096, NULL, 5, NULL) != pdPASS) {
            ble_server_send_status("error_operation_in_progress");
        }
    } else if (decrypted && strcmp(act, "provision_v2") != 0) {
        ble_server_send_status("error_unknown_action");
    } else if (decrypted && strcmp(act, "provision_v2") == 0) {
        cJSON *s = cJSON_GetObjectItem(root, "ssid");
        cJSON *p = cJSON_GetObjectItem(root, "password");
        cJSON *at = cJSON_GetObjectItem(root, "access_token");
        cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
        cJSON *u = cJSON_GetObjectItem(root, "username");
        cJSON *tt = cJSON_GetObjectItem(root, "token_type");
        const char *ota_url = optional_ota_url(root);
        if (*ota_url && config_setup_complete()) {
            // Setting a set-up gadget up again never takes an image to install.
            ESP_LOGW(TAG, "provision on a set-up gadget: ota_url ignored");
            ota_url = "";
        }
        link_wifi_mode_t wifi = link_pairing_wifi_mode();
        bool tokens_ok = cJSON_IsString(at) && at->valuestring && *at->valuestring
                         && cJSON_IsString(rt) && rt->valuestring && *rt->valuestring
                         && cJSON_IsString(tt) && tt->valuestring
                         && strcmp(tt->valuestring, "device") == 0;
        bool wifi_ok = cJSON_IsString(s) && cJSON_IsString(p)
                       && s->valuestring && p->valuestring
                       && *s->valuestring && *p->valuestring;
        // A gadget whose Wi-Fi is optional or absent provisions tokens only
        // when ssid and password are both absent or empty.
        bool token_only = wifi != LINK_WIFI_REQUIRED
                          && (!s || (cJSON_IsString(s) && s->valuestring && !*s->valuestring))
                          && (!p || (cJSON_IsString(p) && p->valuestring && !*p->valuestring));
        if (tokens_ok && wifi == LINK_WIFI_NONE && !token_only) {
            ble_server_send_status("error_wifi_unsupported");
        } else if (!tokens_ok || (!token_only && !wifi_ok)) {
            ble_server_send_status("error_missing_credentials");
        } else if (!setup_hosts_allowed(root)) {
            ble_server_send_status("error_invalid_host");
        } else {
            provision_args_t *a = calloc(1, sizeof(*a));
            if (!a) {
                ble_server_send_status("error_operation_in_progress");
                delete_command_json(root, decrypted);
                return;
            }
            cJSON *proxy = cJSON_GetObjectItemCaseSensitive(root, "proxy");
            a->proxy_present = proxy != NULL;
            if (!network_proxy_parse(proxy, &a->proxy)) {
                free(a);
                ble_server_send_status("error_invalid_proxy");
                delete_command_json(root, decrypted);
                return;
            }
            // Token-only: no ssid reaches on_provision, which skips Wi-Fi.
            if (!token_only) {
                a->ssid = dup_str(s->valuestring);
                a->password = dup_str(p->valuestring);
            }
            a->access_token = dup_str(at->valuestring);
            a->refresh_token = dup_str(rt->valuestring);
            if (wifi != LINK_WIFI_NONE && cJSON_IsString(u) && u->valuestring) {
                a->username = dup_str(u->valuestring);
            }
            if (*ota_url && !token_only) {
                a->ota_url = dup_str(ota_url);
                a->ota_force = optional_ota_force(root);
            }
            // On a gadget an empty ssid means token-only, so out of memory
            // must not turn a Wi-Fi request into one.
            if (wifi != LINK_WIFI_REQUIRED
                && (!a->access_token || !a->refresh_token
                    || (!token_only && (!a->ssid || !a->password)))) {
                secure_free_str(a->ssid);
                secure_free_str(a->password);
                secure_free_str(a->access_token);
                secure_free_str(a->refresh_token);
                secure_free_str(a->username);
                secure_free_str(a->ota_url);
                network_proxy_wipe(&a->proxy, sizeof(a->proxy));
                free(a);
                ble_server_send_status("error_operation_in_progress");
                delete_command_json(root, decrypted);
                return;
            }
            if (*ota_url && !token_only) {
                if (!a->ota_url) {
                    secure_free_str(a->ssid);
                    secure_free_str(a->password);
                    secure_free_str(a->access_token);
                    secure_free_str(a->refresh_token);
                    secure_free_str(a->username);
                    network_proxy_wipe(&a->proxy, sizeof(a->proxy));
                    free(a);
                    ble_server_send_status("error_operation_in_progress");
                    delete_command_json(root, decrypted);
                    return;
                }
            }
            // Endpoints let a device that joins Wi-Fi later reach Muse; a
            // device without Wi-Fi has no use for them.
            cJSON *au = cJSON_GetObjectItem(root, "api_url");
            if (wifi != LINK_WIFI_NONE && cJSON_IsString(au) && au->valuestring && *au->valuestring) {
                a->api_url = dup_str(au->valuestring);
            }
            // Older firmware ignores api_url_v2 and keeps reading api_url.
            cJSON *au2 = cJSON_GetObjectItem(root, "api_url_v2");
            if (wifi != LINK_WIFI_NONE && cJSON_IsString(au2) && au2->valuestring && *au2->valuestring) {
                a->api_url_v2 = dup_str(au2->valuestring);
            }
            cJSON *nh = cJSON_GetObjectItem(root, "noise_host");
            if (wifi != LINK_WIFI_NONE && cJSON_IsString(nh) && nh->valuestring && *nh->valuestring) {
                a->noise_host = dup_str(nh->valuestring);
            }
            // 8 KB stack — mbedtls 3.6 (IDF v6) needs significantly more
            // stack during TLS handshake than v5's 3.x (~2 KB more peak).
            // A BLE-only gadget's provisioning is token-only (no TLS): it
            // peaks at 2.4 KB (StopWatch), so 4 KB of internal RAM.
            a->session_generation = link_pairing_mark_provisioning_active();
            if (a->session_generation == 0
                || helper_task_create(provision_task, "prov", PROVISION_STACK, a, 5, NULL) != pdPASS) {
                uint32_t generation = a->session_generation;
                secure_free_str(a->ssid);
                secure_free_str(a->password);
                secure_free_str(a->access_token);
                secure_free_str(a->refresh_token);
                secure_free_str(a->username);
                secure_free_str(a->ota_url);
                secure_free_str(a->api_url);
                secure_free_str(a->api_url_v2);
                secure_free_str(a->noise_host);
                network_proxy_wipe(&a->proxy, sizeof(a->proxy));
                free(a);
                ble_server_send_pairing_status("error_operation_in_progress", generation);
                ble_server_disconnect_pairing_session(generation);
            }
        }
    } else if (strcmp(act, "get_device_info") == 0) {
        if (!atomic_exchange(&s_device_info_pending, true)) {
            if (xTaskCreate(device_info_task, "devinfo", 4096, NULL, 5, NULL) != pdPASS) {
                atomic_store(&s_device_info_pending, false);
                ble_server_send_status("error_operation_in_progress");
            }
        }
    } else if (strcmp(act, "get_version") == 0) {
        const esp_app_desc_t *desc = esp_app_get_description();
        char ver[80];
        snprintf(ver, sizeof(ver), "version:%s", desc ? desc->version : "unknown");
        ble_server_send_status(ver);
    } else {
        ble_server_send_status("error_unknown_action");
    }
    delete_command_json(root, decrypted);
}

static void dispatch_command(const uint8_t *data, size_t len) {
    dispatch_command_ex(data, len, false);
}

// ---- RX reassembly ---------------------------------------------------------

static void rx_reset_locked(void) {
    free(s_rx_buf); s_rx_buf = NULL;
    s_rx_len = s_rx_cap = 0;
    s_rx_total = 0;
    s_rx_count = 0;
    s_rx_next_idx = 0;
}

static void rx_reset(void) {
    if (!s_rx_mutex) return;
    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    rx_reset_locked();
    xSemaphoreGive(s_rx_mutex);
}

static void handle_rx_write_locked(const uint8_t *data, size_t len,
                                   uint8_t **complete, size_t *complete_len) {
    ESP_LOGI(TAG, "RX write (%d bytes)", (int)len);

    // Chunked? [0xFE, idx, total, ...payload]
    if (len >= CHUNK_HEADER_BYTES && data[0] == CHUNK_MAGIC) {
        uint8_t idx = data[1];
        uint8_t total = data[2];
        const uint8_t *frag = data + CHUNK_HEADER_BYTES;
        size_t flen = len - CHUNK_HEADER_BYTES;
        ESP_LOGI(TAG, "RX chunk %d/%d (%d bytes)", idx + 1, total, (int)flen);

        if (total == 0) { rx_reset_locked(); return; }

        if (idx == 0 || s_rx_total != total) {
            rx_reset_locked();
            s_rx_total = total;
            // Sized to the message: a fixed 8 KB buffer doesn't fit beside the
            // UI on boards without PSRAM. The longer of this chunk and one ATT
            // write is only an estimate; a longer chunk later grows the buffer.
            size_t chunk_max = s_mtu > 3 + CHUNK_HEADER_BYTES
                               ? (size_t)s_mtu - 3 - CHUNK_HEADER_BYTES : 0;
            if (flen > chunk_max) chunk_max = flen;
            s_rx_cap = (size_t)total * chunk_max;
            if (s_rx_cap > MAX_RX_TOTAL_BYTES) s_rx_cap = MAX_RX_TOTAL_BYTES;
            s_rx_buf = malloc(s_rx_cap);
            if (!s_rx_buf) {
                ESP_LOGE(TAG, "no memory to reassemble a %u-byte command", (unsigned)s_rx_cap);
                rx_reset_locked();
                return;
            }
            s_rx_len = 0;
        }

        if (idx != s_rx_next_idx || idx >= s_rx_total) { rx_reset_locked(); return; }

        if (s_rx_len + flen > MAX_RX_TOTAL_BYTES) {
            ESP_LOGE(TAG, "chunked command exceeds %d bytes", MAX_RX_TOTAL_BYTES);
            rx_reset_locked();
            return;
        }
        if (s_rx_len + flen > s_rx_cap) {
            // A long write can carry more than the estimate: assume the rest of
            // the chunks are this long too.
            size_t cap = s_rx_len + (size_t)(s_rx_total - idx) * flen;
            if (cap > MAX_RX_TOTAL_BYTES) cap = MAX_RX_TOTAL_BYTES;
            uint8_t *grown = realloc(s_rx_buf, cap);
            if (!grown) {
                ESP_LOGE(TAG, "no memory to reassemble a %u-byte command", (unsigned)cap);
                rx_reset_locked();
                return;
            }
            s_rx_buf = grown;
            s_rx_cap = cap;
        }
        memcpy(s_rx_buf + s_rx_len, frag, flen);
        s_rx_len += flen;
        s_rx_count++;
        s_rx_next_idx = idx + 1;

        if (s_rx_count < s_rx_total) return;

        ESP_LOGI(TAG, "RX reassembled (%d bytes)", (int)s_rx_len);
        *complete = s_rx_buf;
        *complete_len = s_rx_len;
        s_rx_buf = NULL;
        rx_reset_locked();
        return;
    }
}

static void handle_rx_write(const uint8_t *data, size_t len) {
    if (!s_rx_mutex) return;
    if (len < CHUNK_HEADER_BYTES || data[0] != CHUNK_MAGIC) {
        dispatch_command(data, len);
        return;
    }
    uint8_t *complete = NULL;
    size_t complete_len = 0;
    xSemaphoreTake(s_rx_mutex, portMAX_DELAY);
    handle_rx_write_locked(data, len, &complete, &complete_len);
    xSemaphoreGive(s_rx_mutex);
    if (complete) {
        dispatch_command(complete, complete_len);
        free(complete);
    }
}

// ---- GATT access callbacks -------------------------------------------------

static stack_monitor_t s_host_stack = STACK_MONITOR_INIT;

static int rx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (s_shutting_down) return BLE_ATT_ERR_UNLIKELY;
    if (ble_server_muse_gatt_refused()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total == 0) return 0;
    uint8_t *buf = malloc(total);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, total, &out_len);
    if (rc == 0 && out_len > 0) {
        handle_rx_write(buf, out_len);
    }
    free(buf);
    return 0;
}

static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc = rx_access(conn_handle, attr_handle, ctxt, arg);
    stack_monitor_record(&s_host_stack);
    return rc;
}

static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (s_shutting_down) return BLE_ATT_ERR_UNLIKELY;
    if (ble_server_muse_gatt_refused()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    int rc = os_mbuf_append(ctxt->om, s_last_status, strlen(s_last_status));
    stack_monitor_record(&s_host_stack);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ---- Service definition ----------------------------------------------------

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &RX_UUID.u,
        .access_cb = rx_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_rx_handle,
    },
    {
        .uuid = &TX_UUID.u,
        .access_cb = tx_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

// ---- TX path (status + chunked) --------------------------------------------

static bool notify_payload(const uint8_t *data, size_t len) {
    if (s_shutting_down) return false;
    if (!s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify rc=%d", rc);
    }
    return rc == 0;
}

// A pairing_client_hello's answer that goes in plaintext whatever came before (Gadget setup v2):
// error_operation_in_progress (the re-setup prompt's rate refusal) or error_pairing_unavailable (a re-setup outside
// setup mode). It carries no secret; every other status keeps plaintext_status_allowed's rule.
static void send_hello_answer_plaintext(const char *status) {
    if ((strcmp(status, "error_operation_in_progress") != 0 && strcmp(status, "error_pairing_unavailable") != 0
         && strcmp(status, "error_pairing_invalid_hello") != 0)
        || !s_tx_mutex || s_shutting_down) return;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    size_t n = strlen(status);
    memcpy(s_last_status, status, n + 1);
    bool sent = notify_payload((const uint8_t *)s_last_status, n);
    xSemaphoreGive(s_tx_mutex);
    if (sent) ESP_LOGI(TAG, "status: %s (plaintext, a hello's answer)", status);
}

static bool plaintext_status_allowed(const char *status) {
    return strcmp(status, "error_encryption_required") == 0
           || strcmp(status, "error_pairing_invalid_hello") == 0
           || strcmp(status, "error_pairing_unavailable") == 0
           || strcmp(status, "error_pairing_decrypt") == 0;
}

// The TX lock covers counter allocation and all record chunks. The pairing
// mutex is released by encryption before the staggered BLE transmission.
static bool send_chunked_locked(const char *data, uint32_t record_generation) {
    if (!data || s_shutting_down) return false;
    size_t total_len = strlen(data);
    size_t notify_max = s_mtu;
    if (notify_max <= 3) return false;
    notify_max -= 3;
    if (notify_max > MAX_NOTIFY_CHUNK) notify_max = MAX_NOTIFY_CHUNK;
    if (notify_max <= CHUNK_HEADER_BYTES) return false;
    size_t usable = notify_max - CHUNK_HEADER_BYTES;
    size_t total_chunks = (total_len + usable - 1) / usable;
    if (total_chunks == 0) total_chunks = 1;
    if (total_chunks > 255) {
        ESP_LOGW(TAG, "TX message exceeds BLE framing limit");
        return false;
    }

    ESP_LOGI(TAG, "TX chunked %d bytes -> %d chunks (mtu=%d, %d/chunk)",
             (int)total_len, (int)total_chunks, s_mtu, (int)usable);
    uint8_t buf[MAX_NOTIFY_CHUNK];
    for (size_t i = 0; i < total_chunks; i++) {
        // A phase change must finish this record: its counter is already used.
        // Only a replacement encryption session makes its remaining chunks stale.
        if (record_generation != 0
            && !link_pairing_record_session_is_current(record_generation)) return false;
        size_t off = i * usable;
        size_t flen = (off + usable > total_len) ? (total_len - off) : usable;
        buf[0] = CHUNK_MAGIC;
        buf[1] = (uint8_t)i;
        buf[2] = (uint8_t)total_chunks;
        memcpy(buf + CHUNK_HEADER_BYTES, data + off, flen);
        if (!notify_payload(buf, flen + CHUNK_HEADER_BYTES)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static bool send_status(const char *status, uint32_t generation) {
    if (!status || !s_tx_mutex || s_shutting_down) return false;
    bool sent = false;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uint32_t record_generation = 0;
    char *encrypted = link_pairing_encrypt_status(status, generation, &record_generation);
    if (encrypted) {
        snprintf(s_last_status, sizeof(s_last_status), "encrypted_status");
        sent = send_chunked_locked(encrypted, record_generation);
        free(encrypted);
    } else if (generation != 0 || s_plaintext_status_blocked || !plaintext_status_allowed(status)) {
        ESP_LOGW(TAG, "plaintext status suppressed: %s", status);
    } else {
        size_t n = strlen(status);
        if (n >= sizeof(s_last_status)) n = sizeof(s_last_status) - 1;
        memcpy(s_last_status, status, n);
        s_last_status[n] = '\0';
        sent = notify_payload((const uint8_t *)s_last_status, n);
    }
    xSemaphoreGive(s_tx_mutex);
    if (sent) ESP_LOGI(TAG, "status: %s", status);
    return sent;
}

void ble_server_send_status(const char *status) {
    send_status(status, 0);
}

bool ble_server_send_pairing_status(const char *status, uint32_t generation) {
    return generation != 0 && send_status(status, generation);
}

void ble_server_send_chunked(const char *data) {
    if (!s_tx_mutex || s_shutting_down) return;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    (void)send_chunked_locked(data, 0);
    xSemaphoreGive(s_tx_mutex);
}

bool ble_server_send_encrypted_json(const char *json, uint32_t generation) {
    if (!json || generation == 0 || !s_tx_mutex || s_shutting_down) return false;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uint32_t record_generation = 0;
    char *encrypted = link_pairing_encrypt_json(json, generation, &record_generation);
    bool sent = encrypted && send_chunked_locked(encrypted, record_generation);
    free(encrypted);
    xSemaphoreGive(s_tx_mutex);
    return sent;
}

static void shutdown_stack(bool release_memory);
static void host_wdt_stop(void);

void ble_server_full_shutdown(void) {
    shutdown_stack(true);
}

bool ble_server_power_off(void) {
    if (!s_started || s_shutting_down) return true;
    shutdown_stack(false);
    return !s_started;
}

static void shutdown_stack(bool release_memory) {
    if (!s_started || s_shutting_down) return;
    s_shutting_down = true;
    s_advertising_enabled = false;
    ESP_LOGI(TAG, "shutting down BLE");
    host_wdt_stop();   // the host task ends on purpose

    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGD(TAG, "adv_stop rc=%d", rc);
    }
    // gap_event_cb hides events from the companion while shutting down, and
    // the stack may stop before the disconnect arrives at all. Remember the
    // link so the companion can be told it's gone (below).
    uint16_t phone_conn = s_conn_handle;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            ESP_LOGD(TAG, "terminate rc=%d", rc);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        rc = nimble_port_stop();
        if (rc == 0) break;
        ESP_LOGW(TAG, "nimble_port_stop rc=%d (attempt %d/3)", rc, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed after retries — rebooting");
        stack_monitor_record(NULL);
        esp_restart();
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
    }
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_started = false;
    s_synced = false;
    s_advertising_active = false;
    if (phone_conn != BLE_HS_CONN_HANDLE_NONE) {
        // Otherwise a companion such as ble_basic keeps the dead handle and
        // goes on notifying into a stopped stack.
        struct ble_gap_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = BLE_GAP_EVENT_DISCONNECT;
        ev.disconnect.reason = BLE_HS_ENOTCONN;
        ev.disconnect.conn.conn_handle = phone_conn;
        for (int i = 0; i < s_ncompanions; i++) {
            if (s_companions[i].on_gap_event) (void)s_companions[i].on_gap_event(&ev);
        }
    }

    s_shutting_down = false;
    if (!release_memory) {
        // The controller's static memory stays, so ble_server_start() can
        // bring the stack back.
        ESP_LOGI(TAG, "BLE stopped");
        return;
    }
    s_shutting_down = true;   // released for good: no restart
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    ESP_LOGI(TAG, "BLE shut down");
#else
    err = esp_bt_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_mem_release: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "BLE shut down, memory released");
#endif
}

// ---- GAP / NimBLE host wiring ----------------------------------------------

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    int companion_rc = 0;
    for (int i = 0; i < s_ncompanions && !s_shutting_down; i++) {
        if (s_companions[i].on_gap_event) {
            int rc = s_companions[i].on_gap_event(event);
            if (rc && !companion_rc) companion_rc = rc;
        }
    }
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (s_shutting_down) break;
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
                s_conn_seq++;   /* the re-setup prompt's per-connection gap */
#endif
                s_ever_reachable = true;
                muse_local_press_link_new();   // a press counts for the link that's up when it comes
#if CONFIG_MUSE_GATT_DBHASH
                gatt_dbhash_conn(s_conn_handle, true);
#endif
                s_advertising_active = false;
                s_plaintext_status_blocked = false;
                ESP_LOGI(TAG, "connected conn_handle=%d", s_conn_handle);
            } else {
                s_advertising_active = false;
                ESP_LOGI(TAG, "connect failed status=%d", event->connect.status);
                if (!s_shutting_down) start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
#if CONFIG_MUSE_GATT_DBHASH
            gatt_dbhash_conn(event->disconnect.conn.conn_handle, false);
#endif
            ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
            resetup_gap_ended(&s_prompt_gap, false, esp_timer_get_time());   /* its prompt ends with the link */
#endif
            s_subscribed = false;
            s_mtu = 23;
            s_advertising_active = false;
            s_plaintext_status_blocked = false;
            rx_reset();
            link_pairing_reset();
            factory_test_on_disconnect();
            if (s_cb.on_client_disconnected) s_cb.on_client_disconnected();
            adv_pace_fast_now("disconnect");   // fast for 30 s from the link's loss
            if (!s_shutting_down && s_advertising_enabled) start_advertising();
            else if (!s_shutting_down && s_companion_advertising) start_advertising();
            else if (!s_shutting_down && advertised_companion()) start_advertising();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            // A timed turn ended (see ADV_TURN_MS): the other payload's turn.
            s_advertising_active = false;
            if (!s_shutting_down && event->adv_complete.reason == BLE_HS_ETIMEOUT) {
                s_adv_turn = !s_adv_turn;
                start_advertising();
            }
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_tx_handle) {
                s_subscribed = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "subscribe notify=%d", s_subscribed);
                if (s_subscribed) {
                    xTaskCreate(client_connected_task, "ble_conn", 4096, NULL, 5, NULL);
                }
            }
            break;
        case BLE_GAP_EVENT_MTU:
            s_mtu = event->mtu.value;
            ESP_LOGI(TAG, "mtu=%d conn=%d", event->mtu.value, event->mtu.conn_handle);
            break;
        default:
            break;
    }
    return companion_rc;
}

// The companion whose UUID wants advertising now, if any.
static const ble_companion_t *advertised_companion(void) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO   // only the musegadgets companion (mg_glue.c) has its own UUID
    for (int i = 0; i < s_ncompanions; i++) {
        const ble_companion_t *c = &s_companions[i];
        if (c->adv_uuid128 && c->wants_advertising && c->wants_advertising()) return c;
    }
#endif
    return NULL;
}

// Bench (the musegadgets console's !SETUP ADV): Link setup advertised
// after setup too, flagged as not set up, until then (RAM only).
static int64_t s_bench_setup_until_us = 0;

static bool bench_setup_adv(void) {
    return s_bench_setup_until_us && esp_timer_get_time() < s_bench_setup_until_us;
}

// What advertising should carry now: Link setup's UUID (link), a companion's
// (other), both taking turns, or nothing (returns false).
static bool advertising_wanted(bool *link, const ble_companion_t **other) {
    // A companion with its own UUID (musegadgets) is found after setup too.
    *other = advertised_companion();
    *link = true;
    // The companion service keeps the device visible after setup.
    if (!s_companion_advertising) {
        *link = !config_setup_complete() && s_advertising_enabled;
        if (bench_setup_adv()) {   // !SETUP ADV: Link setup found after setup too
            *link = true;
            return true;
        }
        if (!*other) {
            if (config_setup_complete()) return false;
            if (!s_advertising_enabled) return false;
        }
    }
    return true;
}

static void start_advertising(void) {
    if (s_shutting_down) return;
    if (s_gatt_failed) {   // never advertise without the whole GATT table
        ESP_LOGE(TAG, "advertising held: the GATT table didn't register");
        return;
    }
    if (s_quiet) return;   // a Wi-Fi video stream wants the radio (ble_server_set_quiet)
    if (s_bench_silent) return;   // bench: a stack that stopped advertising (ble_server_bench_silence)
    if (s_bench_pause_until_us && !s_bench_pause_only) return;   // bench: off the phone (ble_server_bench_pause)
    if (s_stall_quiet && !s_safe_identity) return;   // a stalled Muse protocol isn't advertised
    const ble_companion_t *other;
    bool link;
    if (!advertising_wanted(&link, &other)) return;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    if (s_advertising_active) return;
    bool rotate = link && other;
    bool show_other = other && (!link || s_adv_turn);
    struct ble_gap_adv_params advp = {0};
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // >ble.pause=SECS,ADDR: found by anyone, connectable only from the accept list.
    advp.filter_policy = s_bench_pause_only ? BLE_HCI_ADV_FILT_CONN : BLE_HCI_ADV_FILT_NONE;
#if CONFIG_HOMEHUB_BLE_PERSISTENT
    // When only the companion service is advertising (after setup), advertise
    // slowly: each advertising event takes the shared radio from Wi-Fi. Setup
    // keeps NimBLE's fast 30-60 ms default.
    if (config_setup_complete() || !s_advertising_enabled) {
        advp.itvl_min = BLE_GAP_ADV_ITVL_MS(s_companion_interval_ms);
        advp.itvl_max = advp.itvl_min + BLE_GAP_ADV_ITVL_MS(20);
    }
#endif

    // Adv packet: flags + 128-bit service UUID + manufacturer data (~26 bytes).
    // Manufacturer data: 0xFFFF (test/unassigned company ID) + the flags byte.
    uint8_t mfg_data[] = {
        0xFF, 0xFF,
        (uint8_t)((config_setup_complete() && !bench_setup_adv() ? LINK_ADV_SET_UP : 0)
                  | (link_pairing_mgcommands() ? LINK_ADV_BLE_GADGET : 0)),
    };
    ble_uuid128_t other_uuid;

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    if (show_other) {
        other_uuid.u.type = BLE_UUID_TYPE_128;
        memcpy(other_uuid.value, other->adv_uuid128, sizeof(other_uuid.value));
        adv.uuids128 = &other_uuid;
        static ble_uuid16_t other_uuid16;
        uint16_t u16 = other->adv_uuid16 ? other->adv_uuid16() : 0;
        uint16_t appearance = other->adv_appearance ? other->adv_appearance() : 0;
        if (u16) {
            other_uuid16 = (ble_uuid16_t)BLE_UUID16_INIT(u16);
            adv.uuids16 = &other_uuid16;
            adv.num_uuids16 = 1;
            adv.uuids16_is_complete = 0;
        }
        if (appearance) {
            adv.appearance = appearance;
            adv.appearance_is_present = 1;
        }
        if (config_setup_complete()) {
            // Fast (30-50 ms) for 30 s after boot, a disconnect or a
            // talk press with no app; then Apple's 417.5 ms.
            bool fast = adv_pace_fast(&s_adv_pace, esp_timer_get_time());
            adv_pace_itvl(&s_adv_pace, esp_timer_get_time(), &advp.itvl_min, &advp.itvl_max);
            s_adv_paced = true;
            s_adv_paced_fast = fast;
        } else {
            // Before setup the companion takes turns with Link setup: 100-150 ms.
            advp.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
            advp.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
            s_adv_paced = false;
        }
    } else {
        s_adv_paced = false;
        adv.uuids128 = (ble_uuid128_t *)&SVC_UUID;
        adv.mfg_data = mfg_data;
        adv.mfg_data_len = sizeof(mfg_data);
    }

    static char safe_name[32];
    const char *name = s_device_name;
    if (s_safe_identity) {
        // No UUIDs, no Link flags: nothing a Muse scan matches.
        memset(&adv, 0, sizeof(adv));
        adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        const char *tail = strncmp(s_device_name, "MuseGadget-", 11) == 0 ? s_device_name + 11 : s_device_name;
        snprintf(safe_name, sizeof(safe_name), "SAFE-%s", tail);
        name = safe_name;
        rotate = false;
        show_other = false;
    }
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    // Scan response: complete local name (split out so the UUID + name
    // both fit within the 31-byte legacy advertising limit).
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);
    }

    rc = ble_gap_adv_start(s_safe_identity && !s_safe_public ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC, NULL,
                           rotate ? ADV_TURN_MS : BLE_HS_FOREVER, &advp, gap_event_cb, NULL);
    if (rc != 0 && s_safe_identity && !s_safe_public) {
        // Never a silent SAFE board, and never the Muse identity: the public
        // address with the SAFE name, no Muse UUIDs, and
        // the Muse GATT refused (ble_server_muse_gatt_refused).
        ESP_LOGE(TAG, "adv_start rc=%d with the SAFE random address: SAFE name on the public address", rc);
        s_safe_public = true;
        start_advertising();
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    } else {
        s_advertising_active = true;
        s_ever_reachable = true;
        s_adv_link = link;
        s_adv_other = other;
        ESP_LOGI(TAG, "advertising as %s%s", name,
                 rotate ? " (taking turns)" : show_other ? " (companion)" : "");
    }
}

// Starts advertising, or restarts it when what's running isn't what's wanted
// now: host sync can start a companion's advertising before Link setup asks
// for its own, and a running advertisement never picks up the change itself.
static void update_advertising(void) {
    if (!s_synced) return;
    if (s_advertising_active) {
        const ble_companion_t *other;
        bool link;
        if (advertising_wanted(&link, &other) && link == s_adv_link && other == s_adv_other) return;
        int rc = ble_gap_adv_stop();
        if (rc != 0) ESP_LOGD(TAG, "adv_stop rc=%d", rc);
        s_advertising_active = false;
    }
    start_advertising();
}

#if CONFIG_MUSE_GADGET_WDT
// The gadget watchdog's NimBLE host user (muse_wdt.h): a callout on the host's
// own event queue feeds it, so a host task that stops running stops the feeds.
#define HOST_WDT_FEED_MS 10000
static struct ble_npl_callout s_host_wdt;
static bool s_host_wdt_on;

static void host_wdt_feed(struct ble_npl_event *ev) {
    (void)ev;
    muse_wdt_feed(MUSE_WDT_BLE_HOST);
    ble_npl_callout_reset(&s_host_wdt, ble_npl_time_ms_to_ticks32(HOST_WDT_FEED_MS));
}

/* A stalled Muse protocol stops advertising the Muse identity within
 * 10 s. Every second, on the host task: a companion that missed its
 * heartbeat (stalled()) takes the normal identity off the air until it
 * beats again. The task watchdog still resets a stall that lasts 15 s. */
static struct ble_npl_callout s_stall_check;
static void stall_check(struct ble_npl_event *ev) {
    (void)ev;
    bool stalled = false;
    for (int i = 0; i < s_ncompanions; i++) {
        if (s_companions[i].stalled && s_companions[i].stalled()) stalled = true;
    }
    if (stalled && !s_stall_quiet) {
        s_stall_quiet = true;
        if (!s_safe_identity && s_advertising_active) {
            int rc = ble_gap_adv_stop();
            s_advertising_active = false;
            ESP_LOGE(TAG, "the Muse protocol is stalled: advertising stopped (rc=%d)", rc);
        }
    } else if (!stalled && s_stall_quiet) {
        s_stall_quiet = false;
        ESP_LOGW(TAG, "the Muse protocol is back: advertising again");
        start_advertising();
    }
    ble_npl_callout_reset(&s_stall_check, ble_npl_time_ms_to_ticks32(1000));
}

static void host_wdt_start(void) {
    if (s_host_wdt_on) return;
    ble_npl_callout_init(&s_host_wdt, nimble_port_get_dflt_eventq(), host_wdt_feed, NULL);
    ble_npl_callout_init(&s_stall_check, nimble_port_get_dflt_eventq(), stall_check, NULL);
    s_host_wdt_on = true;
    host_wdt_feed(NULL);   // on the host task: on_sync runs there
    ble_npl_callout_reset(&s_stall_check, ble_npl_time_ms_to_ticks32(1000));
}

static void host_wdt_stop(void) {
    if (!s_host_wdt_on) return;
    ble_npl_callout_stop(&s_host_wdt);
    ble_npl_callout_stop(&s_stall_check);
    muse_wdt_remove(MUSE_WDT_BLE_HOST);
    s_host_wdt_on = false;
}
#else
static void host_wdt_start(void) {}
static void host_wdt_stop(void) {}
#endif

/* SAFE on the public address (no random address could be set) refuses
 * the Muse services (musegadgets, Link setup): a phone's saved record that
 * reconnects by address finds nothing Muse to talk to. SMP and the console
 * stay, as in every SAFE boot (the recovery tiers). */
bool ble_server_muse_gatt_refused(void) {
    return s_safe_identity && s_safe_public;
}

#if CONFIG_MUSE_GADGET_BLE_AUDIO
// On the NimBLE host task: the fast window started or ended. A running
// paced advertisement restarts at the interval due now; at the end, the
// slow set (logged).
static void adv_pace_event(struct ble_npl_event *ev) {
    (void)ev;
    int64_t now = esp_timer_get_time();
    bool fast = adv_pace_fast(&s_adv_pace, now);
    if (!fast && s_adv_pace_logged_fast) {
        s_adv_pace_logged_fast = false;
        ESP_LOGI(TAG, "adv: slow 417.5 ms");
    }
    if (s_advertising_active && s_adv_paced && s_adv_paced_fast != fast) {
        ble_gap_adv_stop();
        s_advertising_active = false;
        start_advertising();
    }
    if (fast) {
        ble_npl_callout_reset(&s_adv_pace_co, ble_npl_time_ms_to_ticks32((uint32_t)((s_adv_pace.fast_until_us - now) / 1000) + 1));
    }
}
#endif

// A fast window for 30 s from now (boot, a disconnect, a talk press
// with no app connected), re-armed by each trigger. Any task: the host task
// applies it. Without BLE gadget audio nothing is paced (no companion has a
// UUID of its own): no window, no log.
static void adv_pace_fast_now(const char *why) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (!s_started || s_shutting_down) return;
    if (!s_adv_pace_co_ready) {
        ble_npl_callout_init(&s_adv_pace_co, nimble_port_get_dflt_eventq(), adv_pace_event, NULL);
        s_adv_pace_co_ready = true;
    }
    adv_pace_trigger(&s_adv_pace, esp_timer_get_time(), why);
    s_adv_pace_logged_fast = true;
    ESP_LOGI(TAG, "adv: fast 30 s (%s)", why);
    ble_npl_callout_reset(&s_adv_pace_co, 1);   // the host task's next round
#else
    (void)why;
#endif
}

void ble_server_adv_fast(const char *why) {
    adv_pace_fast_now(why);
}

static void on_sync(void) {
    host_wdt_start();
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    if (s_safe_identity) {
        uint8_t a[6];   // little-endian: a[5] is the most significant byte
        if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, a, NULL) == 0) {
            a[4] ^= 0x5A;          // never the public address
            a[5] |= 0xC0;          // static random: the two top bits set
            rc = ble_hs_id_set_rnd(a);
            ESP_LOGW(TAG, "SAFE identity: %02x:%02x:%02x:%02x:%02x:%02x (rc=%d)", a[5], a[4], a[3], a[2], a[1], a[0], rc);
        } else {
            rc = -1;
        }
        // The SAFE identity never falls back to the Muse one.
        // A second random address, else the public address with the SAFE
        // name, no Muse UUIDs and the Muse GATT refused.
        if (rc != 0) {
            ble_addr_t r;
            rc = ble_hs_id_gen_rnd(0, &r);
            if (rc == 0) rc = ble_hs_id_set_rnd(r.val);
            ESP_LOGE(TAG, "SAFE address refused: a second random address (rc=%d)", rc);
        }
        if (rc != 0) {
            s_safe_public = true;
            ESP_LOGE(TAG, "SAFE: no random address: SAFE name on the public address, Muse GATT refused");
        }
    }
    s_synced = true;
#if CONFIG_MUSE_GATT_DBHASH
    gatt_dbhash_sync();   // the hash over the live table, before anyone can read it
#endif
    adv_pace_fast_now("boot");   // fast for 30 s after boot
    start_advertising();
    muse_boot_mark("advertising");
    static bool s_version_logged;
    if (!s_version_logged) {
        // The controller's HCI version is its LL version (Core 5.1 = 0x0A):
        // Android reads the GATT Database Hash only from LL >= 0x0A.
        extern uint8_t ble_hs_hci_get_hci_version(void);   // NimBLE host, ble_hs_hci_priv.h
        s_version_logged = true;
        ESP_LOGI(TAG, "controller LL version 0x%02x", ble_hs_hci_get_hci_version());
    }
}

void ble_server_bench_setup_adv(uint32_t secs) {
    if (!s_started || s_shutting_down) return;
    s_bench_setup_until_us = secs ? esp_timer_get_time() + (int64_t)secs * 1000000 : 0;
    ESP_LOGW(TAG, "bench: Link setup advertising %s", secs ? "on after setup (!SETUP ADV)" : "off");
    if (s_advertising_active) {   // the new payload now; with a client on, at its disconnect
        ble_gap_adv_stop();
        s_advertising_active = false;
        start_advertising();
    }
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "reset reason=%d", reason);
    s_synced = false;
    s_advertising_active = false;
    s_plaintext_status_blocked = false;
    rx_reset();
    link_pairing_reset();
    factory_test_on_disconnect();
    if (s_cb.on_client_disconnected) s_cb.on_client_disconnected();
}

static void host_task(void *arg) {
    s_host_stack = (stack_monitor_t)STACK_MONITOR_INIT;
    stack_monitor_record(&s_host_stack);
    nimble_port_run();
    stack_monitor_record(NULL);
    nimble_port_freertos_deinit();
}

void ble_server_start(const char *device_name, const ble_callbacks_t *cb) {
    if (s_started) return;
    if (cb) {
        s_cb = *cb;
    } else {
        memset(&s_cb, 0, sizeof(s_cb));
    }
    s_device_name = device_name;
    if (!s_rx_mutex) s_rx_mutex = xSemaphoreCreateMutex();
    if (!s_rx_mutex) {
        ESP_LOGE(TAG, "failed to create RX mutex");
        return;
    }
    s_tx_mutex = xSemaphoreCreateMutex();
    s_shutting_down = false;
    s_synced = false;
    s_advertising_enabled = false;
    s_advertising_active = false;

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    /* The controller runs on the Wi-Fi/BLE coprocessor; NimBLE reaches it over
     * ESP-Hosted's virtual HCI. */
    esp_err_t hosted_err = esp_hosted_bt_controller_init();
    if (hosted_err == ESP_OK) {
        hosted_err = esp_hosted_bt_controller_enable();
    }
    if (hosted_err != ESP_OK) {
        ESP_LOGE(TAG, "coprocessor BT controller: %s", esp_err_to_name(hosted_err));
        return;
    }
#endif

    nimble_port_init();
    ble_svc_gap_init();
#if CONFIG_MUSE_GATT_DBHASH
    gatt_dbhash_init();   // Generic Attribute with the Database Hash (gatt_dbhash.h)
#else
    ble_svc_gatt_init();
#endif

    // The whole GATT table, the companions' (the Muse service) included,
    // is registered here, before the host starts and so before any
    // advertisement; nothing is added later (ble_server_set_companion refuses
    // after start). A table that didn't register holds advertising.
    s_gatt_failed = false;
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc);
    rc = rc ? rc : ble_gatts_add_svcs(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc); s_gatt_failed = true; }

    const struct ble_gatt_svc_def *ft = factory_test_svcs();
    rc = ble_gatts_count_cfg(ft);
    if (rc != 0) ESP_LOGE(TAG, "ft gatts_count_cfg rc=%d", rc);
    rc = rc ? rc : ble_gatts_add_svcs(ft);
    if (rc != 0) { ESP_LOGE(TAG, "ft gatts_add_svcs rc=%d", rc); s_gatt_failed = true; }

    for (int i = 0; i < s_ncompanions; i++) {
        const ble_companion_t *c = &s_companions[i];
        if (c->svcs) {
            rc = ble_gatts_count_cfg(c->svcs);
            if (rc != 0) ESP_LOGE(TAG, "companion gatts_count_cfg rc=%d", rc);
            rc = rc ? rc : ble_gatts_add_svcs(c->svcs);
            if (rc != 0) { ESP_LOGE(TAG, "companion gatts_add_svcs rc=%d", rc); s_gatt_failed = true; }
        }
        if (c->configure_host) c->configure_host();
    }

    ble_svc_gap_device_name_set(device_name);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    s_started = true;
}

typedef struct {
    struct ble_npl_event event;
    uint32_t generation;
} pairing_disconnect_event_t;

static void disconnect_pairing_session_event(struct ble_npl_event *event) {
    pairing_disconnect_event_t *work = ble_npl_event_get_arg(event);
    uint32_t generation = work->generation;
    ble_npl_event_deinit(event);
    free(work);
    // GAP events and client hello dispatch share this queue, so replacement
    // sessions cannot appear between the conditional reset and termination.
    if (!s_shutting_down && link_pairing_reset_session(generation)) {
        ble_server_stop_advertising(true);
    }
}

void ble_server_disconnect_pairing_session(uint32_t generation) {
    if (!s_started || s_shutting_down || generation == 0) return;
    pairing_disconnect_event_t *work = calloc(1, sizeof(*work));
    if (!work) {
        // Even without a host event, invalidate the session keys.
        (void)link_pairing_reset_session(generation);
        ESP_LOGE(TAG, "failed to queue pairing disconnect");
        return;
    }
    work->generation = generation;
    ble_npl_event_init(&work->event, disconnect_pairing_session_event, work);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &work->event);
}

void ble_server_set_quiet(bool quiet) {
    if (s_quiet == quiet) return;
    s_quiet = quiet;
    if (!s_started || s_shutting_down) return;
    if (quiet) {
        if (s_advertising_active) {
            int rc = ble_gap_adv_stop();
            if (rc != 0) ESP_LOGD(TAG, "adv_stop rc=%d", rc);
            s_advertising_active = false;
            ESP_LOGI(TAG, "advertising paused (Wi-Fi video)");
        }
    } else if (s_synced) {
        start_advertising();
    }
}

void ble_server_begin_advertising(void) {
    if (!s_started || s_shutting_down) return;
    s_advertising_enabled = true;
    update_advertising();
}

void ble_server_disconnect_client(void) {
    if (!s_started || s_shutting_down
        || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        ESP_LOGD(TAG, "terminate rc=%d", rc);
    }
}

void ble_server_stop_advertising(bool disconnect_client) {
    if (!s_started || s_shutting_down) return;
    s_advertising_enabled = false;
    // A setup's end (stop_setup_ble) also ends !SETUP ADV; a dropped pairing
    // session (disconnect_client) keeps it for another try.
    if (!disconnect_client) s_bench_setup_until_us = 0;
    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGD(TAG, "adv_stop rc=%d", rc);
    }
    s_advertising_active = false;
    if (disconnect_client) {
        ble_server_disconnect_client();
    }
    if (disconnect_client || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        rx_reset();
        link_pairing_reset();
        s_plaintext_status_blocked = false;
    }
    // A companion may still want to be found (start_advertising decides).
    if (s_synced) start_advertising();
}

void ble_server_set_companion(const ble_companion_t *companion) {
    if (s_started) {
        // The table is fixed once the host runs; a late service would
        // be missing from the first advertisements and the phone's cache.
        ESP_LOGE(TAG, "BLE companion added after start: refused");
        return;
    }
    if (!companion) {
        memset(s_companions, 0, sizeof(s_companions));
        s_ncompanions = 0;
    } else if (s_ncompanions < MAX_COMPANIONS) {
        s_companions[s_ncompanions++] = *companion;
    } else {
        ESP_LOGE(TAG, "too many BLE companions");
    }
}

void ble_server_refresh_advertising(void) {
    if (!s_started || s_shutting_down || !s_synced) return;
    if (s_advertising_active) {
        // Restart with what's wanted now; a turn in progress ends early.
        int rc = ble_gap_adv_stop();
        if (rc != 0) ESP_LOGD(TAG, "adv_stop rc=%d", rc);
        s_advertising_active = false;
    }
    start_advertising();
}

void ble_server_set_companion_advertising(bool enabled) {
    s_companion_advertising = enabled;
    if (!s_started || s_shutting_down) return;
    update_advertising();
}

#if CONFIG_HOMEHUB_BLE_PERSISTENT
void ble_server_set_companion_interval_ms(int interval_ms) {
    if (interval_ms < 20 || interval_ms > 10240 || interval_ms == s_companion_interval_ms) return;
    s_companion_interval_ms = interval_ms;
    if (!s_started || s_shutting_down || !s_advertising_active || s_advertising_enabled) return;
    // Restart advertising at the new interval.
    if (ble_gap_adv_stop() == 0) {
        s_advertising_active = false;
        if (s_synced && s_companion_advertising) start_advertising();
    }
}

int ble_server_companion_interval_ms(void) {
    return s_companion_interval_ms;
}
#endif

bool ble_server_is_advertising(void) {
    return s_started && !s_shutting_down && s_advertising_active;
}

bool ble_server_own_address(uint8_t addr[6]) {
    if (!s_started || s_shutting_down || !s_synced) return false;
    return ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0;
}

bool ble_server_is_synced(void) {
    return s_started && !s_shutting_down && s_synced;
}

bool ble_server_is_started(void) {
    return s_started && !s_shutting_down;
}

void ble_server_forget_bonds(void) {
    if (ble_server_is_started()) {
        int rc = ble_store_clear();
        ESP_LOGI(TAG, "forgot all bonded phones (rc=%d)", rc);
        return;
    }
#if CONFIG_BT_NIMBLE_NVS_PERSIST
    nvs_handle_t h;
    esp_err_t err = nvs_open("nimble_bond", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "forgot all bonded phones (BLE off)");
    } else {
        ESP_LOGE(TAG, "couldn't forget bonded phones: %s", esp_err_to_name(err));
    }
#endif
}

bool ble_server_forget_bonds_but_connected(void) {
    if (!ble_server_is_started() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) return false;
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) return false;
    int dropped = 0;
    for (int i = 0; i < n; i++) {
        if (ble_addr_cmp(&peers[i], &desc.peer_id_addr) == 0) continue;   // the connected phone keeps its bond
        int rc = ble_gap_unpair(&peers[i]);
        if (rc != 0 && rc != BLE_HS_ENOENT) {
            ESP_LOGE(TAG, "couldn't forget a bonded phone (rc=%d)", rc);
            return false;
        }
        dropped++;
    }
    ESP_LOGI(TAG, "forgot %d bonded phone(s); the connected one keeps its bond", dropped);
    return true;
}

bool ble_server_ever_reachable(void) {
    return s_ever_reachable;
}

void ble_server_set_safe_identity(bool safe) {
    s_safe_identity = safe;
}

// Bench (!SAFEID): one boot in the SAFE identity while in normal mode, to
// verify the way back from SAFE (finding it by name, the console and SMP
// from that address) before forcing SAFE itself. The flag lives in RTC
// no-init memory and is taken at the next boot, which restarts back to the
// normal identity after SAFEID_BOOT_MS by itself.
#define SAFEID_MAGIC 0x53414649u   // "SAFI"
#define SAFEID_BOOT_MS 120000
#if CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED
static RTC_NOINIT_ATTR uint32_t s_bench_safeid;
#else
static __NOINIT_ATTR uint32_t s_bench_safeid;   /* no RTC memory (the C61): kept across a restart all the same */
#endif
static esp_timer_handle_t s_safeid_timer;

static void safeid_back(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "bench: SAFE identity boot over: restarting as the gadget");
    muse_restart(MUSE_RESTART_CONSOLE);
}

void ble_server_bench_safe_identity(void) {
    s_bench_safeid = SAFEID_MAGIC;   // the caller restarts
}

bool ble_server_bench_safe_identity_take(void) {
    if (s_bench_safeid != SAFEID_MAGIC) {
        s_bench_safeid = 0;
        return false;
    }
    s_bench_safeid = 0;
    const esp_timer_create_args_t a = { .callback = safeid_back, .name = "safeid_back" };
    if (esp_timer_create(&a, &s_safeid_timer) != ESP_OK ||
        esp_timer_start_once(s_safeid_timer, (uint64_t)SAFEID_BOOT_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "bench: no timer for the way back: staying the gadget");
        return false;
    }
    ESP_LOGW(TAG, "bench: this boot advertises the SAFE identity; back to the gadget in %d s", SAFEID_BOOT_MS / 1000);
    return true;
}

void ble_server_bench_silence(void) {
    if (!s_started || s_shutting_down) return;
    ESP_LOGW(TAG, "bench: advertising off and the link closed until a restart (stay-reachable check)");
    s_bench_silent = true;
    if (s_advertising_active) {
        ble_gap_adv_stop();
        s_advertising_active = false;
    }
    ble_server_disconnect_client();
}

// >ble.pause: the pause is over (on the NimBLE host task): the normal
// connection policy and advertising as before; with a link up (the one
// address a pause admitted), at its disconnect.
static void bench_pause_over(struct ble_npl_event *ev) {
    (void)ev;
    if (!s_bench_pause_until_us) return;
    bool only = s_bench_pause_only;
    s_bench_pause_until_us = 0;
    s_bench_pause_only = false;
    if (only) {
        if (s_advertising_active) {   // the filtered advertisement: back to everyone
            ble_gap_adv_stop();
            s_advertising_active = false;
        }
        ble_gap_wl_set(NULL, 0);
    }
    ESP_LOGW(TAG, "bench: BLE pause over: the normal connection policy and advertising");
    if (s_synced && !s_shutting_down) start_advertising();
}

void ble_server_bench_pause(uint32_t secs, const uint8_t *only_addr) {
    if (!s_started || s_shutting_down || secs == 0) return;
    if (!s_bench_pause_co_ready) {
        ble_npl_callout_init(&s_bench_pause_co, nimble_port_get_dflt_eventq(), bench_pause_over, NULL);
        s_bench_pause_co_ready = true;
    }
    if (s_advertising_active) {
        ble_gap_adv_stop();
        s_advertising_active = false;
    }
    s_bench_pause_only = false;
    if (only_addr) {
        for (int t = 0; t < 2; t++) {
            s_bench_pause_peer[t].type = t ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
            for (int i = 0; i < 6; i++) s_bench_pause_peer[t].val[i] = only_addr[5 - i];   // little-endian
        }
        int rc = ble_gap_wl_set(s_bench_pause_peer, 2);
        if (rc == 0) {
            s_bench_pause_only = true;
        } else {
            ESP_LOGW(TAG, "bench: BLE pause: the accept list was refused (rc=%d): no one for %u s", rc, (unsigned)secs);
        }
    }
    s_bench_pause_until_us = esp_timer_get_time() + (int64_t)secs * 1000000;
    ble_npl_callout_reset(&s_bench_pause_co, ble_npl_time_ms_to_ticks32(secs * 1000));
    ESP_LOGW(TAG, "bench: BLE paused for %u s: the link closed, %s", (unsigned)secs,
             s_bench_pause_only ? "advertising connectable from one address only" : "no advertising");
    ble_server_disconnect_client();
    if (s_bench_pause_only) start_advertising();   // no link: now; with one, at its disconnect
}

bool ble_server_reachable_now(void) {
    if (!s_started || s_shutting_down) return true;   // shut down on purpose (Link builds after setup)
    if (s_quiet) return true;                         // paused on purpose (Wi-Fi video)
    if (s_bench_pause_until_us) return true;          // paused on purpose (>ble.pause, at most 600 s)
    return s_synced && (s_advertising_active || s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

bool ble_server_has_connection(void) {
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ble_server_resetup_prompt_shown(void) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO   // without it a set-up device never shows a re-setup prompt
    resetup_gap_shown(&s_prompt_gap, s_conn_seq);
#endif
}

void ble_server_resetup_prompt_ended(bool dismissed) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    resetup_gap_ended(&s_prompt_gap, dismissed, esp_timer_get_time());
#else
    (void)dismissed;
#endif
}
