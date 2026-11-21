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

#include <stddef.h>

/*
 * Muse account API: finds the VM a device token
 * can reach and the per-VM credential for its Noise connection. Ported from
 * Link's main/vm_api.c.
 */

#define MUSE_HATCH_API_AUTH -2     /* token rejected (401/403) */
#define MUSE_HATCH_API_FAILED -1

typedef struct {
    char vm_id[128];
    char vm_name[64];
    char *vm_token;       /* heap; free with free() */
} muse_hatch_vm_t;

/*
 * Picks the VM whose id is `want_vm` (empty: the account's default VM).
 * Returns 0, MUSE_HATCH_API_AUTH or MUSE_HATCH_API_FAILED.
 */
int muse_hatch_api_find_vm(const char *device_token, const char *want_vm, muse_hatch_vm_t *out);
