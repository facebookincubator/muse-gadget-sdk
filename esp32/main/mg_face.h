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

/*
 * The avatar face (the face.jpg route): the user's dark Muse avatar as the home's
 * background, read by the gadget itself over its own session to Muse (the
 * BLE device session or Link's Wi-Fi session alike, noise_ctrl_req_*). No
 * app code, no new mgcommands.
 *
 * - The gadget only reads workspace/devices/<node_id>/face.jpg; whoever puts
 *   a picture there calls face.set. It never asks Muse to make one.
 * - The gadget looks once at boot, at the face's selection, at setup (its
 *   restart) and on face.set, never per open or on a timer; a failed look
 *   again 30 s doubling to 5 min, at the session's next open (mg_face_core.h,
 *   face_sched_t). Each look is GET Range bytes=0-0 for the size and ETag, then
 *   ranges within the session's frame bound with If-Range. Unchanged: nothing
 *   read.
 * - Only a baseline JPEG that decodes whole is stored (NVS namespace
 *   CONFIG_STORE_FACE_NS, one record), the old one kept until then; PNG and
 *   WebP are refused. The first picture stored picks the avatar face once;
 *   a face the user picked is never changed. A damaged record is erased once
 *   the boot is healthy (recovery tiers and SAFE never read it).
 * - The Muse's name (GET /identity) labels the face, "My Muse" until known.
 * - Privacy: nothing of the picture or the name is ever sent or logged (the
 *   slot, size and an ETag tag only). Both are erased with the pairing:
 *   Forget, Reset pairing, every Link setup, a setup reset, a factory reset
 *   (config_store.c's clears and app.c's passcode_forget()).
 *
 * - Size (Settings > Watch face): Large (the default: the
 *   picture fills the panel) or Small (0.70x, centered across, its ear row
 *   at the reference height below the clock, black around it), NVS key "size"
 *   beside the pick, erased with the rest; the picture is scaled once when
 *   it's stored, the Size changes or the gadget boots (PSRAM), never per
 *   frame.
 *
 * - Memory: no internal RAM while idle or in a call:
 *   the state, the semaphores, the picture and every buffer in PSRAM; the
 *   worker task (an internal stack: NVS writes) only while there's work,
 *   started by mg_face_tick() and deleted once nothing is due, never during
 *   a call or a video stream.
 *
 * Console: face? (state, internal RAM and the worker's last run),
 * face.check, face.pick=avatar|pixel, face.size=large|small,
 * face.erase.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

#if CONFIG_MUSE_AVATAR_FACE
/* At boot, on the app's task (it reads NVS). Starts no task: the stored
 * picture is read once the boot is healthy, by the worker mg_face_tick()
 * starts. */
void mg_face_start(void);
/* The app's loop, every second or few (the BLE-only loop's 1 s, the Wi-Fi
 * heartbeat's 5 s): notes a session (re)open and starts the worker (an
 * internal stack, only while there's work; never during a call or a video
 * stream) when the boot's check, a look, a store or a Pick or Size change is
 * due. Never blocks. */
void mg_face_tick(void);
/* The pairing went: the picture, the name and the pick are erased now. */
bool mg_face_forget(const char *why);   /* false: the record's erase failed (config_erase_face) */
/* face.set {path, byte_len, etag?}: an asynchronous result once read. */
cJSON *mg_face_command(cJSON *params, const char *request_id, noise_ctrl_session_generation_t generation);
/* workspace/devices/<node_id>/face.jpg, for the command's description. */
const char *mg_face_own_path(void);
/* The largest JPEG this gadget takes, for the command's description. */
uint32_t mg_face_max_bytes(void);
/* The face picker: the avatar face (true) or Pixel Muse. */
void mg_face_pick(bool avatar);
bool mg_face_avatar_picked(void);
/* The avatar face's Size: Small (true) or Large. */
bool mg_face_size_small(void);
void mg_face_size_set(bool small);
/* The avatar face's label: the Muse's name, or "My Muse". */
void mg_face_label(char *out, size_t cap);
/* face?, face.check, face.pick=, face.size=, face.erase. */
bool mg_face_console(const char *line);
#else
static inline void mg_face_start(void) {}
static inline void mg_face_tick(void) {}
static inline bool mg_face_forget(const char *why) { (void)why; return true; }
#endif

#ifdef __cplusplus
}
#endif
