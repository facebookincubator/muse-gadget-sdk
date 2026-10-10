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

#include "wifi_mgr.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED
#include "esp_wifi.h"
#endif

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

void identity_set_sta_mac(const uint8_t mac[6])
{
    if (!mac) {
        return;
    }
    snprintf(s_node_id, sizeof(s_node_id),
             NODE_ID_PREFIX "-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_ble_name, sizeof(s_ble_name),
             BLE_NAME_PREFIX "-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id, sizeof(s_device_id), "hatch-link:%s", s_mac);
    ESP_LOGI(TAG, "node_id=%s ble_name=%s", s_node_id, s_ble_name);
}

void identity_init(void) {
    uint8_t mac[6] = {0};
    bool have_mac = false;
#if CONFIG_ESP_HOSTED
    if (wifi_mgr_get_sta_mac(mac)) {
        have_mac = true;
    } else if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        have_mac = true;
    }
#else
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        have_mac = true;
    }
#endif
    if (!have_mac) {
        ESP_LOGW(TAG, "WiFi STA MAC unavailable; using zeros");
    }
    identity_set_sta_mac(mac);
}

const char *identity_node_id(void) { return s_node_id; }
const char *identity_ble_name(void) { return s_ble_name; }
const char *identity_mac(void) { return s_mac; }
const char *identity_device_id(void) { return s_device_id; }
const char *identity_sdk_token(void) {
    return CONFIG_GADGET_SDK_TOKEN[0] ? CONFIG_GADGET_SDK_TOKEN : NULL;
}
