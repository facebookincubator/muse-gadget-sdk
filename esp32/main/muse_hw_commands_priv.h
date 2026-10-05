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

// Shared by the hardware command files (muse_hw_commands.c, muse_hw_camera.c).

#include <stdbool.h>
#include <stddef.h>

#include <stdint.h>

#include "cJSON.h"
#include "muse_hw.h"
#include "noise_control.h"

typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
} hw_reply_to_t;

// Results: {"ok": false, "error": {code, message}}, {"ok": true, "payload"},
// and the marker for one that follows through hw_reply().
cJSON *hw_error(const char *code, const char *message);
cJSON *hw_ok(cJSON *payload);
cJSON *hw_async(void);
void hw_reply_to(hw_reply_to_t *to, const char *request_id,
                 noise_ctrl_session_generation_t session_generation);
// Sends a result for a command that answered hw_async(): to the control
// session, or to the serial console.
void hw_reply(const hw_reply_to_t *to, cJSON *result);

bool hw_int(cJSON *params, const char *key, int *out);
const char *hw_str(cJSON *params, const char *key);
int hw_clamp(int v, int lo, int hi);

// Registration: a parameter, an object of one, and a command in commands_v2.
cJSON *hw_param(const char *type, const char *description);
cJSON *hw_params(const char *name, cJSON *param);
void hw_add(cJSON *commands, const char *name, const char *description,
            cJSON *required, cJSON *optional, int timeout_ms);

// Results for on-device scripts (muse_script.c) carry this generation; the
// serial console's, 0. Neither is ever a control session's.
#define HW_SCRIPT_SESSION ((noise_ctrl_session_generation_t)UINT64_MAX)
#define HW_CONSOLE_SESSION ((noise_ctrl_session_generation_t)0)

// Link's command dispatcher (app.c's), once set; NULL before.
noise_ctrl_command_cb hw_dispatch(void);

// An event as input.read reports it (the fields for its type), for scripts too.
cJSON *hw_event_json(const muse_hw_event_t *ev, int64_t now_us);

// On-device scripts (muse_script.c), with CONFIG_MUSE_SCRIPTS.
void muse_script_init(void);
void muse_script_register(cJSON *commands);
cJSON *muse_script_command(const char *command, cJSON *params);
// A result for a script's command (request_id "script:<token>"); takes it.
void muse_script_result(const char *request_id, cJSON *result);

// base64: 4 characters per 3 bytes plus a NUL; and back (out holds len / 4 * 3).
size_t hw_base64(const uint8_t *in, size_t n, char *out);
size_t hw_base64_decode(const char *in, size_t len, uint8_t *out);

// The expansion ports (muse_hw_io.c): grove.*, i2c.*, uart.*, and the chip's
// temperature for device.status.
void muse_hw_io_init(void);
void muse_hw_io_register(cJSON *commands);
// may_save: false from a script, whose task can't write settings.
cJSON *muse_hw_io_command(const char *command, cJSON *params, bool may_save);
void muse_hw_io_status(cJSON *payload);

// The SD card (muse_hw_storage.c): storage.*, and files for other commands.
// Paths are relative to the card. NULL, or what went wrong.
void muse_hw_storage_init(void);
void muse_hw_storage_register(cJSON *commands);
cJSON *muse_hw_storage_command(const char *command, cJSON *params);
const char *hw_storage_save(const char *path, const void *data, size_t len, bool append);
// PSRAM, for the caller to free; NULL with *err set.
uint8_t *hw_storage_load(const char *path, size_t max, size_t *len, const char **err);

// Downloads up to max bytes (at most 1 MB) into a PSRAM buffer the caller
// frees. NULL, or what went wrong.
const char *hw_download(const char *url, size_t max, uint8_t **data, size_t *len);

// Apps (muse_hw_apps.c): app.*, kept on the scripts' partition.
void muse_hw_apps_init(void);
void muse_hw_apps_load(void);
void muse_hw_apps_remove_owned(const char *script);
void muse_hw_apps_register(cJSON *commands);
cJSON *muse_hw_apps_command(const char *command, cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t session_generation);
// The last crash (muse_crash.c): logged at boot, and last_crash in device.status.
void muse_crash_init(void);
void muse_crash_json(cJSON *payload);

// The pet (muse_hw_pet.c): pet.*.
void muse_hw_pet_register(cJSON *commands);
cJSON *muse_hw_pet_command(const char *command, cJSON *params);
// A script saved to start at boot, and started now if run (muse_script.c):
// for the app library. Not on a script's own task, which can't write flash.
bool muse_script_put(const char *name, const char *src, bool run, char *err, size_t errlen);
// The scripts' LittleFS partition is mounted (muse_script.c).
bool muse_script_storage(void);

// The camera's commands (muse_hw_camera.c), on boards with an SSCMA camera.
void muse_hw_camera_register(cJSON *commands);
// A script was stopped or deleted: stops the camera's live modes it started.
void muse_hw_camera_script_ended(const char *script);
// NULL if `command` isn't one of them.
cJSON *muse_hw_camera_command(const char *command, cJSON *params, const char *request_id,
                              noise_ctrl_session_generation_t session_generation);
