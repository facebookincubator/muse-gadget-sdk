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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes a whole baseline JPEG into out (w x h RGB565, native order),
 * placed by face_place(). NULL on success (with the JPEG's own size and the
 * scale), else why it was refused (PNG and WebP are, by name). */
const char *mg_face_jpeg_decode(const uint8_t *jpeg, size_t len, uint16_t *out, int w, int h,
                                int *img_w, int *img_h, int *scale);

#ifdef __cplusplus
}
#endif
