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

#include <stddef.h>
#include <stdint.h>

// Both buffers must be at least 32 bytes. XXXXXX = last 3 WiFi STA MAC octets.
// node_id "homelink-XXXXXX" (lower) is the backend device identifier (used by
// device-token mint/refresh); ble_name "MuseGadget-XXXXXX" (upper) is the
// advertised BLE name.
void identity_init(void);
// Rebuild BLE/node ids from a Wi-Fi STA MAC (e.g. after Hosted bring-up).
void identity_set_sta_mac(const uint8_t mac[6]);
const char *identity_node_id(void);
const char *identity_ble_name(void);
const char *identity_mac(void);
const char *identity_device_id(void);
// The maker's SDK token (CONFIG_GADGET_SDK_TOKEN), or NULL when the build has none.
const char *identity_sdk_token(void);
