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

#ifndef MG_SETUP_H_
#define MG_SETUP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Muse Link setup on the gadget (protocols/README.md, Gadget setup over Muse
 * Link): the setup service's RX/TX characteristics with 0xFE chunk framing,
 * community pairing v5 (P-256, physical confirmation with the talk button,
 * pairing_encrypted records), get_device_info with wifi "none" and
 * mgcommands 1, and token-only provision_v2. It follows the ESP32 reference
 * (esp32/main/ble_server.c, link_pairing.c, app.c) message for message:
 * the same dispatch order, statuses, plaintext rules, phases, timeouts and
 * record counters.
 *
 *   hello -> pairing_ready (60 s for client_finished)
 *   client_finished -> confirm_required (60 s for a press)
 *   press -> pairing_confirmed, with sdk_token from CONFIG_MG_SDK_TOKEN
 *            (120 s for provision_v2)
 *   provision_v2 -> tokens, K and the markers stored (mg_setup_store.h)
 *            -> auth_ok; Link setup advertising stops
 *
 * Once set up, the gadget is found by its mg advertisement and takes a new
 * setup on the same service (Gadget setup v2, re-provisioning): the same
 * handshake and press, and a provision_v2 whose commit replaces the tokens
 * and K (until then the old ones stay). A reset (button held at power-up,
 * or the token proof's clear) erases it all and advertises Link setup
 * again. Everything runs on the app work queue.
 */

#define MG_SETUP_SERVICE_UUID_LE                                                                   \
	0x0c, 0x24, 0x5f, 0xcf, 0x4e, 0x31, 0x46, 0x8b, 0xcf, 0x46, 0xea, 0x38, 0x1c, 0x3d, 0xdd,  \
		0x7f
#define MG_SETUP_RX_UUID_LE                                                                        \
	0x01, 0x9b, 0x8f, 0x5e, 0x2d, 0x3c, 0xf0, 0xa1, 0x6e, 0x4a, 0xa2, 0x28, 0x29, 0x30, 0x59,  \
		0x4d
#define MG_SETUP_TX_UUID_LE                                                                        \
	0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0a, 0x8f, 0x9c, 0x4e, 0x2b, 0x7b, 0xca, 0xc4, 0x5d,  \
		0xd7

/* The image's SDK token slot (CONFIG_MG_SDK_TOKEN_SLOT, src/mg_token_slot.c):
 * MG_SDK_TOKEN_SLOT_LEN bytes holding the marker, then NULs, until an updater
 * writes a token and NULs over all of it and signs the image again (a web
 * updater can do that in the browser, so no image with a token is ever
 * stored or served). The marker still there means no token. */
#define MG_SDK_TOKEN_SLOT_MARKER "MG-SDK-TOKEN-SLOT-v1:"
#define MG_SDK_TOKEN_SLOT_LEN    128

/* Phase timeouts, as the ESP32. */
#define MG_SETUP_CLIENT_FINISHED_MS (60 * 1000)
/* No timer on the confirm prompt: it stays until the press, the link's end
 * or the phone giving up. */
#define MG_SETUP_CONFIRMED_IDLE_MS  (120 * 1000)
#define MG_SETUP_PROVISIONING_MS    (120 * 1000)

void mg_setup_init(void);

/* A write on the setup RX characteristic. */
void mg_setup_rx(const uint8_t *buf, size_t len);
void mg_setup_connected(void);
void mg_setup_disconnected(void);

/* A press while a pairing waits (the button, or the bench console's
 * >pair.confirm): one press confirms a first setup and a re-setup alike,
 * with no hold, no press time and no edge check; on a release build only a
 * real press (real: the button's GPIO) confirms, an injected one never. True
 * if a pending pairing took it (confirmed, refused, or swallowed before the
 * prompt is up), so it isn't push-to-talk. */
bool mg_setup_button(bool real);
/* The back gesture on a board that has one: the prompt closes. */
void mg_setup_dismiss(void);
/* Setup mode: the only time a set-up gadget whose only key
 * is the talk key takes a re-setup: the button held at boot into the normal
 * firmware (main.c), for MG_SETUP_MODE_MS or until a re-setup is confirmed.
 * Always on in test and bench builds. */
#define MG_SETUP_MODE_MS (5 * 60 * 1000)
void mg_setup_mode_begin(void);
bool mg_setup_mode(void);
/* get_device_info's device_key_auth, 1 or 0 (a keyless setup), for a test. */
void mg_setup_set_device_key_auth(int v);

/* A pairing waits for the button (confirm_required went out). */
bool mg_setup_confirm_pending(void);

/* Advertise the Link setup service (until setup is complete). */
bool mg_setup_advertise_link(void);

/* What a read of the TX characteristic returns: the last plaintext status,
 * or "encrypted_status". */
size_t mg_setup_read_status(char *out, size_t cap);

/* Erases the setup (tokens, K, markers), ends any setup session and
 * advertises Link setup again. 0 or the erase error. */
int mg_setup_factory_reset(const char *why);
/* The same, leaving the device session to end itself (the sealed clear,
 * whose answer goes after the erase) when end_session is false. */
int mg_setup_erase(const char *why, bool end_session);

/* At boot, after mg_queue_init() and the update's slot check, and when the
 * queue takes the slot back from a firmware update: starts the erase of
 * offline clips an owner change still owes (a power loss, a failed erase, a
 * firmware update holding the store). */
void mg_setup_clips_resume(void);

#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
/* Test hook (host and unit tests only): the next hello uses this P-256
 * private scalar and nonce. */
void mg_setup_test_set_ephemeral(const uint8_t priv[32], const uint8_t nonce[16]);
#endif

#endif
