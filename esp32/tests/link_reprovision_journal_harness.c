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
 * The re-provisioning journal of main/config_store.c (Gadget setup v2) on a
 * fake NVS that writes each key at once, as NVS does, and can be made to fail
 * on one key: begin keeps the old setup aside, rollback and boot recovery put
 * it back exactly (absent keys absent again), commit drops it, and the setup
 * resets erase the journal first.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "nvs.h"

#define MAX_ENTRIES 64
#define KEY_BYTES 16
#define VALUE_BYTES 2200

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
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static const char *s_fail_set;     // nvs_set_str of this key fails
static const char *s_fail_erase;   // nvs_erase_key of this key fails
static int s_writes;
// A power cut: the write that would be number s_cut_at never happens, and
// control returns to the setjmp in cut_power_during().
static int s_cut_at;
static jmp_buf s_cut;

static void maybe_cut(void) {
    if (s_cut_at && s_writes + 1 == s_cut_at) longjmp(s_cut, 1);
}

static entry_t *find(const char *key) {
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) return &s_entries[i];
    }
    return NULL;
}

static void put(const char *key, const char *value) {
    entry_t *e = find(key);
    for (size_t i = 0; !e && i < MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            e = &s_entries[i];
            e->used = true;
            snprintf(e->key, sizeof(e->key), "%s", key);
        }
    }
    CHECK(e && strlen(key) < KEY_BYTES && strlen(value) < VALUE_BYTES, "fake NVS: %s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
}

static const char *get(const char *key) {
    entry_t *e = find(key);
    return e ? e->value : NULL;
}

static int count(void) {
    int n = 0;
    for (size_t i = 0; i < MAX_ENTRIES; i++) n += s_entries[i].used;
    return n;
}

static void reset(void) {
    memset(s_entries, 0, sizeof(s_entries));
    s_fail_set = s_fail_erase = NULL;
    s_writes = 0;
    s_cut_at = 0;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char *part_name) { (void)part_name; return ESP_OK; }

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    (void)namespace_name;
    (void)open_mode;
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length) {
    (void)handle;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = strlen(e->value) + 1;
    if (!out_value) {
        *length = need;
        return ESP_OK;
    }
    if (*length < need) {
        *length = need;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, e->value, need);
    *length = need;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    (void)handle;
    if (s_fail_set && strcmp(s_fail_set, key) == 0) return ESP_FAIL;
    maybe_cut();
    put(key, value);
    s_writes++;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    (void)handle;
    entry_t *e = find(key);
    if (e && s_fail_erase && strcmp(s_fail_erase, key) == 0) return ESP_FAIL;
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    maybe_cut();
    memset(e, 0, sizeof(*e));
    s_writes++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

// A set-up token-only gadget, as commit_setup() leaves it, plus a device key.
static char s_long_token[2049];

static void old_setup(void) {
    reset();
    memset(s_long_token, 'o', 2048);
    s_long_token[2048] = '\0';
    put("access_token", s_long_token);
    put("refresh_token", "old-refresh");
    put("mg_proof_k", "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    put("setup_wifi", "skipped");
    put("setup_complete", "1");
    put("pair_sdk_hint", "mgst_abcdefg");
    put("node_id", "homelink-1a2b3c");   // stored with K
    put("node_key", "device-level");   // never part of setup
}

// A set-up gadget whose pair carries an access_expires_at (a client's write-back).
static void old_setup_with_expiry(void) {
    old_setup();
    put("tok_exp", "1700000000");
}

static void check_old(const char *when) {
    CHECK(get("access_token") && strcmp(get("access_token"), s_long_token) == 0, "%s: access", when);
    CHECK(get("refresh_token") && strcmp(get("refresh_token"), "old-refresh") == 0, "%s: refresh", when);
    CHECK(get("mg_proof_k") && strncmp(get("mg_proof_k"), "0011", 4) == 0, "%s: key", when);
    CHECK(get("setup_wifi") && strcmp(get("setup_wifi"), "skipped") == 0, "%s: skipped", when);
    CHECK(get("setup_complete") && strcmp(get("setup_complete"), "1") == 0, "%s: marker", when);
    CHECK(get("pair_sdk_hint") && strcmp(get("pair_sdk_hint"), "mgst_abcdefg") == 0, "%s: hint", when);
    CHECK(!get("username") && !get("api_url_v2") && !get("noise_host"), "%s: absent stays absent", when);
    CHECK(get("node_id") && strcmp(get("node_id"), "homelink-1a2b3c") == 0, "%s: node id", when);
    CHECK(get("node_key") != NULL, "%s: device key", when);
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        CHECK(!s_entries[i].used || strncmp(s_entries[i].key, "rp_", 3) != 0,
              "%s: journal key %s left", when, s_entries[i].key);
    }
    CHECK(count() == 8, "%s: %d keys", when, count());
}

// What a provisioning writes before its commit: new tokens, endpoints the old
// setup didn't have, a new key, a new hint.
static void overwrite(void) {
    CHECK(config_set_str("access_token", "new-access"), "write access");
    CHECK(config_set_str("refresh_token", "new-refresh"), "write refresh");
    CHECK(config_set_str("username", "user@example.com"), "write username");
    CHECK(config_set_str("api_url_v2", "https://api"), "write api");
    CHECK(config_set_str("mg_proof_k", "ffee"), "write key");
    CHECK(config_set_str("pair_sdk_hint", "mgst_zzzzzzz"), "write hint");
    CHECK(config_set_str("node_id", "homelink-ffffff"), "write node id");
}

static void test_begin_keeps_the_old_setup_aside(void) {
    old_setup();
    CHECK(!config_reprovision_open(), "no journal yet");
    CHECK(config_reprovision_begin(), "begin");
    CHECK(config_reprovision_open() && strcmp(get("rp_open"), "1") == 0, "journal open");
    CHECK(get("rp_at") && strcmp(get("rp_at"), s_long_token) == 0, "access copied");
    CHECK(strcmp(get("rp_rt"), "old-refresh") == 0 && get("rp_k") && get("rp_wifi") && get("rp_hint"),
          "the rest copied");
    CHECK(!get("rp_user") && !get("rp_api2") && !get("rp_vm"), "absent copied as absent");
    // Nothing of the setup itself changed.
    CHECK(strcmp(get("access_token"), s_long_token) == 0 && strcmp(get("setup_complete"), "1") == 0,
          "live untouched");
}

static void test_rollback_restores_exactly(void) {
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    CHECK(config_reprovision_rollback(), "rollback");
    CHECK(!config_reprovision_open(), "journal closed");
    check_old("rollback");
    // Rollback with nothing open: nothing to do.
    CHECK(config_reprovision_rollback(), "idle rollback");
    check_old("idle rollback");
}

static void test_commit_keeps_the_new_setup(void) {
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    CHECK(config_reprovision_commit(), "commit");
    CHECK(!config_reprovision_open() && !get("rp_at") && !get("rp_rt") && !get("rp_k"), "journal gone");
    CHECK(strcmp(get("access_token"), "new-access") == 0 && strcmp(get("username"), "user@example.com") == 0,
          "new setup kept");
    // The old tokens are nowhere.
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        CHECK(!s_entries[i].used || (strcmp(s_entries[i].value, s_long_token) != 0
                                     && strcmp(s_entries[i].value, "old-refresh") != 0),
              "old token left in %s", s_entries[i].key);
    }
    // A boot afterwards changes nothing.
    int writes = s_writes;
    CHECK(!config_reprovision_recover(), "nothing to recover");
    CHECK(s_writes == writes && strcmp(get("access_token"), "new-access") == 0, "recover idle");
}

static void test_power_loss_before_the_commit(void) {
    // Midway through the provisioning: boot puts the old setup back.
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    CHECK(config_reprovision_recover(), "recovered");
    check_old("power loss");
    // Midway through begin (copies written, marker not yet): the copies are
    // leftovers, the setup was never touched.
    old_setup();
    put("rp_at", "half");
    put("rp_rt", "half");
    CHECK(!config_reprovision_recover(), "nothing restored");
    check_old("begin cut short");
    // Midway through a rollback: the marker is still there, so boot restores
    // again, the same way.
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    s_fail_set = "refresh_token";
    CHECK(!config_reprovision_rollback(), "rollback fails on refresh_token");
    CHECK(config_reprovision_open(), "journal still open");
    s_fail_set = NULL;
    CHECK(config_reprovision_recover(), "recovered");
    check_old("rollback cut short");
    // After the commit point, before the copies went: leftovers, the new setup stays.
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    s_fail_erase = "rp_at";
    CHECK(config_reprovision_commit(), "commit (copies erased later)");
    s_fail_erase = NULL;
    CHECK(get("rp_at") && !config_reprovision_open(), "a leftover copy");
    CHECK(!config_reprovision_recover(), "not restored");
    CHECK(!get("rp_at") && strcmp(get("access_token"), "new-access") == 0, "leftover erased, new kept");
}

static void test_failures(void) {
    // begin can't copy: nothing open, nothing left, the setup untouched.
    old_setup();
    s_fail_set = "rp_k";
    CHECK(!config_reprovision_begin(), "begin fails");
    s_fail_set = NULL;
    CHECK(!config_reprovision_open(), "no journal");
    check_old("failed begin");
    // ... or can't write the marker.
    old_setup();
    s_fail_set = "rp_open";
    CHECK(!config_reprovision_begin(), "begin fails at the marker");
    s_fail_set = NULL;
    check_old("failed marker");
    // The commit point can't be written: still open, so the old setup can come back.
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    s_fail_erase = "rp_open";
    CHECK(!config_reprovision_commit(), "commit fails");
    s_fail_erase = NULL;
    CHECK(config_reprovision_open() && config_reprovision_rollback(), "rolled back");
    check_old("failed commit");
    // A rollback that failed: a second setup puts the old one back first, and never copies the mixed
    // live keys over the only intact copies.
    old_setup();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    s_fail_set = "access_token";
    CHECK(!config_reprovision_rollback(), "rollback fails");
    CHECK(!config_reprovision_begin(), "a second setup refused while the rollback can't finish");
    CHECK(config_reprovision_open(), "the copies kept");
    s_fail_set = NULL;
    CHECK(config_reprovision_begin(), "then it rolls back first and begins");
    CHECK(config_reprovision_rollback(), "rolled back");
    check_old("a failed rollback, then a second setup");
}

static void test_resets_erase_the_journal_first(void) {
    // A reset while a journal is open: boot must not bring the old setup back.
    old_setup_with_expiry();
    CHECK(config_reprovision_begin(), "begin");
    overwrite();
    CHECK(config_clear_setup(), "clear setup");
    CHECK(!config_reprovision_open() && !get("rp_at") && !get("access_token") && !get("setup_complete"),
          "all gone");
    CHECK(!get("node_id") && !get("rp_node") && !get("mg_proof_k"), "node id and K gone with the rest");
    CHECK(!get("tok_exp") && !get("rp_exp"), "the pair's expiry gone with it");
    CHECK(!config_reprovision_recover() && !get("access_token"), "nothing restored");
    CHECK(get("node_key") != NULL, "device key kept");
    old_setup_with_expiry();
    CHECK(config_reprovision_begin(), "begin");
    CHECK(config_clear_pairing(), "clear pairing");
    CHECK(!config_reprovision_open() && !get("rp_rt") && !get("refresh_token"), "pairing gone");
    CHECK(!get("node_id") && !get("rp_node") && !get("mg_proof_k"), "node id and K gone with the pairing");
    CHECK(!get("tok_exp") && !get("rp_exp"), "the pair's expiry gone with the pairing");
    CHECK(!config_reprovision_recover() && !get("refresh_token"), "nothing restored");
}

// The device session's credentials_update: the
// pair in one commit through this journal, K and node_id untouched. A power
// cut at every write, then boot: the gadget holds the old pair or the new
// one whole, never half of each, and nothing of the journal stays.
// The new pair and its access_expires_at, both or neither.
static bool holds_new_pair(void) {
    return get("access_token") && strcmp(get("access_token"), "new-access") == 0
           && strcmp(get("refresh_token"), "new-refresh") == 0
           && get("tok_exp") && strcmp(get("tok_exp"), "1793860000") == 0;
}

static void check_after_update(const char *when, bool want_new) {
    if (want_new) {
        CHECK(holds_new_pair(), "%s: the new pair and its expiry", when);
    } else {
        CHECK(strcmp(get("access_token"), s_long_token) == 0 && strcmp(get("refresh_token"), "old-refresh") == 0
              && get("tok_exp") && strcmp(get("tok_exp"), "1700000000") == 0,
              "%s: the old pair and its expiry", when);
        CHECK(config_get_token_expires() == 1700000000ull, "%s: the old expiry read", when);
    }
    CHECK(strncmp(get("mg_proof_k"), "0011", 4) == 0 && strcmp(get("node_id"), "homelink-1a2b3c") == 0,
          "%s: K and node id unchanged", when);
    CHECK(strcmp(get("setup_complete"), "1") == 0 && strcmp(get("setup_wifi"), "skipped") == 0,
          "%s: still set up", when);
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        CHECK(!s_entries[i].used || strncmp(s_entries[i].key, "rp_", 3) != 0,
              "%s: journal key %s left", when, s_entries[i].key);
    }
    CHECK(count() == 9, "%s: %d keys", when, count());
}

static void test_token_update(void) {
    old_setup_with_expiry();
    CHECK(config_set_token_pair("new-access", "new-refresh", 1793860000), "update");
    check_after_update("update", true);
    // Each write failing: refused, the old pair stays.
    const char *failing[] = {"rp_at", "rp_rt", "rp_k", "rp_node", "rp_exp", "rp_open", "refresh_token",
                             "access_token", "tok_exp"};
    for (size_t i = 0; i < sizeof(failing) / sizeof(failing[0]); i++) {
        old_setup_with_expiry();
        s_fail_set = failing[i];
        CHECK(!config_set_token_pair("new-access", "new-refresh", 1793860000), "failing %s: refused", failing[i]);
        // The pair read meanwhile is the old one, whole, even when the
        // rollback couldn't write it back yet (NVS still refusing).
        char at[2100], rt[64];
        CHECK(config_get_token_pair(at, sizeof(at), rt, sizeof(rt)) && strcmp(at, s_long_token) == 0
              && strcmp(rt, "old-refresh") == 0, "failing %s: the old pair read", failing[i]);
        s_fail_set = NULL;
        (void)config_reprovision_recover();   // boot
        check_after_update(failing[i], false);
    }
    // The commit point can't be written: refused, rolled back.
    old_setup_with_expiry();
    s_fail_erase = "rp_open";
    CHECK(!config_set_token_pair("new-access", "new-refresh", 1793860000), "commit fails");
    s_fail_erase = NULL;
    CHECK(config_reprovision_open(), "the journal stays open for boot");
    CHECK(config_reprovision_recover(), "boot restores");
    check_after_update("failed commit, then boot", false);
    // A legacy update journal still there, its erase failing: no pair written over it; once it goes,
    // the update is stored and the journal is gone.
    old_setup_with_expiry();
    put("tok_update", "stale");
    s_fail_erase = "tok_update";
    CHECK(!config_set_token_pair("new-access", "new-refresh", 1793860000), "refused over a stale journal");
    s_fail_erase = NULL;
    CHECK(strcmp(get("access_token"), s_long_token) == 0 && strcmp(get("refresh_token"), "old-refresh") == 0
          && find("tok_update"), "stale journal: the old pair, untouched");
    CHECK(config_set_token_pair("new-access", "new-refresh", 1793860000), "then stored");
    CHECK(!find("tok_update"), "the stale journal gone");
    check_after_update("after the stale journal", true);
    // A re-setup's journal open: refused, nothing written.
    old_setup_with_expiry();
    CHECK(config_reprovision_begin(), "a re-setup begins");
    int writes = s_writes;
    CHECK(!config_set_token_pair("new-access", "new-refresh", 1793860000) && s_writes == writes, "refused while open");
    CHECK(config_reprovision_rollback(), "rollback");

    // A power cut at every write, then boot.
    old_setup_with_expiry();
    CHECK(config_set_token_pair("new-access", "new-refresh", 1793860000), "count the writes");
    int total = s_writes;
    int olds = 0, news = 0;
    bool seen_new = false;
    for (int cut = 1; cut <= total; cut++) {
        old_setup_with_expiry();
        s_cut_at = cut;
        if (setjmp(s_cut) == 0) {
            (void)config_set_token_pair("new-access", "new-refresh", 1793860000);
            CHECK(false, "write %d: no power cut", cut);
        }
        s_cut_at = 0;
        char when[48];
        snprintf(when, sizeof(when), "power cut at write %d of %d", cut, total);
        (void)config_reprovision_recover();   // boot
        bool now_new = holds_new_pair();
        check_after_update(when, now_new);
        now_new ? news++ : olds++;
        // Once the new pair stands, it stands for every later cut.
        CHECK(now_new || !seen_new, "%s: back to the old pair after the commit point", when);
        seen_new = seen_new || now_new;
    }
    CHECK(olds > 0 && news > 0, "cuts before and after the commit point (%d old, %d new)", olds, news);
    // An update without access_expires_at: none kept (the old pair's goes with it).
    old_setup_with_expiry();
    CHECK(config_set_token_pair("new-access", "new-refresh", 0) && !get("tok_exp") && !get("rp_exp")
          && config_get_token_expires() == 0, "no expiry: none kept");
    // A re-setup's journal open: the committed pair's expiry is read.
    old_setup_with_expiry();
    CHECK(config_reprovision_begin() && config_erase_key("tok_exp") && config_get_token_expires() == 1700000000ull,
          "the expiry kept aside is read while the journal is open");
    CHECK(config_reprovision_rollback() && config_get_token_expires() == 1700000000ull, "rolled back");
    printf("token update: %d writes, a cut at each (%d before the commit point, %d after)\n", total, olds, news);
}

int main(void) {
    test_begin_keeps_the_old_setup_aside();
    test_rollback_restores_exactly();
    test_commit_keeps_the_new_setup();
    test_power_loss_before_the_commit();
    test_failures();
    test_resets_erase_the_journal_first();
    test_token_update();
    printf("PASS reprovision journal\n");
    return 0;
}
