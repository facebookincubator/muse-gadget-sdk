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

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_log.h"

#include "config_store.h"

static const char *TAG = "link.identity";

// Device identity. The device advertises over BLE as "MuseGadget-XXXXXX" and
// identifies itself to the remote service as node_id "homelink-XXXXXX"
// (device-token mint/refresh). XXXXXX = last 3 WiFi
// STA MAC octets in hex. Builds may change the visible BLE prefix
// (CONFIG_HOMEHUB_BLE_NAME_PREFIX) and bench boards add a suffix
// (CONFIG_HOMEHUB_BLE_NAME_SUFFIX); the node_id prefix is fixed.
#define BLE_NAME_PREFIX CONFIG_HOMEHUB_BLE_NAME_PREFIX CONFIG_HOMEHUB_BLE_NAME_SUFFIX
#define NODE_ID_PREFIX "homelink"

static char s_node_id[32];
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
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed; using zeros");
    }
    snprintf(s_node_id, sizeof(s_node_id),
             NODE_ID_PREFIX "-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_ble_name, sizeof(s_ble_name),
             BLE_NAME_PREFIX "-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id, sizeof(s_device_id), "hatch-link:%s", s_mac);
    ESP_LOGI(TAG, "node_id=%s ble_name=%s", s_node_id, s_ble_name);

    char stored[sizeof(s_sdk_token) + 1];
    if (config_get_str(SDK_TOKEN_KEY, stored, sizeof(stored))) {
        if (identity_sdk_token_valid(stored)) {
            memcpy(s_sdk_token, stored, sizeof(s_sdk_token));
        } else {
            ESP_LOGW(TAG, "ignoring an invalid stored SDK token");
        }
    }
}

const char *identity_node_id(void) { return s_node_id; }
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
