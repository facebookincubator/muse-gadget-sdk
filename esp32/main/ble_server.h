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
#include <stdint.h>

#include "sdkconfig.h"

typedef void (*ble_provision_cb)(const char *ssid, const char *password,
                                 const char *access_token,
                                 const char *refresh_token,
                                 const char *username,
                                 const char *ota_url,
                                 bool ota_force,
                                 const char *api_url,
                                 const char *api_url_v2,
                                 const char *noise_host,
                                 uint32_t session_generation);
typedef void (*ble_ota_cb)(const char *url, bool force);
typedef void (*ble_simple_cb)(void);

typedef struct {
    ble_provision_cb on_provision;
    ble_simple_cb    on_wifi_scan;
    ble_ota_cb       on_ota;
    ble_simple_cb    on_unpair;
    ble_simple_cb    on_get_device_info;
    ble_simple_cb    on_client_connected;
    ble_simple_cb    on_client_disconnected;
    void (*on_pairing_client_finished)(uint32_t session_generation);
} ble_callbacks_t;

struct ble_gatt_svc_def;
struct ble_gap_event;

// A second GATT service sharing this server (Muse's phone setup). Set it before
// ble_server_start(): configure_host runs before the host starts, and
// on_gap_event sees every GAP event; its return value is the event's result.
typedef struct {
    const struct ble_gatt_svc_def *svcs;
    void (*configure_host)(void);
    int (*on_gap_event)(struct ble_gap_event *event);
} ble_companion_t;

void ble_server_set_companion(const ble_companion_t *companion);
// Keep advertising for the companion service even after setup is complete.
void ble_server_set_companion_advertising(bool enabled);
bool ble_server_is_started(void);
// Host and controller synced (ready for GAP and GATT calls).
bool ble_server_is_synced(void);
bool ble_server_is_advertising(void);
// The public address, little-endian. False while BLE is down.
bool ble_server_own_address(uint8_t addr[6]);
// Stop the stack and free its heap but keep the controller's static memory,
// so ble_server_start() can bring it back (ble_server_full_shutdown()
// releases that too, for good).
bool ble_server_power_off(void);
#if CONFIG_HOMEHUB_BLE_PERSISTENT
// Advertising interval after setup (20-10240 ms); applies at once.
void ble_server_set_companion_interval_ms(int interval_ms);
int ble_server_companion_interval_ms(void);
#endif

void ble_server_start(const char *device_name, const ble_callbacks_t *cb);
void ble_server_begin_advertising(void);
void ble_server_stop_advertising(bool disconnect_client);

// Disconnect the current GATT client without changing advertising policy.
void ble_server_disconnect_client(void);
bool ble_server_has_connection(void);

// Send a short status string as a single notification.
void ble_server_send_status(const char *status);
// Deferred pairing work must use these token-bound operations. Stale work is
// discarded, and disconnect executes on the BLE host queue.
bool ble_server_send_pairing_status(const char *status, uint32_t generation);
void ble_server_disconnect_pairing_session(uint32_t generation);

// Send a large payload using framed chunked notifications.
//   byte 0: 0xFE magic
//   byte 1: chunk index (0-based)
//   byte 2: total chunk count
//   bytes 3..: payload fragment
// Max 160 bytes per packet, 50ms stagger between chunks.
// Safe to call from worker tasks; serialized internally.
void ble_server_send_chunked(const char *data);
// Allocate the encrypted record counter and send it in the same TX order.
bool ble_server_send_encrypted_json(const char *json, uint32_t generation);

// Disconnect the BLE client after a delay (ms), giving time for any
// pending status notification to be sent before the link drops.
void ble_server_delayed_disconnect(uint32_t ms);

// Diagnostic: fully shut down the BLE controller so it stops contending
// for the shared radio. After this call, BLE cannot be brought back up
// without a reboot.
void ble_server_full_shutdown(void);
