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

#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

static const char *TAG = "link.config";
static const char *NS = "homehub";
static const char *SETUP_COMPLETE_KEY = "setup_complete";
static const char *SETUP_WIFI_KEY = "setup_wifi";
// A token pair on its way into access_token and refresh_token:
// "<access length>:<access><refresh>" (config_set_token_pair()).
static const char *TOKEN_UPDATE_KEY = "tok_update";
// The longest token pair update, as tokens are stored (app.c's TOKEN_BUF_BYTES).
#define TOKEN_UPDATE_MAX 4000

static void wipe(void *p, size_t n) {
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
}

// Splits a journal in place: access is *alen bytes, refresh runs to the end.
// False if it can't be read.
static bool split_token_update(char *journal, char **access, size_t *alen, char **refresh) {
    char *rest = NULL;
    unsigned long n = strtoul(journal, &rest, 10);
    size_t left = rest && *rest == ':' ? strlen(rest + 1) : 0;
    if (n == 0 || n >= left) return false;
    *access = rest + 1;
    *alen = n;
    *refresh = rest + 1 + n;
    return true;
}

// Copies a journalled pair into its keys, refresh_token first, then drops the
// journal. True once both keys hold it; the journal stays until they do.
static bool apply_token_update(void) {
    char *journal = calloc(1, TOKEN_UPDATE_MAX);
    if (!journal) return false;
    bool ok = false;
    if (config_get_str(TOKEN_UPDATE_KEY, journal, TOKEN_UPDATE_MAX)) {
        char *access, *refresh;
        size_t alen;
        if (split_token_update(journal, &access, &alen, &refresh)) {
            char saved = *refresh;
            ok = config_set_str("refresh_token", refresh);
            *refresh = '\0';
            ok = ok && config_set_str("access_token", access);
            *refresh = saved;
            // Done only once the journal is gone: one left behind would be applied again later, over a
            // newer pair.
            ok = ok && config_erase_key(TOKEN_UPDATE_KEY);
        } else {
            ESP_LOGW(TAG, "token update journal unreadable; dropping it");
            config_erase_key(TOKEN_UPDATE_KEY);
        }
    }
    wipe(journal, TOKEN_UPDATE_MAX);
    free(journal);
    return ok;
}

// The re-provisioning journal (config_store.h): Link's keys a provisioning
// may overwrite before its commit, each with the key that holds its old
// value meanwhile, and the marker that makes the copies count.
static const char *const REPROVISION_KEYS[][2] = {
    {"access_token", "rp_at"},
    {"refresh_token", "rp_rt"},
    {"auth_token", "rp_auth"},
    {"mg_proof_k", "rp_k"},
    {"setup_wifi", "rp_wifi"},
    {"username", "rp_user"},
    {"api_url", "rp_api"},
    {"api_url_v2", "rp_api2"},
    {"noise_host", "rp_noise"},
    {"pair_sdk_hint", "rp_hint"},
    {"vm_url", "rp_vm"},
    {"node_id", "rp_node"},
    {"tok_exp", "rp_exp"},   // the pair's access_expires_at, which goes with it
};
#define REPROVISION_COUNT (sizeof(REPROVISION_KEYS) / sizeof(REPROVISION_KEYS[0]))
static const char *REPROVISION_OPEN_KEY = "rp_open";
// The journalled pair itself, for a reader while it can't be applied.
static bool read_token_update(char *access_token, size_t access_size, char *refresh_token, size_t refresh_size) {
    char *journal = calloc(1, TOKEN_UPDATE_MAX);
    if (!journal) return false;
    bool ok = false;
    char *access, *refresh;
    size_t alen;
    if (config_get_str(TOKEN_UPDATE_KEY, journal, TOKEN_UPDATE_MAX)
        && split_token_update(journal, &access, &alen, &refresh)
        && alen < access_size && strlen(refresh) < refresh_size) {
        memcpy(access_token, access, alen);
        access_token[alen] = '\0';
        strcpy(refresh_token, refresh);
        ok = true;
    }
    wipe(journal, TOKEN_UPDATE_MAX);
    free(journal);
    return ok;
}

static bool nvs_encryption_possible(void) {
#ifndef CONFIG_NVS_ENCRYPTION
    ESP_LOGI(TAG, "NVS encryption not compiled in");
    return false;
#else
    esp_efuse_block_t hmac_blk =
        (esp_efuse_block_t)(EFUSE_BLK_KEY0 + CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
    if (esp_efuse_get_key_purpose(hmac_blk) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        ESP_LOGI(TAG, "HMAC key present in eFuse block %d", CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
        return true;
    }
    ESP_LOGI(TAG, "no HMAC key in eFuse block %d", CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
    if (esp_efuse_read_field_bit(ESP_EFUSE_WR_DIS_RD_DIS)) {
        ESP_LOGE(TAG, "NVS encryption is configured but the eFuse state cannot "
                 "support it; refusing to fall back to plaintext");
        abort();
    }
    ESP_LOGI(TAG, "eFuse read-protection available; HMAC key will be generated");
    return true;
#endif
}

void config_store_init(void) {
    bool encrypted = nvs_encryption_possible();
    ESP_LOGI(TAG, "NVS mode: %s", encrypted ? "encrypted" : "plaintext");
    esp_err_t err = encrypted
        ? nvs_flash_init()
        : nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS requires explicit recovery; refusing identity erase: %s",
                 esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(err);
    // A token pair update a power cut interrupted: finish it.
    if (config_key_lookup(TOKEN_UPDATE_KEY) == CONFIG_KEY_FOUND) {
        bool done = apply_token_update();
        ESP_LOGW(TAG, "finishing an interrupted token pair update: %s", done ? "done" : "failed");
        (void)done;
    }
}

// The device session's credentials_update: the
// pair in one commit through the re-provisioning journal, K and node_id
// untouched. The journal keeps the old setup aside, the pair is written, and
// closing the journal is the commit point; a failed write rolls the old pair
// back, and a power cut before the commit leaves the journal open, so boot
// (config_reprovision_recover()) puts the old pair back. Refused while a
// journal is already open (a re-setup's).
// access_expires_at as NVS keeps it: unsigned decimal, absent for none.
#define TOKEN_EXPIRES_KEY "tok_exp"

static bool set_token_expires(uint64_t at) {
    if (!at) return config_erase_key(TOKEN_EXPIRES_KEY);
    char d[21], out[21];
    int n = 0;
    while (at) {
        d[n++] = (char)('0' + (int)(at % 10u));
        at /= 10u;
    }
    for (int i = 0; i < n; i++) out[i] = d[n - 1 - i];
    out[n] = '\0';
    return config_set_str(TOKEN_EXPIRES_KEY, out);
}

static uint64_t parse_token_expires(const char *s) {
    uint64_t v = 0;
    size_t n = strlen(s);
    if (n == 0 || n > 20) return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) return 0;
        v = v * 10u + d;
    }
    return v;
}

bool config_set_token_pair(const char *access_token, const char *refresh_token, uint64_t access_expires_at) {
    if (!access_token || !*access_token || !refresh_token || !*refresh_token) return false;
    if (config_reprovision_open()) return false;
    // A legacy update journal still there (its erase failed) would be applied over this pair at the next read:
    // gone first, or nothing is written.
    if (!config_erase_key(TOKEN_UPDATE_KEY)) return false;
    if (!config_reprovision_begin()) return false;
    bool ok = config_set_str("refresh_token", refresh_token)
              && config_set_str("access_token", access_token)
              && set_token_expires(access_expires_at);
    if (ok && config_reprovision_commit()) return true;
    if (!config_reprovision_rollback()) {
        ESP_LOGE(TAG, "token pair update failed and the old pair couldn't be put back yet (boot retries)");
    }
    return false;
}

bool config_get_token_pair(char *access_token, size_t access_size, char *refresh_token, size_t refresh_size) {
    if (config_reprovision_open()) {
        // A journal is open (a re-setup, or a token update a failed write
        // couldn't roll back yet): the committed pair is the one kept aside.
        return config_get_str("rp_at", access_token, access_size)
               && config_get_str("rp_rt", refresh_token, refresh_size)
               && access_token[0] && refresh_token[0];
    }
    if (config_key_lookup(TOKEN_UPDATE_KEY) == CONFIG_KEY_FOUND && !apply_token_update()
        && config_key_lookup(TOKEN_UPDATE_KEY) == CONFIG_KEY_FOUND) {
        // Still half applied (NVS refused a key): the journal holds the pair.
        return read_token_update(access_token, access_size, refresh_token, refresh_size);
    }
    return config_get_str("access_token", access_token, access_size)
           && config_get_str("refresh_token", refresh_token, refresh_size)
           && access_token[0] && refresh_token[0];
}

bool config_get_node_id(char *out, size_t size) {
    return config_get_str(config_reprovision_open() ? "rp_node" : "node_id", out, size) && out[0];
}

bool config_get_proof_key_hex(char *out, size_t size) {
    return config_get_str(config_reprovision_open() ? "rp_k" : "mg_proof_k", out, size) && out[0];
}

uint64_t config_get_token_expires(void) {
    char s[24];
    // While a journal is open the committed pair is the one kept aside, and
    // its expiry with it.
    const char *key = config_reprovision_open() ? "rp_exp" : TOKEN_EXPIRES_KEY;
    if (!config_get_str(key, s, sizeof(s))) return 0;
    return parse_token_expires(s);
}

bool config_get_str(const char *key, char *out, size_t buf_size) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = buf_size;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

bool config_set_str(const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool config_erase_key(const char *key) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open for erase %s failed: %s", key,
                 esp_err_to_name(err));
        return false;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "erase %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

config_key_lookup_t config_key_lookup(const char *key) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lookup %s failed to open NVS: %s", key,
                 esp_err_to_name(err));
        return CONFIG_KEY_LOOKUP_ERROR;
    }
    size_t len = 0;
    err = nvs_get_str(h, key, NULL, &len);
    nvs_close(h);
    if (err == ESP_OK) return CONFIG_KEY_FOUND;
    if (err == ESP_ERR_NVS_NOT_FOUND) return CONFIG_KEY_NOT_FOUND;
    ESP_LOGW(TAG, "lookup %s failed: %s", key, esp_err_to_name(err));
    return CONFIG_KEY_LOOKUP_ERROR;
}

static bool keys_are_absent(const char *const *keys, size_t key_count) {
    bool absent = true;
    for (size_t i = 0; i < key_count; i++) {
        config_key_lookup_t state = config_key_lookup(keys[i]);
        if (state != CONFIG_KEY_NOT_FOUND) {
            ESP_LOGE(TAG, "credential %s was not verified absent (state=%d)",
                     keys[i], (int)state);
            absent = false;
        }
    }
    return absent;
}

static bool clear_and_verify(const char *const *keys, size_t key_count) {
    bool erased = true;
    for (size_t i = 0; i < key_count; i++) {
        if (!config_erase_key(keys[i])) erased = false;
    }
    bool absent = keys_are_absent(keys, key_count);
    return erased && absent;
}

bool config_erase_face(void) {
    // Every key mg_face.c keeps (the record, the pick, the Muse's name, the Size).
    static const char *const keys[] = { "rec", "pick", "name", "size" };
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_STORE_FACE_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;   // never written
    if (err != ESP_OK) return false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND); i++) {
        err = nvs_erase_key(h, keys[i]);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "face erase failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool config_clear_pairing(void) {
    static const char *const keys[] = {
        "api_url",
        "api_url_v2",
        "noise_host",
        // The re-provisioning journal before the rest: boot must never
        // restore what this erases.
        "rp_open", "rp_at", "rp_rt", "rp_auth", "rp_k", "rp_wifi", "rp_user",
        "rp_api", "rp_api2", "rp_noise", "rp_hint", "rp_vm", "rp_node", "rp_exp",
        "access_token",
        "refresh_token",
        "auth_token",
        "vm_url",
        "username",
        "pair_sdk_hint",
        "mg_proof_k",
        "node_id",       // goes with the tokens it names
#if CONFIG_MUSE_AVATAR_SYNC
        "avatar_vm",
#endif
        "tok_exp",
        "tok_update",
    };
    bool cleared = clear_and_verify(keys, sizeof(keys) / sizeof(keys[0]));
    // The avatar face is the user's: it goes with the pairing, after
    // the endpoints and credentials.
    (void)config_erase_face();
    return cleared;
}

bool config_unpair(void) {
    // The marker goes last, and only once the pairing is verifiably gone:
    // boot treats tokens and Wi-Fi without the marker as a legacy pair and
    // marks it complete again, which would undo a half-finished unpair.
    return config_clear_pairing() && config_clear_setup_complete();
}

bool config_clear_setup(void) {
    static const char *const keys[] = {
        "api_url",
        "api_url_v2",
        "noise_host",
        // The re-provisioning journal before the rest: boot must never
        // restore what this erases.
        "rp_open", "rp_at", "rp_rt", "rp_auth", "rp_k", "rp_wifi", "rp_user",
        "rp_api", "rp_api2", "rp_noise", "rp_hint", "rp_vm", "rp_node", "rp_exp",
        "access_token",
        "refresh_token",
        "auth_token",
        "vm_url",
        "username",
        "pair_sdk_hint",
        "ssid",
        "password",
        "wifi_channel",
        "wifi_others",
        "wifi_hidden",
        "wifi_proxy",
        "mg_proof_k",
        "node_id",
#if CONFIG_MUSE_AVATAR_SYNC
        "avatar_vm",
#endif
        "tok_exp",
        "tok_update",
        "setup_wifi",
        "setup_complete",
    };
    bool cleared = clear_and_verify(keys, sizeof(keys) / sizeof(keys[0]));
    (void)config_erase_face();   // as config_clear_pairing()
    return cleared;
}

bool config_is_provisioned(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, "access_token", NULL, &len);
    if (err != ESP_OK || len <= 1) {
        len = 0;
        err = nvs_get_str(h, "auth_token", NULL, &len);
    }
    nvs_close(h);
    return err == ESP_OK && len > 1;
}

bool config_setup_complete(void) {
    char value[2] = {0};
    return config_get_str(SETUP_COMPLETE_KEY, value, sizeof(value))
           && value[0] == '1';
}

bool config_setup_complete_or_unreadable(void) {
    char value[2] = {0};
    switch (config_key_lookup(SETUP_COMPLETE_KEY)) {
    case CONFIG_KEY_NOT_FOUND: return false;
    case CONFIG_KEY_FOUND: return !config_get_str(SETUP_COMPLETE_KEY, value, sizeof(value)) || value[0] == '1';
    default: return true;   // the marker can't be read: never "unset"
    }
}

bool config_mark_setup_complete(void) {
    return config_set_str(SETUP_COMPLETE_KEY, "1");
}

bool config_wifi_skipped(void) {
    char value[8] = {0};
    return config_get_str(SETUP_WIFI_KEY, value, sizeof(value))
           && strcmp(value, "skipped") == 0;
}

bool config_mark_wifi_skipped(void) {
    return config_set_str(SETUP_WIFI_KEY, "skipped");
}

bool config_clear_setup_complete(void) {
    const char *keys[] = {SETUP_COMPLETE_KEY};
    return clear_and_verify(keys, sizeof(keys) / sizeof(keys[0]));
}

// ---- Re-provisioning journal ------------------------------------------------

// Copies one key's value to another key, an absent value as an absent key.
static bool copy_key(const char *from, const char *to) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, from, NULL, &len);
    char *value = NULL;
    size_t cap = 0;   /* what was allocated: the second read sets len to the value's size now, which can differ */
    if (err == ESP_OK) {
        cap = len ? len : 1;
        value = malloc(cap);
        if (!value) {
            nvs_close(h);
            return false;
        }
        err = nvs_get_str(h, from, value, &len);
    }
    nvs_close(h);
    bool ok;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ok = config_erase_key(to);
    } else {
        ok = err == ESP_OK && config_set_str(to, value);
    }
    if (value) {
        wipe(value, cap);
        free(value);
    }
    if (!ok) ESP_LOGW(TAG, "copying %s to %s failed", from, to);
    return ok;
}

static bool erase_backups(void) {
    bool ok = true;
    for (size_t i = 0; i < REPROVISION_COUNT; i++) {
        if (!config_erase_key(REPROVISION_KEYS[i][1])) ok = false;
    }
    return ok;
}

bool config_reprovision_open(void) {
    // A marker that can't be read counts as open: the kept-aside setup stays
    // the one in use rather than a half-written new one.
    config_key_lookup_t found = config_key_lookup(REPROVISION_OPEN_KEY);
    if (found != CONFIG_KEY_FOUND) return found == CONFIG_KEY_LOOKUP_ERROR;
    char value[2] = {0};
    return !config_get_str(REPROVISION_OPEN_KEY, value, sizeof(value)) || value[0] == '1';
}

bool config_reprovision_begin(void) {
    // A journal still open (a rollback that failed): its copies are the only
    // intact setup. Put them back first; a new transaction never copies the
    // mixed live keys over them.
    if (config_reprovision_open() && !config_reprovision_rollback()) {
        ESP_LOGE(TAG, "the last setup's rollback is still unfinished; refusing a new one");
        return false;
    }
    // Copies first, the marker last: a power loss before the marker leaves
    // copies nothing restores (boot erases them).
    bool ok = config_erase_key(REPROVISION_OPEN_KEY);
    for (size_t i = 0; ok && i < REPROVISION_COUNT; i++) {
        ok = copy_key(REPROVISION_KEYS[i][0], REPROVISION_KEYS[i][1]);
    }
    ok = ok && config_set_str(REPROVISION_OPEN_KEY, "1");
    if (!ok) {
        (void)config_erase_key(REPROVISION_OPEN_KEY);
        (void)erase_backups();
        ESP_LOGE(TAG, "couldn't keep the old setup aside; nothing changed");
    }
    return ok;
}

bool config_reprovision_commit(void) {
    // The commit point: from here the new values are the setup.
    if (!config_erase_key(REPROVISION_OPEN_KEY)) return false;
    if (!erase_backups()) ESP_LOGW(TAG, "old setup copies not erased yet (boot erases them)");
    return true;
}

bool config_reprovision_rollback(void) {
    if (!config_reprovision_open()) {
        (void)erase_backups();
        return true;
    }
    bool ok = true;
    for (size_t i = 0; i < REPROVISION_COUNT; i++) {
        if (!copy_key(REPROVISION_KEYS[i][1], REPROVISION_KEYS[i][0])) ok = false;
    }
    // Only once every value is back does the journal go; until then a
    // retry, or the next boot, restores again.
    ok = ok && config_erase_key(REPROVISION_OPEN_KEY);
    if (ok) (void)erase_backups();
    return ok;
}

config_recover_t config_reprovision_recover(void) {
    if (!config_reprovision_open()) {
        // Copies without the marker: a begin cut short, or a commit that
        // didn't get to erase them. Neither is part of the setup.
        bool found = false;
        for (size_t i = 0; i < REPROVISION_COUNT && !found; i++) {
            found = config_key_lookup(REPROVISION_KEYS[i][1]) == CONFIG_KEY_FOUND;
        }
        if (found) (void)erase_backups();
        return CONFIG_RECOVER_NONE;
    }
    ESP_LOGW(TAG, "re-provisioning cut short before its commit: restoring the old setup");
    if (!config_reprovision_rollback()) {
        ESP_LOGE(TAG, "restoring the old setup failed");
        return CONFIG_RECOVER_FAILED;
    }
    return CONFIG_RECOVER_RESTORED;
}
