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

/*
 * Muse's phone setup service (muse_ble.c, Wi-Fi builds): which writes it
 * takes. Its writes already need a passkey-authenticated bond (the code on
 * the screen). The ones that change the gadget's Wi-Fi or Muse account
 * (Wi-Fi credentials, the Muse host, VM and token) aren't in a release build
 * at all: the Muse app sets Wi-Fi and the account up through Link setup. A
 * test build (CONFIG_MUSE_GADGET_TEST_BUILD) takes them only while the
 * gadget isn't set up (no device token, no Muse token set by hand), or
 * within MUSE_BLE_SETUP_WINDOW_US after the user turns Phone setup on in
 * Settings. Once set up, they're refused until Reset pairing (as setup v2's
 * own rules). Only the BLE write is gated; the USB console's lines run in
 * every build with no window (muse_ble.c). Pure, for the host tests.
 */

#include <stdbool.h>
#include <stdint.h>

#define MUSE_BLE_SETUP_WINDOW_US (10LL * 60 * 1000 * 1000)

/* A command that changes the gadget's Wi-Fi or Muse account. */
bool muse_ble_gate_is_provisioning(const char *cmd);

/* Whether cmd may run: set_up = a device token (or a hand-set Muse token) is
 * stored; window_until_us = the end of the user's setup window (0: none). */
bool muse_ble_gate_allows(const char *cmd, bool set_up, int64_t now_us, int64_t window_until_us);

/* Whether cmd is in this build at all: a release build (test_build false)
 * takes none of the provisioning writes over the air; the other commands and
 * the status read stay. */
bool muse_ble_gate_in_build(const char *cmd, bool test_build);

/* Whether a write may come over this link at all: encrypted with a bond
 * made with the passkey (authenticated) over LE Secure Connections. Keys
 * from a legacy pairing never count, whatever the build's security manager
 * options allow for other BLE roles. */
bool muse_ble_gate_link_ok(bool encrypted, bool authenticated, bool secure_connections);
