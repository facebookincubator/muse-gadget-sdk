/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "sdkconfig.h"
#include "muse_avatar_render.h"

/* Backend owns immutable pixels. Every acquire, including a decoder's open,
 * must be paired with release after the last strip read. */
typedef struct muse_avatar_image {
    muse_avatar_pixels_t data;
    uint32_t revision;
#if CONFIG_MUSE_AVATAR_ANIMATIONS
    void (*release)(const struct muse_avatar_image *image);
#endif
} muse_avatar_image_t;

void muse_avatar_start(void);
muse_avatar_source_t muse_avatar_source(void);
void muse_avatar_select(muse_avatar_source_t source);
void muse_avatar_refresh(void);
void muse_avatar_clear(void);
const char *muse_avatar_status(void);
const muse_avatar_image_t *muse_avatar_acquire(void);
void muse_avatar_release(const muse_avatar_image_t *image);
#if CONFIG_MUSE_PIXEL_AVATAR
/* Read metadata from the same pinned canonical image, never a newer global
 * identity. The caller keeps the image acquired until copying finishes. */
bool muse_avatar_copy_image_key(const muse_avatar_image_t *image,uint8_t out[32]);
#endif
void muse_avatar_pairing_changed(void);
/* Changes when saved content is cleared or its account/VM binding changes.
 * Source selection alone does not invalidate content-derived metadata. Zero
 * means no image from the current context has been published yet. */
uint32_t muse_avatar_content_epoch(void);
/* Also used by Apps when avatar support is compiled in: one shared mount. */
bool muse_avatar_files_mount(void);

#if CONFIG_MUSE_AVATAR_ANIMATIONS || CONFIG_MUSE_PIXEL_AVATAR
bool muse_avatar_files_space(size_t needed);
#endif
#if CONFIG_MUSE_AVATAR_ANIMATIONS
/* Pairing changes hide all personal surfaces immediately, before worker cleanup. */
bool muse_avatar_personal_allowed(void);
#endif
