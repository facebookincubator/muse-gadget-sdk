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
 * Node ids: main/identity.c on the real
 * main/config_store.c, over a fake NVS that keeps its contents across
 * "reboots" (identity_init() again). With CONFIG_MUSE_GADGET_NODE_ID_PREFIX
 * (GADGET_IDS) a Muse Gadget build's new setup offers gadget-XXXXXX, else
 * homelink-XXXXXX (as a plain Home Link always does), and stores it with its
 * tokens. A device set up earlier keeps its stored id
 * (homelink-XXXXXX, or gadget-XXXXXX from a bench image), or the
 * homelink-XXXXXX a setup that stored none was issued tokens for, through
 * reboots, updates and token refreshes until a setup commits a new one. A
 * stored id is never renamed.
 *
 * A setup stores the id before its tokens (app.c accept_pairing_credentials());
 * a Muse Gadget build (GADGET_BUILD) also commits it with K through the
 * re-provisioning journal (commit_setup()): an abandoned or power-cut
 * re-setup keeps the old id, and the device session's handover names the
 * current pair's. XXXXXX is the Wi-Fi STA MAC's last three octets,
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
// the name: the same digits), the Bluetooth address 44:55:66.
esp_err_t esp_read_mac(uint8_t *mac, int type) {
    memset(mac, 0, 6);
    if (type == ESP_MAC_WIFI_STA) {
        mac[3] = 0xab, mac[4] = 0xcd, mac[5] = 0xef;
    } else if (type == ESP_MAC_BT) {
        mac[3] = 0x44, mac[4] = 0x55, mac[5] = 0x66;
    } else {
        return ESP_FAIL;
    }
    return ESP_OK;
}

#define NEW_ID "gadget-abcdef"
#define OLD_ID "homelink-abcdef"
#define K_HEX "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

static const char *nvs(const char *key) {
    int i = find(key);
    return i < 0 ? NULL : s_nvs[i].value;
}

static void wipe(void) { memset(s_nvs, 0, sizeof(s_nvs)); }

// A boot (or the first boot of an update): boot recovery, then the identity,
// in app_run()'s order.
static void boot(void) {
    (void)config_reprovision_recover();
    identity_init();
}

static void expect(const char *in_use, const char *when) {
    CHECK(strcmp(identity_node_id(), in_use) == 0, "%s: node id %s, not %s", when, identity_node_id(), in_use);
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
static void earlier_setup(void) {
    wipe();
    config_set_str("ssid", "home");
    config_set_str("access_token", "old-access");
    config_set_str("refresh_token", "old-refresh");
    config_set_str("setup_complete", "1");
}

#if defined(GADGET_BUILD) && defined(GADGET_IDS)
// A gadget's setup with gadget ids on, storing the id before its tokens
// (accept_pairing_credentials()).
static void test_setup_storing_the_id_first_takes_gadget_id(void) {
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
    earlier_setup();
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

static void test_accepted_setup_again_takes_gadget_id(void) {
    earlier_setup();
    boot();
    set_up("second-access");
    expect(NEW_ID, "set up again");
    boot();
    expect(NEW_ID, "reboot");
}

static void test_reset_or_unpair_then_setup_takes_gadget_id(void) {
    earlier_setup();
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
    earlier_setup();
    boot();
    CHECK(config_clear_pairing(), "credentials cleared");
    CHECK(identity_refresh_node_id(), "cleared credentials kept the homelink- id");
    expect(NEW_ID, "credentials cleared");
}
#else
// Homelink- offered (a gadget release image, CONFIG_MUSE_GADGET_NODE_ID_PREFIX
// off, or a plain Home Link with or without it): a new setup offers
// homelink- as before and stores it; an id stored with gadget ids on is kept
// until the next setup.
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
    earlier_setup();
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

#ifdef GADGET_BUILD
// What commit_setup() stores (app.c): the pair, K, the id device_info
// offered, the setup marker; then the journal's end and the refresh.
static void commit_setup(const char *access, bool again) {
    CHECK(config_set_str("refresh_token", "refresh") && config_set_str("access_token", access)
              && config_set_str("mg_proof_k", K_HEX) && config_set_str("node_id", identity_setup_node_id())
              && config_mark_setup_complete(),
          "setup writes");
    if (again) CHECK(config_reprovision_commit(), "journal commit");
    (void)identity_refresh_node_id();
}

// A gadget set up by firmware before gadget ids: its homelink- id
// stored with K.
static void old_setup(void) {
    wipe();
    config_set_str("access_token", "old-access");
    config_set_str("refresh_token", "old-refresh");
    config_set_str("mg_proof_k", K_HEX);
    config_set_str("node_id", OLD_ID);
    config_set_str("setup_complete", "1");
}

// The node id the device session's credentials hand over (mg_glue.c,
// session_credentials()): the one stored with the current pair, else the id
// in use.
static const char *handover_node(void) {
    static char node[64];
    if (!config_get_node_id(node, sizeof(node))) snprintf(node, sizeof(node), "%s", identity_node_id());
    return node;
}

#ifdef GADGET_IDS
static void test_new_setup_takes_gadget_id(void) {
    wipe();
    boot();
    CHECK(strcmp(identity_setup_node_id(), NEW_ID) == 0, "setup id %s", identity_setup_node_id());
    expect(NEW_ID, "not set up");
    CHECK(strcmp(identity_ble_name(), "MuseGadget-ABCDEF") == 0, "name %s", identity_ble_name());
    commit_setup("access", false);
    CHECK(strcmp(nvs("node_id"), NEW_ID) == 0, "stored %s", nvs("node_id"));
    expect(NEW_ID, "set up");
    boot();
    expect(NEW_ID, "reboot after setup");
}

static void test_stored_homelink_id_kept(void) {
    old_setup();
    boot();
    expect(OLD_ID, "boot");
    CHECK(strcmp(identity_setup_node_id(), NEW_ID) == 0, "setup id %s", identity_setup_node_id());
    boot();   // an update's first boot reads the same NVS
    expect(OLD_ID, "reboot / OTA");
    // A credentials update (the device session's, or a refresh): the pair
    // changes in one journalled commit, K and the id stay.
    CHECK(config_set_token_pair("new-access", "new-refresh", 0), "token update");
    CHECK(!identity_refresh_node_id(), "a token update changed the id");
    expect(OLD_ID, "credentials update");
    boot();
    expect(OLD_ID, "reboot after credentials update");
    CHECK(strcmp(nvs("node_id"), OLD_ID) == 0, "stored id renamed to %s", nvs("node_id"));
}

static void test_credentials_without_stored_id_are_homelink(void) {
    // Set up before the id was stored, or without a proof key: the tokens
    // were issued for the homelink- id.
    wipe();
    config_set_str("access_token", "access");
    config_set_str("setup_complete", "1");
    boot();
    expect(OLD_ID, "access token, no stored id");
    wipe();
    config_set_str("auth_token", "legacy");
    boot();
    expect(OLD_ID, "legacy auth token, no stored id");
}

static void test_setup_again_takes_gadget_id(void) {
    old_setup();
    boot();
    CHECK(config_reprovision_begin(), "journal begin");
    expect(OLD_ID, "during a re-setup, before its commit");
    commit_setup("second-access", true);
    expect(NEW_ID, "re-setup committed");
    boot();
    expect(NEW_ID, "reboot after re-setup");
}

static void test_abandoned_setup_again_keeps_old_id(void) {
    old_setup();
    boot();
    CHECK(config_reprovision_begin(), "journal begin");
    config_set_str("access_token", "second-access");
    config_set_str("node_id", NEW_ID);
    CHECK(config_reprovision_rollback(), "rollback");
    CHECK(!identity_refresh_node_id(), "a rolled-back setup changed the id");
    expect(OLD_ID, "rolled back");
    // A power cut before the commit: boot recovery puts the old setup back.
    CHECK(config_reprovision_begin(), "journal begin");
    config_set_str("access_token", "third-access");
    config_set_str("node_id", NEW_ID);
    boot();
    expect(OLD_ID, "power cut before the commit");
    CHECK(strcmp(nvs("node_id"), OLD_ID) == 0, "stored %s", nvs("node_id"));
}

static void test_handover_names_the_current_pairs_id(void) {
    // A homelink- gadget being set up again: device_info offers gadget-, the
    // re-setup has written its id, but until the commit the handover names
    // the id the current pair is for.
    old_setup();
    boot();
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "set up: %s", handover_node());
    CHECK(config_reprovision_begin(), "journal begin");
    CHECK(config_set_str("access_token", "second-access") && config_set_str("node_id", identity_setup_node_id()),
          "re-setup writes");
    CHECK(strcmp(identity_setup_node_id(), NEW_ID) == 0, "offer %s", identity_setup_node_id());
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "mid re-setup: %s", handover_node());
    commit_setup("second-access", true);
    CHECK(strcmp(handover_node(), NEW_ID) == 0, "committed: %s", handover_node());

    // Abandoned: still the old id.
    old_setup();
    boot();
    CHECK(config_reprovision_begin(), "journal begin");
    CHECK(config_set_str("node_id", NEW_ID), "re-setup write");
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "mid re-setup: %s", handover_node());
    CHECK(config_reprovision_rollback(), "rollback");
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "abandoned: %s", handover_node());

    // Set up before ids were stored: the homelink- id throughout a re-setup,
    // gadget- once it commits.
    old_setup();
    config_erase_key("node_id");
    boot();
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "no stored id: %s", handover_node());
    CHECK(config_reprovision_begin(), "journal begin");
    CHECK(config_set_str("node_id", NEW_ID), "re-setup write");
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "no stored id, mid re-setup: %s", handover_node());
    commit_setup("second-access", true);
    CHECK(strcmp(handover_node(), NEW_ID) == 0, "no stored id, committed: %s", handover_node());
}

static void test_reset_then_setup_takes_gadget_id(void) {
    old_setup();
    boot();
    CHECK(config_clear_setup(), "reset");
    boot();   // a setup reset restarts
    expect(NEW_ID, "after reset");
    commit_setup("access", false);
    expect(NEW_ID, "new setup after reset");
    boot();
    expect(NEW_ID, "reboot");
}

static void test_cleared_pairing_offers_gadget_id(void) {
    // Credentials revoked (config_clear_pairing): nothing is set up for the
    // old id any more, so the next setup and the id in use are gadget-.
    old_setup();
    boot();
    CHECK(config_clear_pairing(), "clear pairing");
    CHECK(identity_refresh_node_id(), "a cleared pairing kept the old id");
    expect(NEW_ID, "cleared pairing");
}
#else
// A release image (CONFIG_MUSE_GADGET_NODE_ID_PREFIX off): new setups offer
// homelink- as before; the rest is the same, so an id stored by a bench image
// (gadget-) survives too, until the next setup.
static void test_release_offers_homelink(void) {
    wipe();
    boot();
    CHECK(strcmp(identity_setup_node_id(), OLD_ID) == 0, "setup id %s", identity_setup_node_id());
    expect(OLD_ID, "not set up");
    commit_setup("access", false);
    CHECK(strcmp(nvs("node_id"), OLD_ID) == 0, "stored %s", nvs("node_id"));   // stored either way
    boot();
    expect(OLD_ID, "reboot after setup");
}

static void test_release_keeps_a_stored_gadget_id(void) {
    old_setup();
    config_set_str("node_id", NEW_ID);   // set up by a bench image
    boot();
    expect(NEW_ID, "boot");
    CHECK(config_set_token_pair("new-access", "new-refresh", 0), "token update");
    CHECK(!identity_refresh_node_id(), "a token update changed the id");
    boot();
    expect(NEW_ID, "reboot after credentials update");
    CHECK(strcmp(handover_node(), NEW_ID) == 0, "handover %s", handover_node());
    // Set up again: the handover names the current pair's id until the
    // commit, then the setup's (homelink- here).
    CHECK(config_reprovision_begin(), "journal begin");
    CHECK(config_set_str("node_id", identity_setup_node_id()), "re-setup write");
    CHECK(strcmp(handover_node(), NEW_ID) == 0, "mid re-setup: %s", handover_node());
    commit_setup("second-access", true);
    expect(OLD_ID, "re-setup committed");
    CHECK(strcmp(handover_node(), OLD_ID) == 0, "committed: %s", handover_node());
}
#endif
#else
// A plain Home Link: credentials from a setup that stored no id are the
// homelink- id, as on a gadget.
static void test_off_credentials_without_stored_id_are_homelink(void) {
    wipe();
    boot();
    CHECK(strcmp(identity_setup_node_id(), OLD_ID) == 0, "setup id %s", identity_setup_node_id());
    expect(OLD_ID, "not set up");
    config_set_str("access_token", "access");
    config_set_str("setup_complete", "1");
    boot();
    expect(OLD_ID, "set up, no stored id");
}
#endif

int main(void) {
#if defined(GADGET_BUILD) && !defined(GADGET_IDS)
    test_release_offers_homelink();
    test_release_keeps_a_stored_gadget_id();
    test_off_offers_homelink();
    test_off_keeps_a_stored_gadget_id();
    test_stored_id_never_renamed();
    puts("node id harness (gadget, release): PASS");
#elif defined(GADGET_BUILD)
    test_new_setup_takes_gadget_id();
    test_stored_homelink_id_kept();
    test_credentials_without_stored_id_are_homelink();
    test_stored_id_never_renamed();
    test_setup_again_takes_gadget_id();
    test_abandoned_setup_again_keeps_old_id();
    test_handover_names_the_current_pairs_id();
    test_reset_then_setup_takes_gadget_id();
    test_cleared_pairing_offers_gadget_id();
    test_setup_storing_the_id_first_takes_gadget_id();
    test_earlier_setup_keeps_homelink();
    test_accepted_setup_again_takes_gadget_id();
    test_reset_or_unpair_then_setup_takes_gadget_id();
    puts("node id harness (gadget): PASS");
#else
    // A plain Home Link offers homelink- with CONFIG_MUSE_GADGET_NODE_ID_PREFIX
    // on too.
    test_off_offers_homelink();
    test_off_keeps_a_stored_gadget_id();
    test_off_credentials_without_stored_id_are_homelink();
    test_stored_id_never_renamed();
#ifdef GADGET_IDS
    puts("node id harness (Home Link, gadget ids on): PASS");
#else
    puts("node id harness (Home Link): PASS");
#endif
#endif
    return 0;
}
