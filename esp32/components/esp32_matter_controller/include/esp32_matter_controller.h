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

// The matter.* Link commands. main/app.c passes each one to
// esp32_matter_controller_command(), which runs it on an on-device Matter
// controller (esp-matter's commissioner, with a fabric of its own). Devices are
// added over the network, not over Bluetooth.
//
// main/noise_control.cpp advertises the commands (build_register_json()).
// Matter starts with the first command (a couple of seconds); results of slow
// commands come back through reply, once each.
#pragma once

#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sends the result of a command answered {"_async":true}; takes ownership.
typedef void (*esp32_matter_controller_reply_fn)(uint64_t session_generation, const char *request_id, cJSON *result);

// Handles a matter.* command: a result, or {"_async":true} and a reply later.
cJSON *esp32_matter_controller_command(const char *command, cJSON *params, const char *request_id,
                                       uint64_t session_generation, esp32_matter_controller_reply_fn reply);

// Forgets the fabric and every device on it (a setup reset; the device
// restarts right after).
void esp32_matter_controller_erase(void);

#ifdef __cplusplus
}
#endif
