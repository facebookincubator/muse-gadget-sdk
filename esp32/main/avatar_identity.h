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
#include <stdbool.h>
#include <stddef.h>
/* The VM's Gadget API: the identity, and the prefix of every avatar file URL
 * it hands out. Both are requested on ServiceType::Gadget. */
#define AVATAR_IDENTITY_PATH "/api/gadget/identity"
#define AVATAR_IDENTITY_FILE_PREFIX "/api/gadget/identity/avatar?"
/* path: the still's Gadget API URL; revision: the identity's updated_ms, which
 * changes whenever the identity or any avatar file does. */
typedef struct { char path[512]; char revision[128]; } avatar_identity_t;
/* 1: available; 0: explicitly absent; -1: invalid/unknown, retain last good. */
int avatar_identity_parse(const char *json, size_t len, avatar_identity_t *out);
/* True for an avatar file URL from the Gadget API that fits in cap bytes. */
bool avatar_identity_url_ok(const char *url, size_t cap);
struct cJSON;
/* A {"light": URL, "dark": URL} object's light URL, else its dark one; only a
 * URL avatar_identity_url_ok accepts. NULL when neither is. */
const char *avatar_identity_pick_url(const struct cJSON *urls, size_t cap);
