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

/* confirm_disc_harness.c's stand-in for the BLE gadget's mg_ble.h: only what muse_ui.c calls with
 * CONFIG_MUSE_GADGET_BLE_AUDIO on, the update prompt's state (the harness defines them). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MG_BLE_UPDATE_UI_ASK 4
uint8_t mg_ble_update_ui(uint8_t *pct);
void mg_ble_update_ui_clear(void);
const char *mg_ble_update_stop_text(void);
void mg_ble_update_ui_shown(uint8_t state);
bool mg_ble_ptt_ready(void);
