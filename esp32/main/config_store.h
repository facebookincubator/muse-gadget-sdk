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
#include <stddef.h>

// Persistent key/value config stored in NVS.
// Setup keys: ssid, password, wifi_channel, wifi_others, wifi_hidden (see
// wifi_known.h), access_token, refresh_token, vm_url, username, api_url,
// api_url_v2, noise_host, setup_complete. Device-level keys are
// preserved by config_clear_pairing(), config_unpair() and config_clear_setup().
// auth_token is a legacy alias from early Link firmware and is only kept
// long enough to migrate by minting a device token pair.
// All getters return true on success and write a NUL-terminated string
// of up to (buf_size - 1) bytes into out. Missing keys return false.

typedef enum {
    CONFIG_KEY_LOOKUP_ERROR = -1,
    CONFIG_KEY_NOT_FOUND = 0,
    CONFIG_KEY_FOUND = 1,
} config_key_lookup_t;

void config_store_init(void);

bool config_get_str(const char *key, char *out, size_t buf_size);
bool config_set_str(const char *key, const char *value);
bool config_erase_key(const char *key);
config_key_lookup_t config_key_lookup(const char *key);

// Pairing convenience: removes tokens, vm_url, username, account-specific
// endpoint overrides.
// WiFi credentials and any device-level keys are preserved.
bool config_clear_pairing(void);

// Unpair: config_clear_pairing() plus the setup-complete marker, so the device
// can pair again. WiFi credentials are preserved. Returns true only after
// every key is durably erased and a readback reports it absent.
bool config_unpair(void);

// Full local setup reset: removes WiFi, tokens, vm_url, username, endpoint
// overrides, and the setup-complete marker. Returns true
// only after every credential is durably erased and a readback reports it absent.
// Device-level keys are preserved.
bool config_clear_setup(void);

bool config_is_provisioned(void);
bool config_setup_complete(void);
bool config_mark_setup_complete(void);
bool config_clear_setup_complete(void);
