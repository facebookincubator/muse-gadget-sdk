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
 * How an app looks in the launcher (muse_apps_icons.c): a glyph from the
 * firmware's set, white on a disc of one of its colours. Muse picks both when
 * it installs an app; an app without them shows its initial on a colour its
 * id picks. Plain C, so the host tests run it.
 */

#include <stdbool.h>
#include <stdint.h>

#define MUSE_APPS_LOOK_MAX 12  /* the longest glyph or colour name, and its NUL */

/* A glyph is a list of strokes on a 24 x 24 grid, coordinates in tenths,
 * drawn 2 units wide with round ends:
 *   G_LINE n x0 y0 ... (n points)    G_RING cx cy r      G_DOT cx cy r (filled)
 *   G_ARC cx cy r a0 a1 (degrees clockwise from 12, a1 > a0)
 *   G_BOX x y w h radius (outlined)  G_BLOCK x y w h radius (filled)  G_END */
enum { G_END, G_LINE, G_RING, G_ARC, G_DOT, G_BOX, G_BLOCK };

typedef struct {
    const char *name;
    const int16_t *strokes;
} muse_apps_glyph_t;

typedef struct {
    const char *name;
    uint32_t disc, ink;  /* 0xRRGGBB: the disc, and the glyph on it */
} muse_apps_color_t;

extern const muse_apps_glyph_t muse_apps_glyphs[];
extern const int muse_apps_glyph_count;
extern const muse_apps_color_t muse_apps_colors[];
extern const int muse_apps_color_count;

/* Index by name, or -1. */
int muse_apps_glyph_find(const char *name);
int muse_apps_color_find(const char *name);
/* The colour an app without one gets, from its id. */
int muse_apps_color_for_id(const char *id);
/* "timer, clock, ..." for app.install's description. */
void muse_apps_glyph_names(char *out, int cap);
void muse_apps_color_names(char *out, int cap);
