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

// BLE central role beside the gadget's own GATT server: scan for other
// devices, connect to one, and talk GATT to it. Shares the one NimBLE host
// that ble_server starts; call only after ble_server_start() and host sync.
// Blocking calls: run them from a worker task, never from the NimBLE host
// task or a GAP/GATT callback.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    bool connectable;
    char name[32];
} ble_central_dev_t;

// Scan for `duration_ms`. Interval and window are in ms; a window below the
// interval leaves the radio to Wi-Fi the rest of the time. Fills up to `max`
// unique devices (by address) and returns how many, or -1 on error.
// *reports gets the number of advertising reports seen, duplicates included.
int ble_central_scan(uint32_t duration_ms, bool active, uint16_t itvl_ms,
                     uint16_t window_ms, ble_central_dev_t *out, int max,
                     uint32_t *reports);

// Find a device advertising `name` (scanning up to timeout_ms).
bool ble_central_find(const char *name, uint32_t timeout_ms, ble_central_dev_t *out);

typedef struct {
    uint16_t conn_handle;
    uint16_t mtu;
    uint32_t connect_ms;   // connect request to link up
    uint32_t mtu_ms;       // MTU exchange
    uint32_t discover_ms;  // primary service discovery
    int services;
    char gap_name[32];     // Device Name characteristic (0x2A00)
} ble_central_session_t;

// Connect, exchange MTU, discover primary services and read the Device Name
// characteristic, then disconnect unless keep_open. Returns true when every
// step succeeded.
bool ble_central_probe(const ble_central_dev_t *dev, uint32_t timeout_ms,
                       bool keep_open, ble_central_session_t *out);

// Terminate a connection kept open by ble_central_probe().
void ble_central_disconnect(uint16_t conn_handle);

#ifdef __cplusplus
}
#endif
