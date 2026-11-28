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

#include "factory_test.h"

#include <string.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "cJSON.h"

#include "config_store.h"
#include "identity.h"
#include "link_pairing.h"
#include "customer_product_info.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_ENABLED
#include "muse_ble.h"
#endif

static const char *TAG = "link.factory_test";

// Service UUID: 9FC8783A-CABC-412C-B3C9-EAC8E168CC9E
static const ble_uuid128_t FT_SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xcc, 0x68, 0xe1, 0xc8, 0xea, 0xc9, 0xb3,
    0x2c, 0x41, 0xbc, 0xca, 0x3a, 0x78, 0xc8, 0x9f);

// Device Info UUID: 132FBE4A-3DE5-423E-96EA-1DCD09170C4C
static const ble_uuid128_t FT_DEVINFO_UUID = BLE_UUID128_INIT(
    0x4c, 0x0c, 0x17, 0x09, 0xcd, 0x1d, 0xea, 0x96,
    0x3e, 0x42, 0xe5, 0x3d, 0x4a, 0xbe, 0x2f, 0x13);

// Button Test UUID: 26B985C8-8074-4111-8401-1E74702AD718
static const ble_uuid128_t FT_BUTTON_UUID = BLE_UUID128_INIT(
    0x18, 0xd7, 0x2a, 0x70, 0x74, 0x1e, 0x01, 0x84,
    0x11, 0x41, 0x74, 0x80, 0xc8, 0x85, 0xb9, 0x26);

static uint16_t s_devinfo_handle;
static uint16_t s_button_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     s_armed;
static uint8_t  s_button_value;

bool factory_test_allowed(bool set_up, bool window_open) {
    return !set_up || window_open;
}

static bool allowed_now(void) {
#if CONFIG_MUSE_ENABLED
    bool window_open = muse_ble_setup_window_open();
#else
    bool window_open = false;
#endif
    // An unreadable store counts as set up: never "unset" on an NVS error.
    return factory_test_allowed(config_setup_complete_or_unreadable() || config_is_provisioned(), window_open);
}

static int ft_devinfo_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    // Device info is identity: open to any central. The button's read and press notification are the gated part.

    s_conn_handle = conn_handle;
    s_armed = true;
    s_button_value = 0x00;

    const esp_app_desc_t *desc = esp_app_get_description();
    char sn[CUSTOMER_PRODUCT_INFO_SN_LEN + 1] = {0};
    char colour[CUSTOMER_PRODUCT_INFO_TEXT_LEN + 1] = {0};
    char region[CUSTOMER_PRODUCT_INFO_TEXT_LEN + 1] = {0};
    customer_product_info_get_sn(sn, sizeof(sn));
    customer_product_info_get_colour_text(colour, sizeof(colour));
    customer_product_info_get_region_text(region, sizeof(region));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "node", identity_node_id());
    cJSON_AddStringToObject(root, "version", desc ? desc->version : "unknown");
    cJSON_AddStringToObject(root, "serial", sn[0] ? sn : "(none)");
    cJSON_AddStringToObject(root, "colour", colour[0] ? colour : "(none)");
    cJSON_AddStringToObject(root, "region", region[0] ? region : "(none)");
    cJSON_AddStringToObject(root, "verify", link_pairing_sign_factory_test());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return BLE_ATT_ERR_INSUFFICIENT_RES;

    ESP_LOGI(TAG, "device info read, button armed");
    int rc = os_mbuf_append(ctxt->om, json, strlen(json));
    free(json);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ft_button_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (!allowed_now()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    int rc = os_mbuf_append(ctxt->om, &s_button_value, 1);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def s_ft_chrs[] = {
    {
        .uuid = &FT_DEVINFO_UUID.u,
        .access_cb = ft_devinfo_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_devinfo_handle,
    },
    {
        .uuid = &FT_BUTTON_UUID.u,
        .access_cb = ft_button_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_button_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_ft_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &FT_SVC_UUID.u,
        .characteristics = s_ft_chrs,
    },
    { 0 },
};

const struct ble_gatt_svc_def *factory_test_svcs(void) {
    return s_ft_svcs;
}

void factory_test_on_button_press(void) {
    if (!s_armed) return;
    if (s_button_value != 0x00) return;
#if !CONFIG_MUSE_GADGET_AUTOMATION && !CONFIG_MUSE_GADGET_TEST_BUILD
    // A set-up release gadget's presses are the owner's: notified only while the service is open (unset, or the
    // owner's setup window). Bench, test and automation builds notify every press (a set-up bench unit or jig).
    if (!allowed_now()) {
        s_armed = false;
        return;
    }
#endif

    s_button_value = 0x01;
    ESP_LOGI(TAG, "button pressed, notifying");

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_button_value, 1);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_button_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "button notify rc=%d", rc);
    }
}

void factory_test_on_disconnect(void) {
    s_armed = false;
    s_button_value = 0x00;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}
