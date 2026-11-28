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
 * HID over GATT (HOGP) beside the musegadgets service, on the same
 * connection (CONFIG_MUSE_GADGET_BLE_HID): the HID service (0x1812) with the
 * report map of mg_ui.c, and NimBLE's security manager with bonding (Just
 * Works) so the phone's OS can be its HID host. The reports need an
 * encrypted link: a phone that reads the Report Map (the test app, or the
 * OS's HID host) gets "insufficient encryption" and pairs.
 */

#include <stdbool.h>
#include <stdint.h>

#include "mg_ui.h"

struct ble_gatt_svc_def;
struct ble_gap_event;

/* The HID service, for the shared GATT database. */
const struct ble_gatt_svc_def *mg_hogp_service(void);
/* Before the host starts: the security manager (bonding, Just Works) and the bond store. */
void mg_hogp_configure_host(void);
/* Every GAP event: the connection, encryption, the host's subscriptions. True if the HID status changed. */
bool mg_hogp_gap_event(struct ble_gap_event *ev);
/* mg_hid_status_flags_t: bonded (an encrypted, bonded link), host (input reports subscribed). */
uint8_t mg_hogp_status(void);
/* The link is encrypted and the host takes this report: sending it can work. */
bool mg_hogp_ready(uint8_t report_id);
/* Notifies one input report; false if the stack had no room (try again) or it can't go. */
bool mg_hogp_send(const mg_hid_report_t *r);
/*
 * Shows or hides the HID service. Hidden (the default), phones don't see a
 * HID device at all: macOS and iOS ask to pair with any HID device an app
 * connects to, and nobody should see that prompt before choosing HID. The
 * first hid_map with a mode shows it for good (the gadget remembers); a
 * change while connected sends Service Changed (bonded peers rediscover; an
 * unbonded app rediscovers services itself).
 */
void mg_hogp_set_visible(bool visible);
bool mg_hogp_visible(void);

/* The peer on conn holds a bond with this gadget. */
bool mg_hogp_peer_bonded(uint16_t conn);
/* Deletes NimBLE's phone-address records (see store_status in mg_hogp.c),
 * which ble_store_clear() leaves behind. Returns how many went. */
int mg_hogp_forget_addr_records(void);
