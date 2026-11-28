/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Used by temporary image downloads; callers own the result.
 * Encode VM paths as paths, never as URLs or query parameters. */
static inline char *vm_file_request_path(const char *path) {
    if (!path) return NULL;
    while (*path == '/') path++;
    size_t len = strlen(path), prefix = sizeof("/fs/raw/") - 1;
    if (len > (SIZE_MAX - prefix - 1) / 3) return NULL;
    char *out = malloc(prefix + len * 3 + 1);
    if (!out) return NULL;
    memcpy(out, "/fs/raw/", prefix);
    char *p = out + prefix;
    for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || strchr("-._~/", *c)) *p++ = (char)*c;
        else {
            *p++ = '%'; *p++ = "0123456789ABCDEF"[*c >> 4]; *p++ = "0123456789ABCDEF"[*c & 15];
        }
    }
    *p = 0;
    return out;
}
