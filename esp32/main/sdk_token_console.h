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

// Setting the SDK token over serial, which the web flasher does after
// installing a build that has no token of its own (web-flasher/index.html).
//
// The command is the line ">sdk_token=mgst_..."; the board answers
// "@sdk_token ok" and restarts with the token, or "@sdk_token error <why>".
// It's serial-only: BLE CMD writes never reach it, so no one nearby can
// change a gadget's token.

// Handles one console line, without its leading '>'. False if the line isn't
// this command. Muse boards' console calls it (muse_input_set_console_hook).
bool sdk_token_console_command(const char *line);

// Reads '>' lines on boards without the Muse console: on the console UART,
// behind the board's USB bridge, and on the chip's own USB Serial/JTAG port,
// which mirrors the log, so the token can come in on whichever the browser
// opened.
void sdk_token_console_start(void);
