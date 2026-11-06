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

// Muse commands for the Link's Bluetooth LE (CONFIG_HOMEHUB_BLE_COMMANDS):
// ble.state, ble.power, ble.advertise, ble.scan, ble.connect, ble.disconnect,
// ble.discover, ble.read, ble.write and ble.subscribe. They run one at a time
// on their own task and reply through noise_ctrl_send_command_result().

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the commands need from the rest of the firmware.
typedef struct {
    bool (*power)(bool on);              // bring the stack up or down
    bool (*is_on)(void);
    void (*advertise)(bool on, int interval_ms);   // interval 0: unchanged
    bool (*is_advertising)(void);
    int (*adv_interval_ms)(void);
    bool (*own_address)(uint8_t addr[6]);          // little-endian
    const char *(*name)(void);
    bool (*peripheral_connected)(void);            // a phone is connected to us
    void (*set_wifi_first)(bool on);
    bool (*wifi_first)(void);
} ble_commands_ops_t;

void ble_commands_init(const ble_commands_ops_t *ops);

// Add the commands to link.register's commands_v2 object.
void ble_commands_register(cJSON *commands);

// Whether `command` is one of these.
bool ble_commands_handles(const char *command);

// on_ws_command() for these: queues the command and returns {"_async": true},
// or an error result.
cJSON *ble_commands_handle(const char *command, const cJSON *params,
                           const char *request_id, uint64_t session_generation);

// The Muse session ended. Nothing owns the connections and subscriptions it
// opened any more, so they are closed, and Wi-Fi gets its radio priority back
// if a command took it. Returns at once; the work runs on the command task
// after the command in progress, and before any from a later session.
void ble_commands_session_ended(void);

// What ble_commands_session_ended() does, now, on the calling task.
void ble_commands_cleanup(void);

// Run one command now, on the calling task, and return its result
// ({"ok": true, "payload": ...} or an error). The worker uses this; so do
// the host tests.
cJSON *ble_commands_run(const char *command, const cJSON *params);

#ifdef __cplusplus
}
#endif
