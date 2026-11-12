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

#include "esp_err.h"

/*
 * BLE "phone setup" service. Home Link owns the NimBLE stack and advertises as
 * "Muse-XXXXXX"; this adds one GATT service to Link's server:
 *
 *   CMD    (write)       "key=value" text: wifi.ssid, wifi.pass, wifi.connect,
 *                        wifi.forget (=ssid forgets that one, bare forgets all
 *                        saved networks), hatch.host, hatch.vm, hatch.token,
 *                        hatch.token+ (append chunk), hatch.test, volume,
 *                        mic_gain, brightness, sleep; on the WG1200 also
 *                        card.prompt, card.prompt+ (append) and card.prompt?
 *   STATUS (read/notify) JSON snapshot; notifies a short result after each CMD
 *
 * Both require an encrypted, MITM-protected link: the phone pairs with the
 * 6-digit passkey Muse shows on screen. See tools/ble_setup.html.
 */

typedef enum {
    MUSE_BLE_OFF,
    MUSE_BLE_ADVERTISING,
    MUSE_BLE_CONNECTED,
} muse_ble_state_t;

typedef struct {
    muse_ble_state_t state;
    uint32_t passkey;       /* non-zero while a phone is pairing: show it */
    bool secure;            /* current link is encrypted + authenticated */
    /* Long enough for the full "MuseGadget-XXXXXX": the last hex digits are
     * what tells two gadgets apart, so this must not truncate. */
    char name[32];
} muse_ble_status_t;

struct ble_gatt_svc_def;
struct ble_gap_event;

/* Hooks for Link's BLE server (ble_server_set_companion), set before it starts. */
const struct ble_gatt_svc_def *muse_ble_services(void);
/* Security manager config: display-only passkey pairing with bonding. */
void muse_ble_configure_host(void);
/* Sees every GAP event on Link's connection; returns the event's result code. */
int muse_ble_gap_event(struct ble_gap_event *ev);
void muse_ble_set_name(const char *name);

/* Keeps advertising after Link setup to match settings. */
void muse_ble_apply(void);
void muse_ble_status(muse_ble_status_t *out);
void muse_ble_forget_all(void);
/* Runs one CMD line ("key=value", no newline) as if written over BLE; the
 * serial console uses it for USB setup. */
void muse_ble_command(char *cmd);
/* The STATUS characteristic's JSON; returns its length, as snprintf does. */
int muse_ble_status_json(char *out, size_t len);
