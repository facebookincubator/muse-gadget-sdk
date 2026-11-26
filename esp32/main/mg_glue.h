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

// Adds musegadgets BLE audio (components/muse_gadget_ble) to Link's BLE
// server, with this board's battery, name and mic. Only built with
// CONFIG_MUSE_GADGET_BLE_AUDIO. Call from app_run() once NVS is up, before
// the BLE server starts.
void mg_glue_start(void);

// Link setup stored or erased the token proof key ("mg_proof_k").
void mg_glue_setup_changed(void);

// A factory reset (setup reset): the musegadgets settings kept on the device
// (audio_codec 7, capture rate 13 and bitrate 14, the buzz, the queue) go
// back to their defaults at the restart that follows.
void mg_glue_settings_reset(void);

// The pairing changed hands (a setup reset, a new setup's commit): the
// offline clips go with it. true once none are left (mg_ble_queue_forget).
bool mg_glue_queue_forget(const char *why);
