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

#include "host/ble_gatt.h"

#include <stdbool.h>

// The factory jig's service answers on a gadget that isn't set up, and on a
// set-up one only in its owner's setup window (Settings > Bluetooth > Phone
// setup, on boards with the full UI); otherwise both reads are refused with
// no data and the button isn't armed. Pure, for the host tests.
bool factory_test_allowed(bool set_up, bool window_open);

const struct ble_gatt_svc_def *factory_test_svcs(void);
void factory_test_on_button_press(void);
void factory_test_on_disconnect(void);
