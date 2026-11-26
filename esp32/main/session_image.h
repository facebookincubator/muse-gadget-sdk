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
 * A use case taught to Muse through the device session alone
 * (CONFIG_MUSE_SESSION_IMAGE): the gadget registers display.show_image on
 * its own link.register, and its test (Settings > Developer > Tests > Ask
 * Muse for an image; the console's img.ask) asks Muse in plain words, on
 * the gadget's own session (POST /chat/stream), for a picture. Muse makes
 * one, saves it on the VM and calls display.show_image with its path; the
 * gadget reads it (GET /fs/raw, in ranges), shows it for duration_ms over
 * everything but the mic ring, then goes back. No phone app or server code
 * knows about any of it: the app relays the session's bytes.
 *
 * Logs on mg.image (mirrored on the Nordic UART): the press, the request's
 * answer, the command's arrival, the download and the hide, never the
 * request's words or the picture.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "noise_control.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_SESSION_IMAGE

/* display.show_image, from on_ws_command() (the session task): starts the
 * download and returns {"_async": true}, or an error result. */
cJSON *session_image_command(cJSON *params, const char *request_id,
                             noise_ctrl_session_generation_t session_generation);

/* The test's press, from any task (the Tests page's button, img.ask): asks
 * Muse for a picture. False with the reason when it can't. */
bool session_image_ask(char *why, size_t why_cap);

/* The Tests page's line: what the run is doing, or how it went. True
 * while a run is under way (asking, downloading or showing): the page
 * stays open (no 30 s idle close) and the panel lit. */
bool session_image_status(char *out, size_t cap);

/* The console's words, on the USB console and the Nordic UART's !IN:
 * img.ask (the test's press), img.show=PATH[,MS] (a file on the VM shown as
 * display.show_image would, without Muse) and img? (the last run's marks).
 * True if the line was one of them. */
bool session_image_console(const char *line);

#endif

#ifdef __cplusplus
}
#endif
