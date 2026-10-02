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

/* Compile-time appearance hooks. Defaults preserve the original renderer.
 * ACCESSORY: 0 = none, 1 = scarf, 2 = bow. No runtime allocation or stored sprites.
 * The local customizer emits overrides at the top of a standalone muse_pixel.c.
 */

#ifndef MUSE_STYLE_OUT
#define MUSE_STYLE_OUT 0x3a2b22
#endif

#ifndef MUSE_STYLE_OUT2
#define MUSE_STYLE_OUT2 0x8c7560
#endif

#ifndef MUSE_STYLE_FUR_D
#define MUSE_STYLE_FUR_D 0xae987e
#endif

#ifndef MUSE_STYLE_FUR
#define MUSE_STYLE_FUR 0xcfbc9f
#endif

#ifndef MUSE_STYLE_FUR_L
#define MUSE_STYLE_FUR_L 0xe6d7bd
#endif

#ifndef MUSE_STYLE_FUR_H
#define MUSE_STYLE_FUR_H 0xf8eedc
#endif

#ifndef MUSE_STYLE_FACE_D
#define MUSE_STYLE_FACE_D 0xe9cba4
#endif

#ifndef MUSE_STYLE_FACE
#define MUSE_STYLE_FACE 0xf6dfbd
#endif

#ifndef MUSE_STYLE_FACE_L
#define MUSE_STYLE_FACE_L 0xfdeed6
#endif

#ifndef MUSE_STYLE_EYES
#define MUSE_STYLE_EYES 0x120d0b
#endif

#ifndef MUSE_STYLE_BLUSH
#define MUSE_STYLE_BLUSH 0xf4aaa0
#endif

#ifndef MUSE_STYLE_BLUSH_D
#define MUSE_STYLE_BLUSH_D 0xea8f8e
#endif

#ifndef MUSE_STYLE_WIDTH
#define MUSE_STYLE_WIDTH 1.0f
#endif

#ifndef MUSE_STYLE_HEIGHT
#define MUSE_STYLE_HEIGHT 1.0f
#endif

#ifndef MUSE_STYLE_FACE_X
#define MUSE_STYLE_FACE_X 0.0f
#endif

#ifndef MUSE_STYLE_FACE_Y
#define MUSE_STYLE_FACE_Y 0.0f
#endif

#ifndef MUSE_STYLE_ACCESSORY
#define MUSE_STYLE_ACCESSORY 0
#endif

#ifndef MUSE_STYLE_CLOTH
#define MUSE_STYLE_CLOTH 0x5cb8ff
#endif

#ifndef MUSE_STYLE_CLOTH_D
#define MUSE_STYLE_CLOTH_D 0x2a5bd7
#endif
