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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "nvs.h"

#define MAX_ENTRIES 24
#define KEY_BYTES 32
#define VALUE_BYTES 256

#define CHECK(cond, ...) do {                                                \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        exit(1);                                                             \
    }                                                                        \
} while (0)

typedef struct {
    bool used;
    char key[KEY_BYTES];
    char value[VALUE_BYTES];
} fake_entry_t;

typedef enum {
    PENDING_NONE,
    PENDING_SET,
    PENDING_ERASE,
} pending_kind_t;

static fake_entry_t s_entries[MAX_ENTRIES];
static pending_kind_t s_pending_kind;
static char s_pending_key[KEY_BYTES];
static char s_pending_value[VALUE_BYTES];
static esp_err_t s_open_error;
static const char *s_get_error_key;
static const char *s_commit_error_key;
static bool s_require_endpoints_erased_first;

static fake_entry_t *find_entry(const char *key) {
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static void fake_put(const char *key, const char *value) {
    fake_entry_t *entry = find_entry(key);
    if (!entry) {
        for (size_t i = 0; i < MAX_ENTRIES; i++) {
            if (!s_entries[i].used) {
                entry = &s_entries[i];
                entry->used = true;
                snprintf(entry->key, sizeof(entry->key), "%s", key);
                break;
            }
        }
    }
    CHECK(entry != NULL, "fake NVS is full");
    snprintf(entry->value, sizeof(entry->value), "%s", value);
}

static void fake_reset(void) {
    memset(s_entries, 0, sizeof(s_entries));
    s_pending_kind = PENDING_NONE;
    s_pending_key[0] = '\0';
    s_pending_value[0] = '\0';
    s_open_error = ESP_OK;
    s_get_error_key = NULL;
    s_commit_error_key = NULL;
    s_require_endpoints_erased_first = false;
}

esp_err_t nvs_flash_init(void) {
    return ESP_OK;
}

esp_err_t nvs_flash_init_partition(const char *part_name) {
    (void)part_name;
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle) {
    (void)namespace_name;
    (void)open_mode;
    if (s_open_error != ESP_OK) return s_open_error;
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    (void)handle;
    s_pending_kind = PENDING_NONE;
    s_pending_key[0] = '\0';
    s_pending_value[0] = '\0';
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value,
                      size_t *length) {
    (void)handle;
    if (s_get_error_key && strcmp(s_get_error_key, key) == 0) return ESP_FAIL;
    fake_entry_t *entry = find_entry(key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    size_t required = strlen(entry->value) + 1;
    if (!out_value) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) {
        *length = required;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->value, required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    (void)handle;
    s_pending_kind = PENDING_SET;
    snprintf(s_pending_key, sizeof(s_pending_key), "%s", key);
    snprintf(s_pending_value, sizeof(s_pending_value), "%s", value);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    (void)handle;
    if (s_require_endpoints_erased_first
        && strcmp(key, "api_url") != 0 && strcmp(key, "api_url_v2") != 0
        && strcmp(key, "noise_host") != 0) {
        CHECK(!find_entry("api_url") && !find_entry("api_url_v2")
              && !find_entry("noise_host"),
              "endpoint deletion must be committed before erasing %s", key);
    }
    if (!find_entry(key)) return ESP_ERR_NVS_NOT_FOUND;
    s_pending_kind = PENDING_ERASE;
    snprintf(s_pending_key, sizeof(s_pending_key), "%s", key);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    if (s_commit_error_key
        && strcmp(s_commit_error_key, s_pending_key) == 0) {
        return ESP_FAIL;
    }
    if (s_pending_kind == PENDING_SET) {
        fake_put(s_pending_key, s_pending_value);
    } else if (s_pending_kind == PENDING_ERASE) {
        fake_entry_t *entry = find_entry(s_pending_key);
        if (entry) memset(entry, 0, sizeof(*entry));
    }
    s_pending_kind = PENDING_NONE;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err) {
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        default: return "ESP_ERR_UNKNOWN";
    }
}

static void test_lookup_is_tri_state(void) {
    fake_reset();
    CHECK(config_key_lookup("device_state") == CONFIG_KEY_NOT_FOUND,
          "missing identity must be explicit NOT_FOUND");

    fake_put("device_state", "private-key");
    CHECK(config_key_lookup("device_state") == CONFIG_KEY_FOUND,
          "stored identity must be FOUND");

    s_get_error_key = "device_state";
    CHECK(config_key_lookup("device_state") == CONFIG_KEY_LOOKUP_ERROR,
          "read failure must not collapse to NOT_FOUND");
    s_get_error_key = NULL;

    s_open_error = ESP_FAIL;
    CHECK(config_key_lookup("device_state") == CONFIG_KEY_LOOKUP_ERROR,
          "open failure must not collapse to NOT_FOUND");
}

static void test_erase_propagates_commit_failure(void) {
    fake_reset();
    fake_put("access_token", "token");
    s_commit_error_key = "access_token";

    CHECK(!config_erase_key("access_token"),
          "erase must fail when nvs_commit fails");
    CHECK(config_key_lookup("access_token") == CONFIG_KEY_FOUND,
          "failed commit must leave the credential present");
}

static void test_setup_clear_verifies_credentials_and_preserves_identity(void) {
    static const char *const setup_keys[] = {
        "access_token", "refresh_token", "auth_token", "vm_url", "username",
        "pair_sdk_hint", "ssid", "password", "wifi_channel",
        "setup_complete", "api_url", "api_url_v2", "noise_host",
    };
    fake_reset();
    for (size_t i = 0; i < sizeof(setup_keys) / sizeof(setup_keys[0]); i++) {
        fake_put(setup_keys[i], "credential");
    }
    fake_put("device_state", "device-identity");

    CHECK(config_clear_setup(), "setup clear should succeed");
    for (size_t i = 0; i < sizeof(setup_keys) / sizeof(setup_keys[0]); i++) {
        CHECK(config_key_lookup(setup_keys[i]) == CONFIG_KEY_NOT_FOUND,
              "%s was not deleted", setup_keys[i]);
    }
    char identity[32] = {0};
    CHECK(config_get_str("device_state", identity, sizeof(identity)),
          "unrelated device state was deleted");
    CHECK(strcmp(identity, "device-identity") == 0,
          "unrelated device state changed");
}

static void test_pairing_clear_preserves_wifi_channel(void) {
    fake_reset();
    fake_put("access_token", "access");
    fake_put("pair_sdk_hint", "mgst_AAAAAAA");
    fake_put("ssid", "network");
    fake_put("password", "secret");
    fake_put("wifi_channel", "104");

    CHECK(config_clear_pairing(), "pairing clear should succeed");
    CHECK(config_key_lookup("access_token") == CONFIG_KEY_NOT_FOUND,
          "pairing token was not deleted");
    CHECK(config_key_lookup("pair_sdk_hint") == CONFIG_KEY_NOT_FOUND,
          "the SDK token pairing used was not forgotten");
    CHECK(config_key_lookup("ssid") == CONFIG_KEY_FOUND,
          "pairing clear deleted wifi credentials");
    CHECK(config_key_lookup("wifi_channel") == CONFIG_KEY_FOUND,
          "pairing clear deleted wifi channel");
}

static void test_setup_clear_fails_closed_on_uncommitted_credential(void) {
    fake_reset();
    fake_put("access_token", "access");
    fake_put("refresh_token", "refresh");
    fake_put("ssid", "network");
    fake_put("device_state", "device-identity");
    s_commit_error_key = "refresh_token";

    CHECK(!config_clear_setup(),
          "setup clear must fail when any credential erase is uncommitted");
    CHECK(config_key_lookup("refresh_token") == CONFIG_KEY_FOUND,
          "uncommitted credential unexpectedly disappeared");
    CHECK(config_key_lookup("device_state") == CONFIG_KEY_FOUND,
          "failed setup clear deleted unrelated device state");
}

static void test_unpair_clears_pairing_and_marker_but_keeps_wifi(void) {
    static const char *const pairing_keys[] = {
        "access_token", "refresh_token", "auth_token", "vm_url", "username",
        "pair_sdk_hint", "api_url", "api_url_v2", "noise_host", "setup_complete",
    };
    static const char *const wifi_keys[] = {
        "ssid", "password", "wifi_channel", "wifi_others", "wifi_hidden",
    };
    fake_reset();
    for (size_t i = 0; i < sizeof(pairing_keys) / sizeof(pairing_keys[0]); i++) {
        fake_put(pairing_keys[i], "credential");
    }
    for (size_t i = 0; i < sizeof(wifi_keys) / sizeof(wifi_keys[0]); i++) {
        fake_put(wifi_keys[i], "network");
    }
    fake_put("device_state", "device-identity");

    CHECK(config_unpair(), "unpair should succeed");
    for (size_t i = 0; i < sizeof(pairing_keys) / sizeof(pairing_keys[0]); i++) {
        CHECK(config_key_lookup(pairing_keys[i]) == CONFIG_KEY_NOT_FOUND,
              "%s was not deleted", pairing_keys[i]);
    }
    for (size_t i = 0; i < sizeof(wifi_keys) / sizeof(wifi_keys[0]); i++) {
        CHECK(config_key_lookup(wifi_keys[i]) == CONFIG_KEY_FOUND,
              "unpair deleted wifi key %s", wifi_keys[i]);
    }
    CHECK(config_key_lookup("device_state") == CONFIG_KEY_FOUND,
          "unpair deleted unrelated device state");
}

static void test_unpair_keeps_marker_until_pairing_is_gone(void) {
    fake_reset();
    fake_put("access_token", "access");
    fake_put("refresh_token", "refresh");
    fake_put("setup_complete", "1");
    s_commit_error_key = "refresh_token";

    // A failed unpair must leave the device paired, marker included, rather
    // than tokens without it (which boot would mark complete again).
    CHECK(!config_unpair(), "unpair must fail when a token erase is uncommitted");
    CHECK(config_key_lookup("refresh_token") == CONFIG_KEY_FOUND,
          "uncommitted credential unexpectedly disappeared");
    CHECK(config_key_lookup("setup_complete") == CONFIG_KEY_FOUND,
          "marker cleared while pairing credentials remain");
}

static void test_reset_clears_endpoints_before_credentials(void) {
    bool (*const resetters[])(void) = {config_clear_pairing, config_unpair,
                                       config_clear_setup};
    for (size_t i = 0; i < sizeof(resetters) / sizeof(resetters[0]); i++) {
        fake_reset();
        fake_put("api_url", "https://previous.example");
        fake_put("api_url_v2", "https://previous-v2.example");
        fake_put("noise_host", "previous.example");
        fake_put("access_token", "access");
        fake_put("setup_complete", "1");
        s_require_endpoints_erased_first = true;

        CHECK(resetters[i](), "reset should succeed");
    }
}

static const char *value_of(const char *key) {
    fake_entry_t *entry = find_entry(key);
    return entry ? entry->value : NULL;
}

// The device session's credentials_update: the
// pair in one commit through the re-provisioning journal, K and node_id
// untouched (link_reprovision_journal_harness.c cuts the power at each write).
static void test_token_pair_is_one_update(void) {
    fake_reset();
    fake_put("access_token", "old-access");
    fake_put("refresh_token", "old-refresh");
    fake_put("mg_proof_k", "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    fake_put("node_id", "homelink-1a2b3c");
    CHECK(config_set_token_pair("new-access", "new-refresh", 0), "a token pair update must store");
    CHECK(value_of("access_token") && strcmp(value_of("access_token"), "new-access") == 0, "access not stored");
    CHECK(value_of("refresh_token") && strcmp(value_of("refresh_token"), "new-refresh") == 0, "refresh not stored");
    CHECK(strncmp(value_of("mg_proof_k"), "0011", 4) == 0 && strcmp(value_of("node_id"), "homelink-1a2b3c") == 0,
          "K and node_id must stay");
    CHECK(!find_entry("rp_open") && !find_entry("rp_at") && !find_entry("rp_k") && !find_entry("tok_update"),
          "the journal must be closed and its copies gone");
    CHECK(!config_set_token_pair("", "r", 0) && !config_set_token_pair("a", NULL, 0), "an empty token must be refused");

    // A write failing: refused, the old pair back, the journal closed.
    const char *failing[] = {"refresh_token", "access_token", "rp_at", "rp_open"};
    for (size_t f = 0; f < sizeof(failing) / sizeof(failing[0]); f++) {
        s_commit_error_key = failing[f];
        CHECK(!config_set_token_pair("third-access", "third-refresh", 0), "a failed write must fail the update");
        char at[64], rt[64];
        CHECK(config_get_token_pair(at, sizeof(at), rt, sizeof(rt)) && strcmp(at, "new-access") == 0
              && strcmp(rt, "new-refresh") == 0, "a failed update must be read as the old pair");
        s_commit_error_key = NULL;
        (void)config_reprovision_recover();   // boot: a rollback NVS refused finishes
        CHECK(strcmp(value_of("access_token"), "new-access") == 0
              && strcmp(value_of("refresh_token"), "new-refresh") == 0, "a failed update must leave the old pair");
        CHECK(!find_entry("rp_open"), "a failed update must close the journal");
    }
    // A re-setup's journal is open: refused, nothing touched.
    fake_put("rp_open", "1");
    CHECK(!config_set_token_pair("fourth-access", "fourth-refresh", 0), "refused while a journal is open");
    CHECK(strcmp(value_of("access_token"), "new-access") == 0 && strcmp(value_of("rp_open"), "1") == 0,
          "a refused update must change nothing");
    CHECK(config_erase_key("rp_open"), "erase");

    // A pair journalled by a build before 3.1 ("tok_update"): boot finishes it.
    fake_put("tok_update", "8:access-4refresh-4");
    config_store_init();
    CHECK(strcmp(value_of("access_token"), "access-4") == 0 && strcmp(value_of("refresh_token"), "refresh-4") == 0,
          "boot must apply a journalled pair");
    // A journal that can't be read is dropped, and changes nothing.
    fake_put("tok_update", "99:short");
    config_store_init();
    CHECK(!find_entry("tok_update") && strcmp(value_of("access_token"), "access-4") == 0,
          "an unreadable journal must be dropped without effect");
    // Reset erases a pending pair with the rest of the credentials.
    fake_put("tok_update", "1:ab");
    CHECK(config_clear_setup() && !find_entry("tok_update"), "setup clear must erase the journal");
    fake_put("tok_update", "1:ab");
    CHECK(config_clear_pairing() && !find_entry("tok_update"), "pairing clear must erase the journal");
}

static void test_token_pair_reader_never_mixes_pairs(void) {
    char access[64], refresh[64];
    fake_reset();
    CHECK(!config_get_token_pair(access, sizeof(access), refresh, sizeof(refresh)), "no pair, nothing read");
    fake_put("access_token", "old-access");
    fake_put("refresh_token", "old-refresh");
    CHECK(config_get_token_pair(access, sizeof(access), refresh, sizeof(refresh))
          && strcmp(access, "old-access") == 0 && strcmp(refresh, "old-refresh") == 0, "the stored pair");

    // NVS refuses access_token after refresh_token took the new token: the
    // update rolls back, and the reader gets the whole old pair.
    s_commit_error_key = "access_token";
    CHECK(!config_set_token_pair("new-access", "new-refresh", 0), "the update must fail");
    CHECK(config_get_token_pair(access, sizeof(access), refresh, sizeof(refresh))
          && strcmp(access, "old-access") == 0 && strcmp(refresh, "old-refresh") == 0,
          "while NVS refuses the rollback, the kept-aside pair is read");
    s_commit_error_key = NULL;
    (void)config_reprovision_recover();   // boot
    CHECK(config_get_token_pair(access, sizeof(access), refresh, sizeof(refresh))
          && strcmp(access, "old-access") == 0 && strcmp(refresh, "old-refresh") == 0,
          "a failed update must be read as the old pair, whole");

    // A pre-3.1 journal half applied (refresh_token new, access_token old)
    // while NVS still refuses access_token: read whole from the journal.
    fake_put("refresh_token", "new-refresh");
    fake_put("tok_update", "10:new-accessnew-refresh");
    s_commit_error_key = "access_token";
    CHECK(config_get_token_pair(access, sizeof(access), refresh, sizeof(refresh))
          && strcmp(access, "new-access") == 0 && strcmp(refresh, "new-refresh") == 0,
          "a half-applied update must be read whole from its journal");
    CHECK(!config_get_token_pair(access, 5, refresh, sizeof(refresh)), "a token that doesn't fit isn't cut");
    // Once NVS takes it, reading finishes the update.
    s_commit_error_key = NULL;
    CHECK(config_get_token_pair(access, sizeof(access), refresh, sizeof(refresh))
          && strcmp(access, "new-access") == 0 && !find_entry("tok_update")
          && strcmp(value_of("access_token"), "new-access") == 0, "reading must finish the update");
}

/* An unreadable setup marker never reads as "unset" to who fails closed. */
static void test_setup_marker_fails_closed(void) {
    fake_reset();
    CHECK(!config_setup_complete_or_unreadable(), "no marker is unset");
    fake_put("setup_complete", "1");
    CHECK(config_setup_complete_or_unreadable(), "the marker is set up");
    s_get_error_key = "setup_complete";
    CHECK(config_setup_complete_or_unreadable(), "a marker that can't be read must read as set up");
    CHECK(!config_setup_complete(), "config_setup_complete itself is unchanged (false on an error)");
    s_get_error_key = NULL;
    s_open_error = ESP_FAIL;
    CHECK(config_setup_complete_or_unreadable(), "a namespace that can't be opened must read as set up");
    s_open_error = ESP_OK;
}

int main(void) {
    test_setup_marker_fails_closed();
    test_lookup_is_tri_state();
    test_erase_propagates_commit_failure();
    test_token_pair_is_one_update();
    test_token_pair_reader_never_mixes_pairs();
    test_setup_clear_verifies_credentials_and_preserves_identity();
    test_pairing_clear_preserves_wifi_channel();
    test_setup_clear_fails_closed_on_uncommitted_credential();
    test_unpair_clears_pairing_and_marker_but_keeps_wifi();
    test_unpair_keeps_marker_until_pairing_is_gone();
    test_reset_clears_endpoints_before_credentials();
    puts("config_store harness: PASS");
    return 0;
}
