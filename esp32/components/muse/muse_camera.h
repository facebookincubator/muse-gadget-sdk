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

#include "cJSON.h"
#include "esp_err.h"

/*
 * The board's AI camera, which speaks Seeed's SSCMA AT protocol
 * (muse_board->sscma: the Watcher's Himax). A request "AT+<cmd>" gets one
 * reply; a SAMPLE or INVOKE also streams events, each a frame's image and
 * results. Replies and events are "\r{json}\n" lines:
 *   {"type": 0 reply | 1 event | 2 log, "name", "code", "data"}
 * The camera is powered only between muse_camera_open() and _close().
 */

bool muse_camera_present(void);

/* Powers the camera up and waits until it answers (a second or two). Opens
 * nest; the last close powers it down. */
esp_err_t muse_camera_open(void);
void muse_camera_close(void);

/* Sends "AT+<cmd>" and waits for its reply. *data (may be NULL) gets the
 * reply's "data", for the caller to cJSON_Delete; *code (may be NULL) its code
 * (0 ok). ESP_ERR_TIMEOUT if none came, ESP_FAIL on a nonzero code. One
 * request at a time; others wait their turn. */
esp_err_t muse_camera_request(const char *cmd, cJSON **data, int *code, int timeout_ms);

/* Waits for the next event named `name` ("SAMPLE", "INVOKE"). Only the
 * newest two are kept, so a stream can't pile up. *data as above. */
esp_err_t muse_camera_event(const char *name, cJSON **data, int timeout_ms);

/* Drops the events waiting, before starting a new SAMPLE or INVOKE. */
void muse_camera_flush_events(void);
