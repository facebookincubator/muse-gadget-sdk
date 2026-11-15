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

#include "identity.h"

#include <stdatomic.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_log.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "config_store.h"

static const char *TAG = "link.identity";

// Device identity. The device advertises over BLE as "MuseGadget-XXXXXX" and
// identifies itself to the remote service by its node id (device-token
// mint/refresh): the one its setup stored, or the "homelink-XXXXXX" a device
// set up by earlier firmware keeps (identity.h). XXXXXX = last 3
// WiFi STA MAC octets in hex. Builds may change the visible BLE prefix
// (CONFIG_HOMEHUB_BLE_NAME_PREFIX) and bench boards add a suffix
// (CONFIG_HOMEHUB_BLE_NAME_SUFFIX); neither changes the node id.
#define BLE_NAME_PREFIX CONFIG_HOMEHUB_BLE_NAME_PREFIX CONFIG_HOMEHUB_BLE_NAME_SUFFIX
// Earlier firmware's node ids, which devices it set up keep, and what a new
// setup offers while gadget ids are off (CONFIG_MUSE_GADGET_NODE_ID_PREFIX).
#define LEGACY_NODE_ID_PREFIX "homelink"
#if CONFIG_MUSE_GADGET_NODE_ID_PREFIX
#define NODE_ID_PREFIX "gadget"
#else
#define NODE_ID_PREFIX LEGACY_NODE_ID_PREFIX
#endif

static char s_setup_node_id[32];
static char s_legacy_node_id[32];
// The stored id, in two buffers so a reader of the one in use never sees it
// rewritten (it changes only when setup stores new credentials).
static char s_stored_node_id[2][32];
static int s_stored_slot;
static _Atomic(const char *) s_node_id = "";
static char s_ble_name[32];
static char s_mac[18];
static char s_device_id[48];
// Set over serial (sdk_token_console.h), which the web flasher does after
// installing a build that has no token of its own.
static char s_sdk_token[IDENTITY_SDK_TOKEN_LEN + 1];

#define SDK_TOKEN_KEY "sdk_token"
// Cleared with the pairing credentials (config_clear_pairing).
#define PAIRED_SDK_TOKEN_KEY "pair_sdk_hint"
#define NO_SDK_TOKEN_HINT "none"

void identity_init(void) {
    uint8_t mac[6] = {0};
#if CONFIG_ESP_HOSTED_ENABLED
    /* The STA MAC is the Wi-Fi coprocessor's: app_run() brings Wi-Fi up first.
     * It names the device to Muse, so retry a failed call before using zeros. */
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    for (int i = 0; i < 4 && err != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    }
#else
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA MAC unavailable (%s); using zeros", esp_err_to_name(err));
    }
    snprintf(s_setup_node_id, sizeof(s_setup_node_id),
             NODE_ID_PREFIX "-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_legacy_node_id, sizeof(s_legacy_node_id),
             LEGACY_NODE_ID_PREFIX "-%02x%02x%02x", mac[3], mac[4], mac[5]);
    identity_refresh_node_id();
    snprintf(s_ble_name, sizeof(s_ble_name),
             BLE_NAME_PREFIX "-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id, sizeof(s_device_id), "hatch-link:%s", s_mac);
    ESP_LOGI(TAG, "node_id=%s ble_name=%s", identity_node_id(), s_ble_name);

    char stored[sizeof(s_sdk_token) + 1];
    if (config_get_str(SDK_TOKEN_KEY, stored, sizeof(stored))) {
        if (identity_sdk_token_valid(stored)) {
            memcpy(s_sdk_token, stored, sizeof(s_sdk_token));
        } else {
            ESP_LOGW(TAG, "ignoring an invalid stored SDK token");
        }
    }
}

bool identity_refresh_node_id(void) {
    const char *before = atomic_load(&s_node_id);
    const char *use;
    char stored[sizeof(s_stored_node_id[0])];
    if (config_get_node_id(stored, sizeof(stored))) {
        // The id this device was set up with: never renamed.
        if (strcmp(stored, before) == 0) return false;
        s_stored_slot ^= 1;
        memcpy(s_stored_node_id[s_stored_slot], stored, sizeof(stored));
        use = s_stored_node_id[s_stored_slot];
    } else if (config_is_provisioned()) {
        // Credentials from earlier firmware, which stored no id: they were
        // issued for the homelink- id.
        use = s_legacy_node_id;
    } else {
        use = s_setup_node_id;
    }
    atomic_store(&s_node_id, use);
    return strcmp(before, use) != 0;
}

const char *identity_node_id(void) { return atomic_load(&s_node_id); }
const char *identity_setup_node_id(void) { return s_setup_node_id; }
const char *identity_ble_name(void) { return s_ble_name; }
const char *identity_mac(void) { return s_mac; }
const char *identity_device_id(void) { return s_device_id; }
const char *identity_sdk_token(void) {
    if (s_sdk_token[0]) return s_sdk_token;
    return CONFIG_GADGET_SDK_TOKEN[0] ? CONFIG_GADGET_SDK_TOKEN : NULL;
}

// The same check as cmake/validate_config.cmake: "mgst_", then base64url of
// 32 bytes, whose last character carries only four bits.
bool identity_sdk_token_valid(const char *token) {
    if (!token || strlen(token) != IDENTITY_SDK_TOKEN_LEN || strncmp(token, "mgst_", 5) != 0) {
        return false;
    }
    for (const char *c = token + 5; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') return false;
    }
    return strchr("AEIMQUYcgkosw048", token[IDENTITY_SDK_TOKEN_LEN - 1]) != NULL;
}

// Pairing and the API client take the token at boot, so it stays the same
// until the next one.
bool identity_set_sdk_token(const char *token) {
    return identity_sdk_token_valid(token) && config_set_str(SDK_TOKEN_KEY, token);
}

static void sdk_token_hint(char out[IDENTITY_SDK_TOKEN_HINT_LEN + 1]) {
    const char *token = identity_sdk_token();
    snprintf(out, IDENTITY_SDK_TOKEN_HINT_LEN + 1, "%.*s", IDENTITY_SDK_TOKEN_HINT_LEN,
             token ? token : NO_SDK_TOKEN_HINT);
}

void identity_note_paired_sdk_token(void) {
    char hint[IDENTITY_SDK_TOKEN_HINT_LEN + 1];
    sdk_token_hint(hint);
    if (!config_set_str(PAIRED_SDK_TOKEN_KEY, hint)) {
        ESP_LOGW(TAG, "couldn't store which SDK token pairing used");
    }
}

bool identity_sdk_token_changed_since_pairing(char *paired_hint, size_t cap) {
    char paired[IDENTITY_SDK_TOKEN_HINT_LEN + 1];
    if (!config_get_str(PAIRED_SDK_TOKEN_KEY, paired, sizeof(paired))) return false;
    char current[IDENTITY_SDK_TOKEN_HINT_LEN + 1];
    sdk_token_hint(current);
    if (strcmp(paired, current) == 0) return false;
    if (paired_hint && cap) snprintf(paired_hint, cap, "%s", paired);
    return true;
}
