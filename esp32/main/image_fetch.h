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
#include <stddef.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    const char *code;     // error code when !ok, e.g. "out_of_memory"
    const char *message;  // error detail when !ok
    const char *format;   // "jpeg" or "rgb565"
    int width, height;    // size drawn, after any JPEG scaling
    int scale;            // JPEG downscale: 1, 2, 4 or 8
    size_t bytes;         // bytes downloaded
    int ms;               // download and draw time
} image_fetch_result_t;

typedef void (*image_fetch_done_cb)(const image_fetch_result_t *result, void *user);

// `row` for a JPEG centred down the screen, on boards with the full UI only.
#define IMAGE_FETCH_CENTRE (-1)

// `row` when the command gives none: boards with the full UI centre a JPEG, and the display
// backends draw from the top.
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE
#define IMAGE_FETCH_DEFAULT_ROW IMAGE_FETCH_CENTRE
#else
#define IMAGE_FETCH_DEFAULT_ROW 0
#endif

// Download an image over HTTP(S) and draw it as it arrives, starting at
// `row`, with the status on top. Takes a baseline JPEG, shrunk 1/2, 1/4 or
// 1/8 as needed to fit and centred across, or raw RGB565 (high byte first)
// in whole screen-width rows. On boards with the full UI, `row` may also be
// IMAGE_FETCH_CENTRE, which centres a JPEG down the screen (raw data starts at
// the top). E-paper shows it once it is all in. One download runs at a time,
// on its own task, and only when there is memory to spare.
//
// Returns false with `code`/`message` set if the download cannot start
// (busy, low memory, no display); otherwise `done` runs later on the
// download task.
bool image_fetch_start(const char *url, int row, image_fetch_done_cb done,
                       void *user, const char **code, const char **message);

// The same for a file on the VM, fetched with GET /fs/raw/<path> over the
// Noise session rather than from the internet, in Range requests small enough
// that no reply overflows the session's buffers. Each range is drawn before
// the next is asked for. Shares the one-download limit with
// image_fetch_start.
bool image_fetch_start_vm(const char *path, int row, image_fetch_done_cb done,
                          void *user, const char **code, const char **message);

#ifdef __cplusplus
}
#endif
