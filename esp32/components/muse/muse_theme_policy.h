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
#include <stdint.h>
typedef enum { MUSE_THEME_AUTO, MUSE_THEME_DARK, MUSE_THEME_LIGHT } muse_theme_mode_t;
typedef struct { bool light; uint32_t background; bool original_dark; } muse_theme_palette_t;
muse_theme_palette_t muse_theme_palette(muse_theme_mode_t mode, bool hint_valid, uint32_t hint);
/* Legacy RGB is a stable semantic key. Dark without a hint is byte-for-byte
 * the existing palette. Only UI style colors pass through this function. */
uint32_t muse_theme_map(const muse_theme_palette_t *palette,uint32_t legacy,bool foreground);
uint32_t muse_theme_text_on(const muse_theme_palette_t *palette,uint32_t legacy,uint32_t surface);
float muse_theme_contrast(uint32_t a,uint32_t b);
uint16_t muse_theme_rgb565(uint32_t rgb);
