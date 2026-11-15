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

#include <stdint.h>

#include "noise_control.h"

#define BUG_REPORT_REQUEST_ID_MAX_BYTES 63
// The server drops larger bug reports. A full log tail (~22 KB base64)
// plus a full connectivity history (~15-30 KB) can exceed it, so the report
// drops the oldest connectivity events until it fits.
#define BUG_REPORT_MAX_BYTES (32 * 1024)

typedef enum {
    BUG_REPORT_START_ACCEPTED = 0,
    BUG_REPORT_START_INVALID_REQUEST_ID,
    BUG_REPORT_START_INVALID_SESSION,
    BUG_REPORT_START_BUSY,
    BUG_REPORT_START_OUT_OF_MEMORY,
    BUG_REPORT_START_TASK_UNAVAILABLE,
} bug_report_start_status_t;

// Starts one asynchronous diagnostic snapshot. The request ID is an opaque
// Remote-control correlation key; user-entered report metadata stays outside the
// firmware artifact.
bug_report_start_status_t bug_report_start(
    const char *source_request_id,
    noise_ctrl_session_generation_t session_generation);
