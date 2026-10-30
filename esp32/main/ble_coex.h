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

// Wi-Fi first radio sharing while BLE stays up (CONFIG_HOMEHUB_BLE_COEX).
//
// The chip has one RF path for Wi-Fi (2.4 and 5 GHz) and BLE. ESP-IDF's
// coexistence scheduler picks a time-slice scheme from status bits that the
// BLE controller reports: while any of them is set it gives BLE half of every
// beacon period (about 51 ms of each 102 ms), even when BLE has nothing to
// send, and Wi-Fi traffic stalls for that half. On the C5 that halves the
// Noise tunnel's throughput. BLE events still ask for the radio one by one
// with their own priorities, so withholding a bit from the scheduler keeps
// that BLE activity working without reserving half the airtime for it.
//
// Status bits the C5's NimBLE controller reports (ESP-IDF v6.0):
//   BLE_COEX_ADV   advertising (set from boot; cleared while connected)
//   BLE_COEX_SCAN  scanning, including the scan before a central connect
//   BLE_COEX_CONN  a connection is up
//
// The policy only applies while ble_coex_set_active(true): the app turns it
// on once setup is complete, so provisioning keeps ESP-IDF's balanced scheme.

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_COEX_ADV  0x1u
#define BLE_COEX_SCAN 0x2u
#define BLE_COEX_CONN 0x4u

#if CONFIG_HOMEHUB_BLE_COEX

// Turn the policy on or off. Off hands the scheduler every bit the controller
// has set, as if the policy had never run.
void ble_coex_set_active(bool active);

// The BLE status bits withheld from the scheduler while active. Defaults to
// the Kconfig choice; takes effect at once.
void ble_coex_set_withheld(uint32_t bits);
uint32_t ble_coex_withheld(void);

// BLE bits the controller has set (whether or not they are withheld).
uint32_t ble_coex_controller_bits(void);
bool ble_coex_active(void);
// BLE status bits outside ADV/SCAN/CONN the controller has set since boot:
// non-zero means this ESP-IDF uses bits ble_coex doesn't know.
uint32_t ble_coex_unknown_bits(void);

#else

static inline void ble_coex_set_active(bool active) { (void)active; }
static inline void ble_coex_set_withheld(uint32_t bits) { (void)bits; }
static inline uint32_t ble_coex_withheld(void) { return 0; }
static inline uint32_t ble_coex_controller_bits(void) { return 0; }
static inline bool ble_coex_active(void) { return false; }
static inline uint32_t ble_coex_unknown_bits(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif
