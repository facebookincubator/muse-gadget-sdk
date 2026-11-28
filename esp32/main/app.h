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

void app_run(void);

// How joining one of the saved Wi-Fi networks went.
typedef enum {
    APP_WIFI_JOINED,
    APP_WIFI_NOT_NEARBY,    // no saved network showed up in a scan
    APP_WIFI_FAILED,        // one did, but joining it failed
    APP_WIFI_BUSY,          // another setup operation had the radio
} app_wifi_join_t;

// Not now: a tap anywhere but the prompt's confirm, or the back gesture, ends
// the pairing prompt (first setup or re-setup) as a dismiss does; the phone can
// start it again.
void app_dismiss_pairing_prompt(void);
// Setup mode now (resetup_gap.h): a set-up gadget whose only key is its talk key takes a re-setup only in it.
bool app_setup_mode(void);
// A set-up gadget's pairing_client_hello may start a re-setup: not in SAFE, and on a board whose only key is its
// talk key only in setup mode (ble_server.c answers error_pairing_unavailable otherwise).
bool app_resetup_open(void);

#if CONFIG_MUSE_ENABLED
// Muse (muse_glue.c) drives setup through these; all are safe from any task.

// Remembers a Wi-Fi network first among the saved ones (hidden: it doesn't
// broadcast its name), or with an empty ssid forgets them all.
void app_wifi_set_credentials(const char *ssid, const char *password, bool hidden);
// Forgets one saved network; the next takes its place if it was first.
bool app_wifi_forget(const char *ssid);

// Joins a saved network under the setup operation gate: scans, then tries the
// saved networks that showed up, strongest first, and probes for the hidden
// ones. first_only joins just the first saved network, without scanning (the
// one just chosen). If paired, the heartbeat then reaches the VM (retrying
// until the session runs). Nothing here wakes the screen.
app_wifi_join_t app_wifi_join_saved(int timeout_ms, bool first_only);
// When the last join that scanned, this one or the one at boot, found no saved
// network nearby (esp_timer_get_time()); 0 if it found one.
int64_t app_wifi_none_nearby_at(void);
// Drops the VM session and leaves Wi-Fi, so the radio can sleep;
// app_wifi_join_saved() brings both back. Returns false when busy.
bool app_wifi_nap(void);
// Starts the BLE server if needed and keeps it advertising (or not) for Muse's
// phone-setup service once setup is complete.
void app_ble_companion_set(bool advertise);
// A press of a control that can confirm pairing (the talk key, a second key, the prompt's on-screen Confirm).
// Returns true when a pairing waits for a press: the press is Link's then, and on a Muse Gadget one press confirms
// it, whenever it began (Home Link: only one that began after pressed_after_us, the prompt). talk_key: the talk
// key, which never confirms a re-setup outside setup mode: it only dismisses the prompt.
bool app_confirm_pairing_press(int64_t pressed_after_us, bool talk_key);
// Full setup reset and reboot, from a worker task.
void app_reset_setup_async(void);
#endif

#if CONFIG_MUSE_GADGET_BLE_AUDIO
// The token proof's clear (mgcommands.h): erases the tokens, the proof key
// and the setup markers, then restarts into Link setup. From any task.
void app_gadget_clear_setup(void);
// The device session's credentials_update: config_set_token_pair() (one
// commit through the re-provisioning journal), refused (false) while the
// gadget is being set up again. From the mg worker.
bool app_gadget_store_session_tokens(const char *access_token, const char *refresh_token,
                                     uint64_t access_expires_at);
// The gadget is being set up again (a re-setup's journal begin until its
// commit has ended the old setup's sessions, or a rollback): the device
// session hands out no credentials meanwhile. From any task.
bool app_gadget_session_busy(void);
// The stored token pair, under the lock every pair read and write takes
// (config_get_token_pair()). From any task.
bool app_gadget_read_token_pair(char *access_token, size_t access_size, char *refresh_token,
                                size_t refresh_size, uint64_t *access_expires_at);
// The device session's credential in one snapshot under that lock: the node
// id ("" if none stored), the pair with its expiry and the token proof key
// (hex, "" if none), all one setup's. False with no pair, or while a
// re-setup holds the pair (app_gadget_session_busy()). From any task.
bool app_gadget_read_session_setup(char *node_id, size_t node_size, char *access_token,
                                   size_t access_size, char *refresh_token, size_t refresh_size,
                                   uint64_t *access_expires_at, char *proof_k_hex,
                                   size_t proof_k_size);
#endif

#if CONFIG_MUSE_ENABLED || CONFIG_HOMEHUB_VOICE
// Looks up a VM credential from the paired account (want_vm: a VM id, or empty
// for the preferred VM). *vm_token is heap; free() it.
bool app_hatch_vm_credentials(const char *want_vm, char *vm_id, size_t id_cap,
                              char *vm_name, size_t name_cap, char **vm_token);
#endif
