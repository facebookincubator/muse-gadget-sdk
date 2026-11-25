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
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// The servers the device sends its account and VM credentials to. Pairing
// names them (provision_v2's api_url, api_url_v2 and noise_host) and so does
// the Muse host setting, so a release build takes only the ones the Muse app
// sends, exactly as it sends them: other names under the same domains aren't
// Muse's API or its Noise ingress. CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS lifts
// the check for development. A missing or empty value means the default and
// always passes. Link (main/) and the Muse component both use this.

static inline bool muse_server_listed(const char *value, const char *const *list,
                                      size_t count) {
    if (!value || !*value) return true;
#if defined(CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS) && CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS
    (void)list;
    (void)count;
    return true;
#else
    for (size_t i = 0; i < count; i++) {
        if (strcmp(value, list[i]) == 0) return true;
    }
    return false;
#endif
}

// An API base: the origin alone, since API paths are appended to it.
static inline bool muse_server_api_allowed(const char *url) {
    static const char *const allowed[] = {
        "https://api.muse.ai",        // api_url_v2, and Link's default
        "https://hatch-api.meta.ai",  // api_url, which only older firmware reads
    };
    return muse_server_listed(url, allowed, sizeof(allowed) / sizeof(allowed[0]));
}

// The Noise host: a bare hostname, dialled on port 443.
static inline bool muse_server_noise_allowed(const char *host) {
    static const char *const allowed[] = {"hatch.metaaivm.com"};
    return muse_server_listed(host, allowed, sizeof(allowed) / sizeof(allowed[0]));
}
