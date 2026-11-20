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
#include "sdkconfig.h"
#include "muse_theme_policy.h"
#if CONFIG_MUSE_UI_THEME
void muse_theme_start(void);
muse_theme_mode_t muse_theme_mode(void);
void muse_theme_request_mode(muse_theme_mode_t mode);
/* Content providers publish a stable background hint from any task. No
 * provider is required; Automatic falls back to the original dark scheme. */
void muse_theme_set_hint(bool valid,uint32_t rgb);
uint32_t muse_theme_epoch(void);
uint32_t muse_theme_hint(void);
int muse_theme_save_error(void);
#else
static inline void muse_theme_start(void) {}
static inline muse_theme_mode_t muse_theme_mode(void) {return MUSE_THEME_DARK;}
static inline void muse_theme_request_mode(muse_theme_mode_t mode) {(void)mode;}
static inline void muse_theme_set_hint(bool valid,uint32_t rgb) {(void)valid;(void)rgb;}
#endif
