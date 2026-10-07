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

#include "ble_server.h"
#include "stack_monitor.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_app_desc.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cJSON.h"
#include "config_store.h"
#include "link_pairing.h"
#include "factory_test.h"

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

#define MAX_NOTIFY_CHUNK   160
#define CHUNK_HEADER_BYTES 3
#define CHUNK_MAGIC        0xFE
#define MAX_RX_TOTAL_BYTES 8192

static ble_callbacks_t s_cb = {0};
static const char *s_device_name = NULL;

static uint16_t s_rx_handle = 0;
static uint16_t s_tx_handle = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed = false;
static bool s_started = false;
static bool s_shutting_down = false;
static bool s_synced = false;
static bool s_advertising_enabled = false;
static bool s_advertising_active = false;
static bool s_plaintext_status_blocked = false;
static ble_companion_t s_companion = {0};
static bool s_companion_advertising = false;

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
    free(a);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
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

    if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && config_setup_complete()) {
        // BLE outlives setup only for a companion service (Muse phone setup).
        // Re-pairing goes through unpair/reset, never a new session here.
        ble_server_send_status("error_pairing_unavailable");
    } else if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
        char *reply = NULL;
        const char *err = link_pairing_handle_client_hello(root, &reply);
        if (err) {
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
        if (!cJSON_IsString(s) || !cJSON_IsString(p) || !cJSON_IsString(at)
            || !s->valuestring || !p->valuestring || !at->valuestring
            || !*s->valuestring || !*p->valuestring || !*at->valuestring
            || !cJSON_IsString(rt) || !rt->valuestring || !*rt->valuestring
            || !cJSON_IsString(tt) || !tt->valuestring
            || strcmp(tt->valuestring, "device") != 0) {
            ble_server_send_status("error_missing_credentials");
        } else {
            provision_args_t *a = calloc(1, sizeof(*a));
            if (!a) {
                ble_server_send_status("error_operation_in_progress");
                delete_command_json(root, decrypted);
                return;
            }
            a->ssid = dup_str(s->valuestring);
            a->password = dup_str(p->valuestring);
            a->access_token = dup_str(at->valuestring);
            if (cJSON_IsString(rt) && rt->valuestring) {
                a->refresh_token = dup_str(rt->valuestring);
            }
            if (cJSON_IsString(u) && u->valuestring) {
                a->username = dup_str(u->valuestring);
            }
            if (*ota_url) {
                a->ota_url = dup_str(ota_url);
                a->ota_force = optional_ota_force(root);
                if (!a->ota_url) {
                    secure_free_str(a->ssid);
                    secure_free_str(a->password);
                    secure_free_str(a->access_token);
                    secure_free_str(a->refresh_token);
                    secure_free_str(a->username);
                    free(a);
                    ble_server_send_status("error_operation_in_progress");
                    delete_command_json(root, decrypted);
                    return;
                }
            }
            cJSON *au = cJSON_GetObjectItem(root, "api_url");
            if (cJSON_IsString(au) && au->valuestring && *au->valuestring) {
                a->api_url = dup_str(au->valuestring);
            }
            // Older firmware ignores api_url_v2 and keeps reading api_url.
            cJSON *au2 = cJSON_GetObjectItem(root, "api_url_v2");
            if (cJSON_IsString(au2) && au2->valuestring && *au2->valuestring) {
                a->api_url_v2 = dup_str(au2->valuestring);
            }
            cJSON *nh = cJSON_GetObjectItem(root, "noise_host");
            if (cJSON_IsString(nh) && nh->valuestring && *nh->valuestring) {
                a->noise_host = dup_str(nh->valuestring);
            }
            // 8 KB stack — mbedtls 3.6 (IDF v6) needs significantly more
            // stack during TLS handshake than v5's 3.x (~2 KB more peak).
            a->session_generation = link_pairing_mark_provisioning_active();
            if (a->session_generation == 0
                || xTaskCreate(provision_task, "prov", 8192, a, 5, NULL) != pdPASS) {
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
            // UI on boards without PSRAM. The sender fills every chunk but the
            // last, so the longer of this chunk and one ATT write bounds them.
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

        if (s_rx_len + flen > s_rx_cap) { rx_reset_locked(); return; }
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

void ble_server_full_shutdown(void) {
    if (!s_started || s_shutting_down) return;
    s_shutting_down = true;
    s_advertising_enabled = false;
    ESP_LOGI(TAG, "shutting down BLE");

    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGD(TAG, "adv_stop rc=%d", rc);
    }
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

    err = esp_bt_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_mem_release: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "BLE shut down, memory released");
}

// ---- GAP / NimBLE host wiring ----------------------------------------------

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    int companion_rc = 0;
    if (s_companion.on_gap_event && !s_shutting_down) {
        companion_rc = s_companion.on_gap_event(event);
    }
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (s_shutting_down) break;
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
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
            ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed = false;
            s_mtu = 23;
            s_advertising_active = false;
            s_plaintext_status_blocked = false;
            rx_reset();
            link_pairing_reset();
            factory_test_on_disconnect();
            if (s_cb.on_client_disconnected) s_cb.on_client_disconnected();
            if (!s_shutting_down && s_advertising_enabled) start_advertising();
            else if (!s_shutting_down && s_companion_advertising) start_advertising();
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

static void start_advertising(void) {
    if (s_shutting_down) return;
    // The companion service keeps the device visible after setup.
    if (!s_companion_advertising) {
        if (config_setup_complete()) return;
        if (!s_advertising_enabled) return;
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    if (s_advertising_active) return;
    struct ble_gap_adv_params advp = {0};
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // Adv packet: flags + 128-bit service UUID + manufacturer data (~26 bytes).
    // Manufacturer data: 0xFFFF (test/unassigned company ID) + 1 byte paired flag.
    uint8_t mfg_data[] = { 0xFF, 0xFF, config_setup_complete() ? 0x01 : 0x00 };

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    adv.mfg_data = mfg_data;
    adv.mfg_data_len = sizeof(mfg_data);

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    // Scan response: complete local name (split out so the UUID + name
    // both fit within the 31-byte legacy advertising limit).
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)s_device_name;
    rsp.name_len = strlen(s_device_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &advp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    } else {
        s_advertising_active = true;
        ESP_LOGI(TAG, "advertising as %s", s_device_name);
    }
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    s_synced = true;
    start_advertising();
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

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc);

    const struct ble_gatt_svc_def *ft = factory_test_svcs();
    rc = ble_gatts_count_cfg(ft);
    if (rc != 0) ESP_LOGE(TAG, "ft gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(ft);
    if (rc != 0) ESP_LOGE(TAG, "ft gatts_add_svcs rc=%d", rc);

    if (s_companion.svcs) {
        rc = ble_gatts_count_cfg(s_companion.svcs);
        if (rc != 0) ESP_LOGE(TAG, "companion gatts_count_cfg rc=%d", rc);
        rc = ble_gatts_add_svcs(s_companion.svcs);
        if (rc != 0) ESP_LOGE(TAG, "companion gatts_add_svcs rc=%d", rc);
    }
    if (s_companion.configure_host) s_companion.configure_host();

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

void ble_server_begin_advertising(void) {
    if (!s_started || s_shutting_down) return;
    s_advertising_enabled = true;
    if (s_synced) start_advertising();
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
    if (s_companion_advertising && s_synced) start_advertising();
}

void ble_server_set_companion(const ble_companion_t *companion) {
    if (s_started) return;
    if (companion) {
        s_companion = *companion;
    } else {
        memset(&s_companion, 0, sizeof(s_companion));
    }
}

void ble_server_set_companion_advertising(bool enabled) {
    s_companion_advertising = enabled;
    if (!s_started || s_shutting_down) return;
    if (enabled) {
        if (s_synced) start_advertising();
    } else if (!s_advertising_enabled && s_advertising_active) {
        int rc = ble_gap_adv_stop();
        if (rc != 0) {
            ESP_LOGD(TAG, "adv_stop rc=%d", rc);
        }
        s_advertising_active = false;
    }
}

bool ble_server_is_started(void) {
    return s_started && !s_shutting_down;
}

bool ble_server_has_connection(void) {
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
