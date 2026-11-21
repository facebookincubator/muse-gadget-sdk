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
#include "muse_avatar_theme.h"
#include "muse_avatar.h"
#include "muse_avatar_background.h"
#include "muse_theme.h"
static uint32_t s_revision,s_background,s_content_epoch;
static bool s_seen,s_valid;
void muse_avatar_theme_tick(void) {
    uint32_t epoch=muse_avatar_content_epoch();
    if(epoch!=s_content_epoch) {
        s_content_epoch=epoch;s_valid=false;s_seen=false;
    }
    if(!epoch || muse_avatar_source()!=MUSE_AVATAR_PERSONAL) {
        muse_theme_set_hint(false,0);return;
    }
    const muse_avatar_image_t *image=muse_avatar_acquire();
    if(image && (!s_seen || image->revision!=s_revision)) {
        s_valid=muse_avatar_background_estimate(&image->data,&s_background,NULL);
        s_revision=image->revision;s_seen=true;
    }
    muse_avatar_release(image);
    if(muse_avatar_content_epoch()!=epoch)s_valid=false;
    /* Retain this source's last hint through asynchronous reloads, but never
     * lend it to Built-in or carry it across a content/account invalidation. */
    muse_theme_set_hint(s_valid,s_background);
}
