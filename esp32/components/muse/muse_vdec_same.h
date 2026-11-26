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

/*
 * Which macroblocks of a tinyh264 picture are the previous picture's exactly
 * (muse_ui_fb.c then skips comparing them): P_Skip with a zero motion vector,
 * predicted from the picture the decoder output just before (its buffer), and
 * no deblocking edge that could touch it. A UI stream that changes a few
 * macroblocks codes the rest as such skips, so most of the comparison goes.
 */

#include <stddef.h>
#include <stdint.h>

#include "h264bsd_storage.h"

/*
 * Fills map[mb] = 1 for each macroblock of the picture just decoded that is
 * the previous output picture's (prev: its luma data, as h264bsdDecode
 * returned it), 0 otherwise. Returns how many, or -1 when the map can't be
 * trusted (no previous picture, concealed macroblocks, no room in map).
 */
int muse_h264_same_map(const storage_t *s, const uint8_t *prev, uint8_t *map, size_t cap);
