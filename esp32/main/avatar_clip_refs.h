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
#include "avatar_identity.h"
#include "muse_avatar_pack.h"
typedef struct { avatar_identity_t identity; char path[MUSE_AVATAR_CLIPS][512]; } avatar_clip_refs_t;
/* Bounded GET /api/gadget/identity parsing; only the documented animation
 * fields are used. */
bool avatar_clip_refs_parse(const char *json, size_t len, avatar_clip_refs_t *out);
