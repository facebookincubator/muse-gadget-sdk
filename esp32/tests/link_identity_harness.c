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

// Runs main/identity.c against an in-memory config store. The build's token
// (CONFIG_GADGET_SDK_TOKEN) is BUILD_TOKEN; tests/test_link_identity.py
// passes both.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_mac.h"
#include "identity.h"

#define CHECK(cond, ...) do {                                                \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        exit(1);                                                             \
    }                                                                        \
} while (0)

#define OTHER_TOKEN "mgst_BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBA"

#define MAX_ENTRIES 8

static struct {
    char key[32];
    char value[64];
} s_entries[MAX_ENTRIES];

static int find(const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].key[0] && strcmp(s_entries[i].key, key) == 0) return i;
    }
    return -1;
}

bool config_get_str(const char *key, char *out, size_t buf_size) {
    int i = find(key);
    if (i < 0 || strlen(s_entries[i].value) >= buf_size) return false;
    strcpy(out, s_entries[i].value);
    return true;
}

bool config_set_str(const char *key, const char *value) {
    int i = find(key);
    for (int j = 0; i < 0 && j < MAX_ENTRIES; j++) {
        if (!s_entries[j].key[0]) i = j;
    }
    if (i < 0) return false;
    snprintf(s_entries[i].key, sizeof(s_entries[i].key), "%s", key);
    snprintf(s_entries[i].value, sizeof(s_entries[i].value), "%s", value);
    return true;
}

static void erase(const char *key) {
    int i = find(key);
    if (i >= 0) memset(&s_entries[i], 0, sizeof(s_entries[i]));
}

esp_err_t esp_read_mac(uint8_t *mac, int type) {
    (void)type;
    memset(mac, 0, 6);
    return ESP_OK;
}

static void test_a_token_set_after_pairing_needs_pairing_again(void) {
    char paired[IDENTITY_SDK_TOKEN_HINT_LEN + 1] = "";
    identity_init();
    CHECK(strcmp(identity_sdk_token(), BUILD_TOKEN) == 0, "build token not used");
    CHECK(!identity_sdk_token_changed_since_pairing(paired, sizeof(paired)),
          "an unpaired gadget reported a changed token");

    identity_note_paired_sdk_token();
    CHECK(!identity_sdk_token_changed_since_pairing(paired, sizeof(paired)),
          "the token pairing used reported as changed");

    CHECK(identity_set_sdk_token(OTHER_TOKEN), "valid token refused");
    identity_init();
    CHECK(strcmp(identity_sdk_token(), OTHER_TOKEN) == 0, "stored token not used");
    CHECK(identity_sdk_token_changed_since_pairing(paired, sizeof(paired)),
          "a token set after pairing was not reported");
    CHECK(strcmp(paired, "mgst_AAAAAAA") == 0, "paired hint was %s", paired);

    identity_note_paired_sdk_token();
    CHECK(!identity_sdk_token_changed_since_pairing(NULL, 0),
          "pairing again did not take the new token");
}

static void test_unknown_or_tokenless_pairings(void) {
    char paired[IDENTITY_SDK_TOKEN_HINT_LEN + 1] = "";
    erase("pair_sdk_hint");
    CHECK(!identity_sdk_token_changed_since_pairing(paired, sizeof(paired)),
          "a pairing with no noted token reported a change");

    config_set_str("pair_sdk_hint", "none");
    CHECK(identity_sdk_token_changed_since_pairing(paired, sizeof(paired)),
          "a token added after a tokenless pairing was not reported");
    CHECK(strcmp(paired, "none") == 0, "paired hint was %s", paired);
}

int main(void) {
    test_a_token_set_after_pairing_needs_pairing_again();
    test_unknown_or_tokenless_pairings();
    puts("identity harness: PASS");
    return 0;
}
