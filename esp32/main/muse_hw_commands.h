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

#include "cJSON.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// The board's hardware as commands for the agent, on Muse boards with
// CONFIG_MUSE_HW_COMMANDS: the screen (text, brightness, sleep), touch and
// the buttons as events it can wait for, the RGB light, the speaker (clips
// from a URL, tones, volume), the mic's level and the battery. The face and
// settings keep working alongside.

// Takes ">hw {json}" lines on the serial console (tools/muse/hw.py), so the
// commands run without the cloud; results print as "@hw {...}" lines.
void muse_hw_commands_init(void);

// The console runs commands through Link's own dispatcher once it's set
// (app.c's, the one link.invoke uses), so every agent command works there.
void muse_hw_commands_set_dispatcher(noise_ctrl_command_cb dispatch);

// Adds the commands to link.register's commands_v2.
void muse_hw_commands_register(cJSON *commands);

// Runs `command` if it's one of these: a result, or {"_async": true} when the
// result follows through noise_ctrl_send_command_result(). NULL if it isn't.
cJSON *muse_hw_command(const char *command, cJSON *params,
                       const char *request_id,
                       noise_ctrl_session_generation_t session_generation);

#ifdef __cplusplus
}
#endif
