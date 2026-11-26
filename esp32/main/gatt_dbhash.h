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
 * The Generic Attribute service with the Database Hash (0x2B2A) and Client
 * Supported Features (0x2B29) beside Service Changed (required where the controller is LL 5.1 or later, since Android's
 * robust caching reads the hash only then and otherwise rediscovers the
 * whole table on every connect). It replaces NimBLE's ble_svc_gatt, whose
 * hash needs CONFIG_BT_NIMBLE_GATT_CACHING (and its GATT client cache).
 *
 * The hash is computed on the device from its own live table: every
 * attribute the host registered, walked in handle order, with each
 * declaration's value read back as the server serves it. It's served only
 * if that walk accounts for every handle; otherwise reads of 0x2B2A fail and
 * a phone rediscovers, as without it. tools/gatt_dbhash.py computes the same
 * hash from the walk's dump (">gatt.dump") for a bench check.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In place of ble_svc_gatt_init(), after nimble_port_init(): adds the
 * service and starts recording the registration. */
void gatt_dbhash_init(void);
/* At the host's sync: walks the table and computes the hash. */
void gatt_dbhash_sync(void);
/* A connection came or went: its Client Supported Features start at zero. */
void gatt_dbhash_conn(uint16_t conn_handle, bool connected);
bool gatt_dbhash_served(void);
/* The walk and the hash on the console (bench). */
void gatt_dbhash_dump(void);

#ifdef __cplusplus
}
#endif
