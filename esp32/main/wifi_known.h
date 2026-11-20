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

#pragma once

#include <stdbool.h>
#include "network_proxy.h"

// The Wi-Fi networks this device remembers, most recently joined first.
// The first is kept in the "ssid" and "password" keys, as before, so code
// that only knows one network still finds it; "wifi_hidden" marks it as a
// network that doesn't broadcast its name. The rest are a JSON list in
// "wifi_others". "wifi_channel" belongs to the first network and is erased
// whenever a different network becomes first.
//
// Each record also carries its proxy; the first record uses "wifi_proxy",
// bound to its SSID so old firmware reordering cannot move the proxy.
// These read and write NVS: call them only from tasks with internal-RAM
// stacks.

#define WIFI_KNOWN_MAX 8
#define WIFI_KNOWN_SSID_MAX 32
#define WIFI_KNOWN_PASS_MAX 64

typedef struct {
    char ssid[WIFI_KNOWN_SSID_MAX + 1];
    char password[WIFI_KNOWN_PASS_MAX + 1];
    network_proxy_config_t proxy;
    bool hidden;          // joined by name; doesn't show in scans
} wifi_known_net_t;

typedef struct {
    int count;
    wifi_known_net_t nets[WIFI_KNOWN_MAX];
} wifi_known_list_t;

// Creates the lock. Call once, after config_store_init().
void wifi_known_init(void);

// Reads the list. Returns the count (0 if nothing is saved).
int wifi_known_load(wifi_known_list_t *list);

// Puts a network first, adding it or moving it up, and drops the oldest
// beyond WIFI_KNOWN_MAX. hidden: 1 or 0 sets the flag, -1 keeps the saved
// one (0 for a new network).
bool wifi_known_remember(const char *ssid, const char *password, int hidden);

// Forgets one network. If it was first, the next one takes its place.
// Returns false only if saving failed.
bool wifi_known_forget(const char *ssid);

// Forgets every network.
bool wifi_known_forget_all(void);

// Clears the passwords from a list before it goes out of scope.
void wifi_known_wipe(wifi_known_list_t *list);

// Existing remembered networks only. NULL is not an Off setting.
bool wifi_known_set_proxy(const char *ssid, const network_proxy_config_t *proxy);
bool wifi_known_get_proxy(const char *ssid, network_proxy_config_t *out);
