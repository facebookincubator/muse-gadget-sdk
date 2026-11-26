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
#include <stdint.h>

#include "cJSON.h"

// sdk_token may be NULL; when set it rides on the encrypted pairing_confirmed
// status so the app can present it at device-token mint.
void link_pairing_init(const char *node_id, const char *device_id,
                       const char *mac, const char *firmware_version,
                       const char *sdk_token);
void link_pairing_reset(void);
void link_pairing_add_device_info(cJSON *root);

// What provision_v2 needs of Wi-Fi, as device_info's "wifi" reports it
// (protocols/README.md, Gadget setup over Muse Link). Required is every
// non-gadget build's behaviour, and device_info then omits the field.
typedef enum {
    LINK_WIFI_REQUIRED = 0,
    LINK_WIFI_OPTIONAL,   // no ssid: token-only provisioning
    LINK_WIFI_NONE,       // a BLE-only gadget: ssid must be absent
} link_wifi_mode_t;

// Gadget builds call this before BLE starts: the Wi-Fi mode, and whether the
// device also serves the musegadgets service (device_info "mgcommands": 1).
void link_pairing_set_gadget(link_wifi_mode_t wifi, bool mgcommands);
link_wifi_mode_t link_pairing_wifi_mode(void);
bool link_pairing_mgcommands(void);

bool link_pairing_plaintext_setup_blocked(void);

const char *link_pairing_handle_client_hello(cJSON *root, char **response_json);
const char *link_pairing_decrypt_command(cJSON *root, char **plaintext_json);
// Successful confirmation transitions return an opaque nonzero session token.
uint32_t link_pairing_handle_client_finished(void);
bool link_pairing_confirmation_required(void);
// Arm physical input only after the encrypted prompt has been emitted.
bool link_pairing_arm_confirmation(uint32_t generation);
// A dismissed prompt: no press confirms it any more (its session ends with the
// link).
void link_pairing_disarm_confirmation(uint32_t generation);
// Home Link: only a press that began after the prompt was armed confirms it,
// within its window; pressed_after_us (esp_timer's clock) is a time before the
// press began, when its control was last seen up, at the latest. A Muse
// Gadget (CONFIG_MUSE_GADGET_BLE_AUDIO): one press while the prompt is up
// confirms, whenever it began (pressed_after_us unused), and the prompt has no
// timer.
uint32_t link_pairing_confirm_active_session(int64_t pressed_after_us);
bool link_pairing_session_confirmed(void);
uint32_t link_pairing_mark_provisioning_active(void);
// A nonzero token binds a worker to its originating provisioning session.
// Zero queries the current state for UI/timeout bookkeeping only.
bool link_pairing_provisioning_session_valid(uint32_t generation);
bool link_pairing_extend_provisioning_deadline(uint32_t generation);
// Validate and commit the final local NVS marker under the pairing lock.
// The callback must not perform network/BLE operations or re-enter pairing.
bool link_pairing_commit_provisioning(uint32_t generation, bool (*commit)(void));

// Tokens bind deferred work to one handshake phase, including expiry cleanup.
// Finished, physical confirmation, and provisioning invalidate earlier work
// without changing encryption keys or record counters.
uint32_t link_pairing_session_generation(void);
bool link_pairing_session_is_current(uint32_t generation);
// Record tokens remain valid across phase changes, until encryption keys reset.
bool link_pairing_record_session_is_current(uint32_t generation);
bool link_pairing_reset_session(uint32_t generation);

const char *link_pairing_sign_factory_test(void);

// Returns NULL when no encrypted setup session is ready. Caller owns result.
// A nonzero generation rejects stale work atomically with encryption; zero
// retains the generic status API for the currently active session.
char *link_pairing_encrypt_status(const char *status, uint32_t generation,
                                  uint32_t *record_generation);
char *link_pairing_encrypt_json(const char *plain_json, uint32_t generation,
                                uint32_t *record_generation);
