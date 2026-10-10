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

// A basic GATT service that stays up after setup, next to Wi-Fi and the
// Noise session (CONFIG_HOMEHUB_BLE_PERSISTENT). It plugs into ble_server as
// its companion service, so it shares the one NimBLE host and connection.
//
// Service 4d475342-0001-4000-8000-6c696e6b6267:
//   ...0002  status      read, notify. Compact JSON: Wi-Fi, session and
//                        tunnel state, free internal/DMA/PSRAM heap, and the
//                        BLE byte counters below.
//   ...0003  throughput  write without response, notify
//                        (CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST only). Writes
//                        are counted and dropped; a 4-byte write is read as a
//                        little-endian byte count to notify back, and a 1-byte
//                        write resets the counters (status "rx", "rx_ms").

#include <stdbool.h>

#include "ble_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the service with ble_server. Call before ble_server_start().
void ble_basic_register(void);

// Whether a client is connected to the gadget's GATT server.
bool ble_basic_client_connected(void);

// Whether the NimBLE host has synced with the controller.
bool ble_basic_host_synced(void);

// Notify the status characteristic to a subscribed client, if any.
void ble_basic_notify_status(void);

#ifdef __cplusplus
}
#endif
