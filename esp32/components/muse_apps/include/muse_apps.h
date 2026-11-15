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

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WebAssembly apps (CONFIG_MUSE_WASM_APPS): Muse's app.run reads
 * an app (a folder holding app.wasm, or a .wasm file) from the VM over Link's
 * Noise session and runs it with WAMR's interpreter on the screen
 * (muse_app_view.h); app.stop, another app.run, the power key, a call or the
 * video view closes it. app.install keeps an app on the gadget (the "apps"
 * LittleFS partition) for its launcher, Apps in Settings, and app.run {id}.
 * One app runs at a time, on a task of its own. The API
 * apps import is components/muse_apps/sdk/api/api.json (REFERENCE.md beside it); components/muse_apps/sdk/gadget.h,
 * generated from it, declares it for C.
 *
 * The component is optional: without the option it builds nothing, and Link
 * reaches it only through these calls, each under CONFIG_MUSE_WASM_APPS.
 */

/* Answers an asynchronous command (noise_ctrl_send_command_result in
 * main/noise_control.h); takes the result. */
typedef void (*muse_apps_result_fn)(uint64_t session_generation, const char *request_id, cJSON *result);

/* Starts the app task, idle until an app is asked for. Once, at boot, after
 * Link has registered its operations (muse_link_register). */
void muse_apps_start(muse_apps_result_fn send_result);

/* link.register: adds app.install, app.list, app.remove, app.run, app.stop,
 * app.status, app.logs, app.api, files.put, files.list and files.remove to
 * its commands object. */
void muse_apps_advertise(cJSON *commands);

/* Runtime board/device/WASM description. Caller owns it; NULL on allocation
 * failure. Only present with CONFIG_MUSE_WASM_APPS. No hardware reads. */
cJSON *muse_apps_capabilities(void);
/* Append it to device.guide's payload; leaves payload unchanged on failure. */
bool muse_apps_add_capabilities(cJSON *payload);

/* on_ws_command(): the app.* commands. app.install, app.remove, app.run and
 * app.stop return {"_async": true}, the result coming later through
 * send_result, or an error result; app.list, app.status, app.logs and app.api
 * answer at once. */
bool muse_apps_handles(const char *command);
cJSON *muse_apps_command(const char *command, cJSON *params, const char *request_id, uint64_t session_generation);

/* The console's app lines (tools/muse/app.py): "app.begin", then "app+=B64"
 * pieces of a .wasm file and "app=B64" (the last piece, then run it) or
 * "app.put=PATH" (write it to the VM) or "app.install.upload=[ID[:ICON:COLOR]
 * [NAME]]" (install it). "app.install=PATH", or "app.install=ID[:ICON:COLOR]
 * PATH [NAME]" (ID "-" for the manifest's), installs one from the VM;
 * "app.run=ID" runs an installed app and "app.run=PATH" one from the VM (a
 * folder or a .wasm file); "app.remove=ID" and "app.list" do what the
 * commands do; "app.launcher" opens the launcher and "app.key" is the power
 * key, for bench tests; "app.stop" closes the app. "files.put=PATH [NAME]",
 * "files.list", "files.remove=NAME" and, after an upload, "files.upload=NAME"
 * handle the gadget's own files.
 * "app.status", "app.logs" and "app.api=PART" answer as the commands do;
 * app.api requires a part and returns no runtime discovery. "app.event=TYPE,A,B" hands the app an input event, as a touch
 * or press would, and "app.press=talk|second,down|up" presses or releases a
 * button as the board would, through the app view (long presses, the exit
 * hold), for bench tests. Each is answered with an "@app" line (a
 * trap later prints "@app.exit"). False for any other line. */
bool muse_apps_console(const char *line);

/* The gadget was unpaired: removes every installed app and file, so the next
 * owner's gadget doesn't carry the last one's. */
void muse_apps_forget_all(void);

/* The gadget is moving to another VM: the running app stops, so one VM's
 * app doesn't carry on with the next VM. */
void muse_apps_vm_changed(void);

#ifdef __cplusplus
}
#endif
