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
#include <stdint.h>
#include <stddef.h>

// Persistent key/value config stored in NVS.
// Setup keys: ssid, password, wifi_channel, wifi_others, wifi_hidden, wifi_proxy (see
// wifi_known.h), access_token, refresh_token, vm_url, username, api_url,
// api_url_v2, noise_host, setup_complete, and on gadgets setup_wifi
// ("skipped" after token-only setup) and mg_proof_k (the token proof key,
// hex). Device-level keys are
// preserved by config_clear_pairing(), config_unpair() and config_clear_setup().
// auth_token is a legacy alias from early Link firmware and is only kept
// long enough to migrate by minting a device token pair.
// All getters return true on success and write a NUL-terminated string
// of up to (buf_size - 1) bytes into out. Missing keys return false.

// The avatar face's picture, the Muse's name, the face pick and its Size
// (mg_face.h) live in a namespace of their own and go with the pairing:
// config_clear_pairing() and config_clear_setup() erase it, and so does
// config_erase_face() (a completed Link setup, the console's face.erase).
#define CONFIG_STORE_FACE_NS "mg_face"
bool config_erase_face(void);

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

// Stores a refreshed token pair (access_token and refresh_token: the device
// session's credentials_update) and its access_expires_at (unix seconds; 0:
// none, and none is kept) in one commit through the re-provisioning journal
// below, K and node_id unchanged: a failed write or
// a power cut before the commit leaves the old pair and expiry. False if it
// wasn't stored (the old pair stays), and while a journal is open (a re-setup).
bool config_set_token_pair(const char *access_token, const char *refresh_token, uint64_t access_expires_at);
// The stored pair's access_expires_at (the committed one while a journal is
// open), 0 if none.
uint64_t config_get_token_expires(void);
// The stored token pair, never half of two: while the re-provisioning
// journal is open, the committed pair it keeps aside. (Builds of the device session
// before 3.1 journalled an update in "tok_update": config_store_init()
// finishes one such a build left, and this reads it while NVS refuses.)
// False if there's no pair (or it doesn't fit).
bool config_get_token_pair(char *access_token, size_t access_size, char *refresh_token, size_t refresh_size);
// The node id stored with that pair (and K, on a gadget) by setup
// (identity.h), the same way: while the journal is open, the committed
// setup's, kept aside, never the one a setup in progress wrote (the
// credentials a gadget hands over name the id its
// current pair is for). False if none is stored (not set up, or set up by
// firmware that stored none).
bool config_get_node_id(char *out, size_t size);
// The token proof key ("mg_proof_k", hex) the same way: while the journal is
// open, the committed setup's, kept aside, so a proof and the pair it unlocks
// are always one setup's. False if none is stored.
bool config_get_proof_key_hex(char *out, size_t size);

// Pairing convenience: removes tokens (and the token proof key), vm_url,
// username, account-specific endpoint overrides.
// WiFi credentials and any device-level keys are preserved.
bool config_clear_pairing(void);

// Unpair: config_clear_pairing() plus the setup-complete marker, so the device
// can pair again. WiFi credentials are preserved. Returns true only after
// every key is durably erased and a readback reports it absent.
bool config_unpair(void);

// Full local setup reset: removes WiFi, tokens, the token proof key, vm_url,
// username, endpoint overrides, and the setup-complete and Wi-Fi-skipped
// markers. Returns true
// only after every credential is durably erased and a readback reports it absent.
// Device-level keys are preserved.
bool config_clear_setup(void);

bool config_is_provisioned(void);
bool config_setup_complete(void);
/* Set up, or its marker can't be read (any NVS error but "not found"): who
 * fails closed asks this, so an unreadable marker never reads as unset. */
bool config_setup_complete_or_unreadable(void);
bool config_mark_setup_complete(void);
bool config_clear_setup_complete(void);
// Token-only gadget setup records that it skipped Wi-Fi, so boot recovery
// keeps a setup without Wi-Fi credentials.
bool config_wifi_skipped(void);
bool config_mark_wifi_skipped(void);

// Re-provisioning a set-up gadget (protocols/README.md, Gadget setup v2): its
// provisioning overwrites Link's setup keys (the tokens, the proof key, the
// Wi-Fi-skipped marker, the username and endpoint overrides, vm_url and the
// SDK token hint, the node id) one at a time, while the device stays set
// up; the device session's token updates use it too (config_set_token_pair()). NVS writes
// one key atomically, not several, so begin copies the old values aside
// (rp_*) and then writes the journal marker rp_open; the provisioning
// commit's last step is commit, which erases the marker: the commit point.
// Until then rollback copies the old values back, and so does recover at
// boot after a power loss, so the device is never left with parts of two
// setups. Wi-Fi networks are not part of it. config_clear_pairing() and
// config_clear_setup() erase the journal first.
bool config_reprovision_begin(void);
bool config_reprovision_open(void);
bool config_reprovision_commit(void);
// true once no journal is open (the old values are back, or none was open).
bool config_reprovision_rollback(void);
typedef enum {
    CONFIG_RECOVER_NONE = 0,       // no journal was open
    CONFIG_RECOVER_RESTORED = 1,   // the old setup is back
    CONFIG_RECOVER_FAILED = 2,     // the journal stays open: the old setup is still the one kept aside
} config_recover_t;
// At boot, before anything reads the setup.
config_recover_t config_reprovision_recover(void);
