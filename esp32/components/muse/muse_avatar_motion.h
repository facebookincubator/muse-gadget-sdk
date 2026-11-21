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
#include "muse_avatar.h"
#include "muse_pixel.h"
#include "muse_avatar_pack.h"
/* UI asks for a clip each visible home frame. Missing rigs keep the personal
 * frame; pet/speech feedback is applied to it by the UI, never another avatar. */
void muse_avatar_motion_tick(const muse_pose_t *pose);
const muse_avatar_image_t *muse_avatar_motion_acquire(void);
void muse_avatar_motion_bind(const uint8_t source[32],const uint8_t binding[32]);
void muse_avatar_motion_enable(bool enabled);
void muse_avatar_motion_status(void);
void muse_avatar_motion_seen(uint32_t revision);
bool muse_avatar_motion_preview(unsigned clip);
/* Console install is serialized by the console owner. At most 1 MiB staging
 * in PSRAM; storage admission and atomic rename preserve the previous pack. */
bool muse_avatar_motion_install_begin(size_t size);
bool muse_avatar_motion_install_chunk(size_t offset,const uint8_t *data,size_t size);
bool muse_avatar_motion_install_commit(void);
void muse_avatar_motion_install_abort(void);
void muse_avatar_motion_clear(void);
