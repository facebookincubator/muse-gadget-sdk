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
#include "muse_pixel_art.h"

typedef struct {
    muse_pixel_pack_t pack;
    uint32_t revision;
} muse_pixel_avatar_t;
/* Backend owns pack bytes. Every render-strip acquire pins one immutable
 * asset until its matching release, including across a concurrent update. */
const muse_pixel_avatar_t *muse_pixel_avatar_acquire(void);
void muse_pixel_avatar_release(const muse_pixel_avatar_t *model);
void muse_pixel_avatar_start(void);
/* Call periodically (at least once/second, independently of VM availability)
 * to retry a transient initial worker-allocation failure with bounded backoff. */
void muse_pixel_avatar_poll(void);
/* Call after a real Built-in/My Muse transition, including a round trip back
 * to Personal. Revokes prior acquisitions/uploads immediately; no file I/O. */
void muse_pixel_avatar_source_changed(void);
/* A non-NULL authoritative generation hides stale models immediately. NULL
 * still/source teardown does not erase known generation information. */
void muse_pixel_avatar_identity(const uint8_t source_key[32]);
/* Call at committed stored pairing/explicit endpoint/VM owner boundaries.
 * Snapshots begun before this fence cannot revalidate the old owner. Fresh
 * probes use stored ownership only and do not require cloud success. */
void muse_pixel_avatar_invalidate(void);
void muse_pixel_avatar_clear(void);
#if CONFIG_LV_USE_SNAPSHOT
/* Private validation flag suppresses initial autojoin as well as the keeper.
 * Only a snapshot build exposes the USB command; production builds omit it. */
bool muse_pixel_avatar_test_boot_offline(void);
#endif
bool muse_pixel_avatar_console(const char *line);
