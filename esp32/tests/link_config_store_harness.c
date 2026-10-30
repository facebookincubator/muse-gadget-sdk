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

static void test_reset_clears_endpoints_before_credentials(void) {
    bool (*const resetters[])(void) = {config_clear_pairing, config_clear_setup};
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

int main(void) {
    test_lookup_is_tri_state();
    test_erase_propagates_commit_failure();
    test_setup_clear_verifies_credentials_and_preserves_identity();
    test_pairing_clear_preserves_wifi_channel();
    test_setup_clear_fails_closed_on_uncommitted_credential();
    test_reset_clears_endpoints_before_credentials();
    puts("config_store harness: PASS");
    return 0;
}
