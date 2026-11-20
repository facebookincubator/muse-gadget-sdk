/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Double-click to open live preview, then tap the shutter or double-click again. */
void watcher_camera_preview_toggle(void);
bool watcher_camera_preview_active(void);
/* Closes the live view now, from the caller's task, and clears its frame
 * from the screen; a photo note's photo was its frame. */
void watcher_camera_preview_close(void);
/* Captures one frame for the Home Link camera.capture command. */
bool watcher_camera_capture(char **jpeg_base64, const char **error);
esp_err_t watcher_camera_prepare(void);

#ifdef __cplusplus
}
#endif
