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

/*
 * Node ids: main/identity.c on the real main/config_store.c, over a fake NVS
 * that keeps its contents across "reboots" (identity_init() again). A new
 * setup offers gadget-XXXXXX with CONFIG_MUSE_GADGET_NODE_ID_PREFIX
 * (GADGET_IDS), else homelink-XXXXXX, and stores it with its tokens; a device set up
 * by earlier firmware, which stored no id, keeps homelink-XXXXXX through
 * reboots, updates and token refreshes until it is set up again. A stored id
 * is never renamed. XXXXXX is the Wi-Fi STA MAC's last three octets,
 * lowercase, the BLE name's digits.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_mac.h"
#include "identity.h"
#include "nvs.h"

#define CHECK(cond, ...) do {                                                \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        exit(1);                                                             \
    }                                                                        \
} while (0)

#define MAX_ENTRIES 64
#define KEY_BYTES 16
#define VALUE_BYTES 256

static struct {
    bool used;
    char key[KEY_BYTES];
    char value[VALUE_BYTES];
} s_nvs[MAX_ENTRIES];

static int find(const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) return i;
    }
    return -1;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)ns;
    (void)mode;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char *part) { (void)part; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    (void)h;
    int i = find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = strlen(s_nvs[i].value) + 1;
    if (!out) {
        *len = need;
        return ESP_OK;
    }
    if (*len < need) {
        *len = need;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, s_nvs[i].value, need);
    *len = need;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    (void)h;
    int i = find(key);
    for (int j = 0; i < 0 && j < MAX_ENTRIES; j++) {
        if (!s_nvs[j].used) i = j;
    }
    CHECK(i >= 0 && strlen(key) < KEY_BYTES && strlen(value) < VALUE_BYTES, "fake NVS: %s", key);
    s_nvs[i].used = true;
    snprintf(s_nvs[i].key, sizeof(s_nvs[i].key), "%s", key);
    snprintf(s_nvs[i].value, sizeof(s_nvs[i].value), "%s", value);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    int i = find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    memset(&s_nvs[i], 0, sizeof(s_nvs[i]));
    return ESP_OK;
}

// The Wi-Fi STA MAC ends ab:cd:ef (lowercase in the node id, uppercase in
// the name: the same digits).
esp_err_t esp_read_mac(uint8_t *mac, int type) {
    memset(mac, 0, 6);
    if (type != ESP_MAC_WIFI_STA) return ESP_FAIL;
    mac[3] = 0xab, mac[4] = 0xcd, mac[5] = 0xef;
    return ESP_OK;
}

#define NEW_ID "gadget-abcdef"
#define OLD_ID "homelink-abcdef"

static const char *nvs(const char *key) {
    int i = find(key);
    return i < 0 ? NULL : s_nvs[i].value;
}

static void wipe(void) { memset(s_nvs, 0, sizeof(s_nvs)); }

static void boot(void) { identity_init(); }

static void expect(const char *in_use, const char *when) {
    CHECK(strcmp(identity_node_id(), in_use) == 0, "%s: node id %s, not %s", when, identity_node_id(), in_use);
}

// What a setup stores (app.c, accept_pairing_credentials() and the commit):
// the id device_info offered first, then the tokens, the setup marker; then
// the refresh.
static void set_up(const char *access) {
    CHECK(config_set_str("node_id", identity_setup_node_id()) && config_set_str("access_token", access)
              && config_set_str("refresh_token", "refresh") && config_mark_setup_complete(),
          "setup writes");
    (void)identity_refresh_node_id();
}

// A device set up by earlier firmware: tokens, no stored id.
static void old_setup(void) {
    wipe();
    config_set_str("ssid", "home");
    config_set_str("access_token", "old-access");
    config_set_str("refresh_token", "old-refresh");
    config_set_str("setup_complete", "1");
}

#ifdef GADGET_IDS
static void test_new_setup_takes_gadget_id(void) {
    wipe();
    boot();
    CHECK(strcmp(identity_setup_node_id(), NEW_ID) == 0, "setup id %s", identity_setup_node_id());
    expect(NEW_ID, "not set up");
    CHECK(strcmp(identity_ble_name(), "MuseGadget-ABCDEF") == 0, "name %s", identity_ble_name());
    set_up("access");
    CHECK(strcmp(nvs("node_id"), NEW_ID) == 0, "stored %s", nvs("node_id"));
    expect(NEW_ID, "set up");
    boot();
    expect(NEW_ID, "reboot after setup");
    // A token refresh leaves the id alone.
    CHECK(config_set_str("access_token", "refreshed") && config_set_str("refresh_token", "r2"), "refresh");
    CHECK(!identity_refresh_node_id(), "a refresh changed the id");
    expect(NEW_ID, "after a refresh");
}

static void test_earlier_setup_keeps_homelink(void) {
    old_setup();
    boot();
    expect(OLD_ID, "boot");
    CHECK(strcmp(identity_setup_node_id(), NEW_ID) == 0, "setup id %s", identity_setup_node_id());
    boot();   // an update's first boot reads the same NVS
    expect(OLD_ID, "reboot / OTA");
    CHECK(config_set_str("access_token", "refreshed") && config_set_str("refresh_token", "r2"), "refresh");
    CHECK(!identity_refresh_node_id(), "a refresh changed the id");
    boot();
    expect(OLD_ID, "reboot after a refresh");
    CHECK(!nvs("node_id"), "an id was stored for it");
    wipe();
    config_set_str("auth_token", "legacy");   // the oldest credentials
    boot();
    expect(OLD_ID, "legacy auth token");
}

static void test_stored_id_never_renamed(void) {
    wipe();
    config_set_str("node_id", OLD_ID);
    config_set_str("access_token", "access");
    boot();
    expect(OLD_ID, "stored homelink- id");
    wipe();
    config_set_str("node_id", NEW_ID);
    config_set_str("access_token", "access");
    boot();
    expect(NEW_ID, "stored gadget- id");
}

static void test_set_up_again_takes_gadget_id(void) {
    old_setup();
    boot();
    set_up("second-access");
    expect(NEW_ID, "set up again");
    boot();
    expect(NEW_ID, "reboot");
}

static void test_reset_or_unpair_then_setup_takes_gadget_id(void) {
    old_setup();
    boot();
    CHECK(config_clear_setup(), "reset");
    boot();
    expect(NEW_ID, "after reset");
    set_up("access");
    expect(NEW_ID, "setup after reset");

    set_up("access");
    CHECK(config_unpair(), "unpair");
    CHECK(!nvs("node_id"), "unpair kept the node id");
    CHECK(identity_refresh_node_id() || strcmp(identity_node_id(), NEW_ID) == 0, "refresh");
    expect(NEW_ID, "unpaired");
    old_setup();
    boot();
    CHECK(config_clear_pairing(), "credentials cleared");
    CHECK(identity_refresh_node_id(), "cleared credentials kept the homelink- id");
    expect(NEW_ID, "credentials cleared");
}

#else
// Gadget ids off (the default): a new setup offers homelink- as before and
// stores it; an id stored with gadget ids on is kept until the next setup.
static void test_off_offers_homelink(void) {
    wipe();
    boot();
    CHECK(strcmp(identity_setup_node_id(), OLD_ID) == 0, "setup id %s", identity_setup_node_id());
    expect(OLD_ID, "not set up");
    set_up("access");
    CHECK(strcmp(nvs("node_id"), OLD_ID) == 0, "stored %s", nvs("node_id"));
    boot();
    expect(OLD_ID, "reboot after setup");
}

static void test_off_keeps_a_stored_gadget_id(void) {
    old_setup();
    config_set_str("node_id", NEW_ID);
    boot();
    expect(NEW_ID, "boot");
    CHECK(config_set_str("access_token", "refreshed") && config_set_str("refresh_token", "r2"), "refresh");
    CHECK(!identity_refresh_node_id(), "a refresh changed the id");
    boot();
    expect(NEW_ID, "reboot after a refresh");
    set_up("second-access");
    expect(OLD_ID, "set up again");
}
#endif

int main(void) {
#ifndef GADGET_IDS
    test_off_offers_homelink();
    test_off_keeps_a_stored_gadget_id();
    puts("node id harness (gadget ids off): PASS");
#else
    test_new_setup_takes_gadget_id();
    test_earlier_setup_keeps_homelink();
    test_stored_id_never_renamed();
    test_set_up_again_takes_gadget_id();
    test_reset_or_unpair_then_setup_takes_gadget_id();
    puts("node id harness: PASS");
#endif
    return 0;
}
