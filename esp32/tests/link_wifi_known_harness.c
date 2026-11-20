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
#include "freertos/semphr.h"
#include "nvs.h"
#include "wifi_known.h"

#define MAX_ENTRIES 24
#define KEY_BYTES 16
#define VALUE_BYTES 4000   // NVS's own limit for a string

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

static fake_entry_t s_entries[MAX_ENTRIES];
static bool s_pending_set;
static bool s_pending_erase;
static char s_pending_key[KEY_BYTES];
static char s_pending_value[VALUE_BYTES];
static int s_writes;
static int s_locked;

// ---- Fakes ------------------------------------------------------------------

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    static int mutex;
    return &mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks) {
    (void)semaphore;
    (void)wait_ticks;
    CHECK(s_locked == 0, "wifi_known took its lock twice");
    s_locked++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    CHECK(s_locked == 1, "wifi_known gave a lock it didn't hold");
    s_locked--;
    return pdTRUE;
}

static fake_entry_t *find_entry(const char *key) {
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static void fake_put(const char *key, const char *value) {
    CHECK(strlen(key) < KEY_BYTES, "NVS key too long: %s", key);
    CHECK(strlen(value) < VALUE_BYTES, "NVS value too long for %s", key);
    fake_entry_t *entry = find_entry(key);
    for (size_t i = 0; !entry && i < MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            entry = &s_entries[i];
            entry->used = true;
            snprintf(entry->key, sizeof(entry->key), "%s", key);
        }
    }
    CHECK(entry != NULL, "fake NVS is full");
    snprintf(entry->value, sizeof(entry->value), "%s", value);
}

static const char *fake_get(const char *key) {
    fake_entry_t *entry = find_entry(key);
    return entry ? entry->value : NULL;
}

static void fake_reset(void) {
    memset(s_entries, 0, sizeof(s_entries));
    s_pending_set = s_pending_erase = false;
    s_writes = 0;
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
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    (void)handle;
    s_pending_set = s_pending_erase = false;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value,
                      size_t *length) {
    (void)handle;
    const char *value = fake_get(key);
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    size_t required = strlen(value) + 1;
    if (!out_value) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) {
        *length = required;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, value, required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    (void)handle;
    s_pending_set = true;
    snprintf(s_pending_key, sizeof(s_pending_key), "%s", key);
    snprintf(s_pending_value, sizeof(s_pending_value), "%s", value);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    (void)handle;
    if (!find_entry(key)) return ESP_ERR_NVS_NOT_FOUND;
    s_pending_erase = true;
    snprintf(s_pending_key, sizeof(s_pending_key), "%s", key);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    if (s_pending_set) {
        fake_put(s_pending_key, s_pending_value);
        s_writes++;
    } else if (s_pending_erase) {
        fake_entry_t *entry = find_entry(s_pending_key);
        if (entry) memset(entry, 0, sizeof(*entry));
        s_writes++;
    }
    s_pending_set = s_pending_erase = false;
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

// ---- Helpers ----------------------------------------------------------------

static wifi_known_list_t s_list;

static int load(void) {
    int n = wifi_known_load(&s_list);
    CHECK(n == s_list.count, "load returned %d for %d networks", n, s_list.count);
    CHECK(s_locked == 0, "lock held after load");
    return n;
}

static void expect_order(const char *const *ssids, int count) {
    CHECK(load() == count, "expected %d networks, have %d", count, s_list.count);
    for (int i = 0; i < count; i++) {
        CHECK(strcmp(s_list.nets[i].ssid, ssids[i]) == 0,
              "network %d is %s, expected %s", i, s_list.nets[i].ssid, ssids[i]);
    }
}

static void remember(const char *ssid, const char *password, int hidden) {
    CHECK(wifi_known_remember(ssid, password, hidden), "remember %s failed", ssid);
    CHECK(s_locked == 0, "lock held after remember");
}

// ---- Tests ------------------------------------------------------------------

static void test_empty_and_round_trip(void) {
    fake_reset();
    CHECK(load() == 0, "nothing saved should load as no networks");

    remember("Home", "home-pass", 0);
    const char *one[] = {"Home"};
    expect_order(one, 1);
    CHECK(strcmp(s_list.nets[0].password, "home-pass") == 0, "password lost");
    CHECK(!s_list.nets[0].hidden, "not hidden");
    // The first network stays where one-network code (and older firmware)
    // reads it.
    CHECK(fake_get("ssid") && strcmp(fake_get("ssid"), "Home") == 0, "ssid key");
    CHECK(fake_get("password") && strcmp(fake_get("password"), "home-pass") == 0,
          "password key");
    CHECK(!fake_get("wifi_others"), "one network needs no list");
    CHECK(!fake_get("wifi_hidden"), "no hidden flag");

    remember("Office", "office \"pass\" \\ 1", 0);
    remember("Cafe", "", 0);
    const char *three[] = {"Cafe", "Office", "Home"};
    expect_order(three, 3);
    CHECK(strcmp(s_list.nets[0].password, "") == 0, "open network");
    CHECK(strcmp(s_list.nets[1].password, "office \"pass\" \\ 1") == 0,
          "password with quotes and backslash didn't survive: %s",
          s_list.nets[1].password);
    CHECK(strcmp(s_list.nets[2].password, "home-pass") == 0, "oldest password");
    CHECK(fake_get("wifi_others") != NULL, "others saved");
    CHECK(strstr(fake_get("wifi_others"), "Cafe") == NULL,
          "the first network isn't repeated in the list");

    // Saving what's already saved writes nothing.
    int writes = s_writes;
    remember("Cafe", "", -1);
    CHECK(s_writes == writes, "unchanged list rewrote %d keys", s_writes - writes);
}

static void test_legacy_single_network(void) {
    fake_reset();
    fake_put("ssid", "Old");
    fake_put("password", "old-pass");
    const char *one[] = {"Old"};
    expect_order(one, 1);
    CHECK(strcmp(s_list.nets[0].password, "old-pass") == 0, "legacy password");

    fake_reset();
    fake_put("ssid", "Open");   // no password key at all
    expect_order((const char *[]){"Open"}, 1);
    CHECK(s_list.nets[0].password[0] == '\0', "missing password reads as open");
}

static void test_remember_reorders_and_caps(void) {
    fake_reset();
    remember("A", "pa", 0);
    remember("B", "pb", 0);
    remember("C", "pc", 0);
    remember("A", "pa2", -1);
    const char *order[] = {"A", "C", "B"};
    expect_order(order, 3);
    CHECK(strcmp(s_list.nets[0].password, "pa2") == 0, "new password kept");

    char name[8];
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "N%d", i);
        remember(name, "secret", 0);
    }
    CHECK(load() == WIFI_KNOWN_MAX, "capped at %d, have %d", WIFI_KNOWN_MAX,
          s_list.count);
    for (int i = 0; i < WIFI_KNOWN_MAX; i++) {
        snprintf(name, sizeof(name), "N%d", 9 - i);
        CHECK(strcmp(s_list.nets[i].ssid, name) == 0, "slot %d is %s, not %s",
              i, s_list.nets[i].ssid, name);
    }

    // Eight of the longest names and passwords still fit.
    fake_reset();
    char ssid[WIFI_KNOWN_SSID_MAX + 1], pass[WIFI_KNOWN_PASS_MAX + 1];
    for (int i = 0; i < WIFI_KNOWN_MAX; i++) {
        memset(ssid, 'a' + i, WIFI_KNOWN_SSID_MAX);
        ssid[WIFI_KNOWN_SSID_MAX] = '\0';
        memset(pass, '"', WIFI_KNOWN_PASS_MAX);
        pass[WIFI_KNOWN_PASS_MAX] = '\0';
        remember(ssid, pass, i % 2);
    }
    CHECK(load() == WIFI_KNOWN_MAX, "long entries dropped: %d", s_list.count);
    CHECK(strcmp(s_list.nets[WIFI_KNOWN_MAX - 1].password, pass) == 0,
          "long password");

    CHECK(!wifi_known_remember("", "x", 0), "empty name refused");
    CHECK(!wifi_known_remember("123456789012345678901234567890123", "x", 0),
          "33-byte name refused");
}

static void test_forget(void) {
    fake_reset();
    remember("A", "pa", 0);
    remember("B", "pb", 1);
    remember("C", "pc", 0);
    fake_put("wifi_channel", "11");

    // Not the first: the first and its channel stay.
    CHECK(wifi_known_forget("A"), "forget A");
    expect_order((const char *[]){"C", "B"}, 2);
    CHECK(fake_get("wifi_channel") != NULL, "channel kept");

    // The first: the next takes its place, with its password and flag.
    CHECK(wifi_known_forget("C"), "forget C");
    expect_order((const char *[]){"B"}, 1);
    CHECK(strcmp(fake_get("ssid"), "B") == 0, "B promoted to the ssid key");
    CHECK(strcmp(fake_get("password"), "pb") == 0, "B's password promoted");
    CHECK(fake_get("wifi_hidden") && strcmp(fake_get("wifi_hidden"), "1") == 0,
          "B's hidden flag promoted");
    CHECK(!fake_get("wifi_others"), "list erased when one network is left");
    CHECK(!fake_get("wifi_channel"), "C's channel erased");

    CHECK(wifi_known_forget("nope"), "forgetting an unknown network is fine");
    expect_order((const char *[]){"B"}, 1);

    CHECK(wifi_known_forget("B"), "forget the last");
    CHECK(load() == 0, "nothing left");
    CHECK(!fake_get("ssid") && !fake_get("password") && !fake_get("wifi_hidden"),
          "keys erased");

    remember("A", "pa", 0);
    remember("B", "pb", 1);
    fake_put("wifi_channel", "6");
    CHECK(wifi_known_forget_all(), "forget all");
    CHECK(load() == 0, "forget all left networks");
    const char *keys[] = {"ssid", "password", "wifi_hidden", "wifi_others",
                          "wifi_channel"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        CHECK(!fake_get(keys[i]), "%s left after forget all", keys[i]);
    }
}

static void test_hidden_flag(void) {
    fake_reset();
    remember("Hidden", "ph", 1);
    CHECK(load() == 1 && s_list.nets[0].hidden, "hidden flag saved");
    CHECK(fake_get("wifi_hidden") && strcmp(fake_get("wifi_hidden"), "1") == 0,
          "wifi_hidden key");

    remember("Seen", "ps", 0);
    CHECK(!fake_get("wifi_hidden"), "flag follows the first network");
    CHECK(load() == 2 && !s_list.nets[0].hidden && s_list.nets[1].hidden,
          "flag kept in the list");

    remember("Hidden", "ph", -1);
    CHECK(load() == 2 && s_list.nets[0].hidden, "-1 keeps the flag");
    remember("New", "pn", -1);
    CHECK(load() == 3 && !s_list.nets[0].hidden, "-1 on a new network is 0");
    remember("Hidden", "ph", 0);
    CHECK(load() == 3 && !s_list.nets[0].hidden, "0 clears the flag");
}

static void test_channel_follows_first_network(void) {
    fake_reset();
    remember("A", "pa", 0);
    fake_put("wifi_channel", "6");
    remember("A", "pa", 0);
    CHECK(fake_get("wifi_channel") != NULL, "same first network keeps channel");
    remember("B", "pb", 0);
    CHECK(!fake_get("wifi_channel"), "a new first network drops the channel");
}

static void test_bad_list_is_ignored(void) {
    fake_reset();
    fake_put("ssid", "A");
    fake_put("password", "pa");
    fake_put("wifi_others", "{not json");
    expect_order((const char *[]){"A"}, 1);

    // A list that repeats the first network, or itself, loads once each.
    fake_put("wifi_others",
             "[{\"s\":\"A\",\"p\":\"x\"},{\"s\":\"B\",\"p\":\"pb\",\"h\":true},"
             "{\"s\":\"B\",\"p\":\"y\"},{\"p\":\"no name\"},{\"s\":\"\"}]");
    expect_order((const char *[]){"A", "B"}, 2);
    CHECK(strcmp(s_list.nets[0].password, "pa") == 0, "first network's password");
    CHECK(s_list.nets[1].hidden && strcmp(s_list.nets[1].password, "pb") == 0,
          "first copy of B wins");
}

static void test_clear_setup_forgets_networks(void) {
    fake_reset();
    remember("A", "pa", 0);
    remember("B", "pb", 1);
    fake_put("wifi_channel", "1");
    fake_put("device_state", "identity");
    CHECK(fake_get("wifi_others") && fake_get("wifi_hidden"), "set up");
    CHECK(config_clear_setup(), "clear setup");
    CHECK(load() == 0, "networks left after clear setup");
    CHECK(!fake_get("wifi_others"), "wifi_others left");
    CHECK(!fake_get("wifi_hidden"), "wifi_hidden left");
    CHECK(fake_get("device_state") != NULL, "device keys kept");
}

static void test_proxy_follows_its_network(void) {
    fake_reset();
    remember("Office", "wifi1", 0);
    network_proxy_config_t proxy = {.type = NETWORK_PROXY_SOCKS5, .host = "proxy.example",
                                     .port = 1080, .username = "agent", .password = "secret"};
    CHECK(wifi_known_set_proxy("Office", &proxy), "set remembered network proxy");
    CHECK(load() == 1 && s_list.nets[0].proxy.type == NETWORK_PROXY_SOCKS5, "primary round trip");
    CHECK(!strcmp(s_list.nets[0].proxy.password, "secret"), "proxy password round trip");
    remember("Home", "wifi2", 0);
    CHECK(load() == 2 && s_list.nets[0].proxy.type == NETWORK_PROXY_OFF, "new network defaults to direct");
    CHECK(s_list.nets[1].proxy.port == 1080, "moving to others keeps proxy");
    CHECK(!strcmp(s_list.nets[1].proxy.username, "agent"), "secondary auth round trip");
    remember("Office", "new-wifi-password", -1);
    CHECK(load() == 2 && s_list.nets[0].proxy.port == 1080, "password edit preserves proxy");
    CHECK(wifi_known_forget("Home"), "forget another network");
    CHECK(load() == 1 && s_list.nets[0].proxy.port == 1080, "forget preserves survivor proxy");
    CHECK(!wifi_known_set_proxy("missing", &proxy), "cannot create phantom networks");
    network_proxy_config_t off = {0};
    CHECK(wifi_known_set_proxy("Office", &off), "turn proxy off");
    CHECK(load() == 1 && s_list.nets[0].proxy.type == NETWORK_PROXY_OFF, "off round trip");
    CHECK(!fake_get("wifi_proxy"), "off erases credentials");
    CHECK(wifi_known_set_proxy("Office", &proxy), "set again");
    fake_put("ssid", "Downgraded Home");
    CHECK(load() == 1 && s_list.nets[0].proxy.type == NETWORK_PROXY_OFF,
          "older firmware moving the first network cannot attach its predecessor's proxy");
    CHECK(config_clear_setup() && !fake_get("wifi_proxy"), "full setup reset erases proxy");
    remember("Office", "wifi1", 0);
    fake_put("wifi_proxy", "{bad json");
    CHECK(load() == 1 && s_list.nets[0].proxy.type == NETWORK_PROXY_INVALID, "corrupt primary proxy fails closed");
    remember("Home", "wifi2", 0);
    CHECK(load() == 2 && s_list.nets[1].proxy.type == NETWORK_PROXY_INVALID, "invalid proxy survives MRU reorder");
    CHECK(wifi_known_forget("Office") && load() == 1, "forget removes network and proxy");
}

int main(void) {
    wifi_known_init();
    test_proxy_follows_its_network();
    test_empty_and_round_trip();
    test_legacy_single_network();
    test_remember_reorders_and_caps();
    test_forget();
    test_hidden_flag();
    test_channel_follows_first_network();
    test_bad_list_is_ignored();
    test_clear_setup_forgets_networks();
    puts("wifi_known: ok");
    return 0;
}
