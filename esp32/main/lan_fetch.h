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

// lan.fetch: GET a URL on the gadget's home network (a camera snapshot, say)
// and write the body to a file on the user's VM over POST /fs/write, so Muse
// can read what its VM can't reach. CONFIG_HOMEHUB_LAN_FETCH.

#include <stdbool.h>

#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parameters, all checked here:
//   url         http:// or https://, optionally with user:password@; the host
//               must be (or resolve to) a private IPv4 address
//   username,   a login for Basic or Digest, in place of the URL's
//   password
//   path        the file on the VM, relative to the user's home
//               (default lan-fetch/latest.<ext>, ext from Content-Type)
//   max_bytes   1 KiB..2 MiB, default 1 MiB
//   timeout_ms  for the download, 1000..60000, default 15000
//   delivery    "file": POST /fs/write only; "inline": base64 in the result
//               (up to 512 KB); "auto" (default): the file, or inline with
//               file_error once the VM refuses /fs/write to this gadget's
//               credentials (retried 10 minutes after a refusal)
// Returns {"_async": true} and later sends the result on the session, or an
// error result at once. Never logs or returns the password.
cJSON *lan_fetch_command(cJSON *params, const char *request_id,
                         noise_ctrl_session_generation_t session_generation);

// The USB console's ">lan.fetch {json}": the same command with the same
// parameters, its result printed as "@lan.fetch {...}" instead of sent to
// Muse. False if the line is something else.
bool lan_fetch_console(const char *line);

#ifdef __cplusplus
}
#endif
