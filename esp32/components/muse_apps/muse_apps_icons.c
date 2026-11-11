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


/*
 * The launcher's glyphs and colours. The glyphs are drawn for this firmware,
 * as strokes on a 24 x 24 grid (muse_apps_icons.h); the colours are bright
 * enough to tell apart at a glance and dark enough for a white glyph, but for
 * yellow, which takes a dark one.
 */

#include "muse_apps_icons.h"

#include <stdio.h>
#include <string.h>

static const int16_t g_timer[] = {G_RING, 120, 135, 80, G_LINE, 2, 120, 135, 120, 95, G_LINE, 2, 100, 25, 140, 25, G_LINE, 2, 120, 25, 120, 55, G_LINE, 2, 182, 68, 196, 54, G_END};
static const int16_t g_clock[] = {G_RING, 120, 120, 90, G_LINE, 3, 120, 70, 120, 120, 155, 140, G_END};
static const int16_t g_calendar[] = {G_BOX, 30, 50, 180, 160, 30, G_LINE, 2, 30, 100, 210, 100, G_LINE, 2, 80, 30, 80, 70, G_LINE, 2, 160, 30, 160, 70, G_DOT, 80, 150, 13, G_DOT, 120, 150, 13, G_DOT, 160, 150, 13, G_END};
static const int16_t g_check[] = {G_LINE, 3, 45, 125, 95, 175, 195, 65, G_END};
static const int16_t g_list[] = {G_DOT, 45, 60, 14, G_DOT, 45, 120, 14, G_DOT, 45, 180, 14, G_LINE, 2, 90, 60, 200, 60, G_LINE, 2, 90, 120, 200, 120, G_LINE, 2, 90, 180, 200, 180, G_END};
static const int16_t g_note[] = {G_LINE, 6, 40, 200, 50, 155, 160, 45, 195, 80, 85, 190, 40, 200, G_LINE, 2, 135, 70, 170, 105, G_END};
static const int16_t g_chat[] = {G_BOX, 30, 40, 180, 130, 40, G_LINE, 3, 80, 160, 70, 210, 130, 160, G_END};
static const int16_t g_mic[] = {G_BOX, 90, 25, 60, 115, 30, G_ARC, 120, 110, 65, 90, 270, G_LINE, 2, 120, 175, 120, 210, G_LINE, 2, 90, 210, 150, 210, G_END};
static const int16_t g_music[] = {G_LINE, 4, 90, 180, 90, 55, 190, 35, 190, 160, G_DOT, 65, 180, 26, G_DOT, 165, 160, 26, G_END};
static const int16_t g_heart[] = {G_ARC, 80, 90, 40, 225, 450, G_ARC, 160, 90, 40, 270, 495, G_LINE, 3, 52, 118, 120, 187, 188, 118, G_END};
static const int16_t g_pulse[] = {G_LINE, 6, 20, 120, 60, 120, 90, 50, 145, 190, 175, 120, 220, 120, G_END};
static const int16_t g_moon[] = {G_ARC, 120, 120, 90, 105, 357, G_ARC, 175, 75, 75, 155, 307, G_END};
static const int16_t g_sun[] = {G_RING, 120, 120, 40, G_LINE, 2, 120, 50, 120, 25, G_LINE, 2, 169, 71, 187, 53, G_LINE, 2, 190, 120, 215, 120, G_LINE, 2, 169, 169, 187, 187, G_LINE, 2, 120, 190, 120, 215, G_LINE, 2, 71, 169, 53, 187, G_LINE, 2, 50, 120, 25, 120, G_LINE, 2, 71, 71, 53, 53, G_END};
static const int16_t g_cloud[] = {G_ARC, 75, 150, 40, 180, 360, G_ARC, 130, 115, 55, 275, 458, G_ARC, 170, 155, 35, 24, 180, G_LINE, 2, 75, 190, 170, 190, G_END};
static const int16_t g_drop[] = {G_ARC, 120, 145, 65, 57, 303, G_LINE, 3, 175, 110, 120, 25, 65, 110, G_END};
static const int16_t g_leaf[] = {G_ARC, 190, 190, 140, 270, 360, G_ARC, 50, 50, 140, 90, 180, G_LINE, 2, 35, 205, 130, 110, G_END};
static const int16_t g_bolt[] = {G_LINE, 7, 130, 25, 45, 135, 120, 135, 110, 215, 195, 105, 120, 105, 130, 25, G_END};
static const int16_t g_star[] = {G_LINE, 11, 120, 28, 145, 93, 215, 97, 161, 141, 179, 209, 120, 171, 61, 209, 79, 141, 25, 97, 95, 93, 120, 28, G_END};
static const int16_t g_spark[] = {G_LINE, 9, 120, 25, 138, 102, 215, 120, 138, 138, 120, 215, 102, 138, 25, 120, 102, 102, 120, 25, G_END};
static const int16_t g_smile[] = {G_RING, 120, 120, 90, G_ARC, 120, 120, 50, 115, 245, G_DOT, 90, 95, 13, G_DOT, 150, 95, 13, G_END};
static const int16_t g_game[] = {G_BOX, 20, 65, 200, 110, 55, G_LINE, 2, 55, 120, 95, 120, G_LINE, 2, 75, 100, 75, 140, G_DOT, 150, 110, 13, G_DOT, 180, 130, 13, G_END};
static const int16_t g_dice[] = {G_BOX, 35, 35, 170, 170, 40, G_DOT, 80, 80, 14, G_DOT, 160, 80, 14, G_DOT, 120, 120, 14, G_DOT, 80, 160, 14, G_DOT, 160, 160, 14, G_END};
static const int16_t g_target[] = {G_RING, 120, 120, 90, G_RING, 120, 120, 50, G_DOT, 120, 120, 16, G_END};
static const int16_t g_bricks[] = {G_BLOCK, 25, 35, 55, 32, 10, G_BLOCK, 92, 35, 55, 32, 10, G_BLOCK, 160, 35, 55, 32, 10, G_BLOCK, 25, 85, 55, 32, 10, G_BLOCK, 160, 85, 55, 32, 10, G_DOT, 115, 145, 18, G_LINE, 2, 80, 200, 160, 200, G_END};
static const int16_t g_book[] = {G_LINE, 2, 120, 70, 120, 200, G_LINE, 6, 120, 70, 90, 52, 30, 45, 30, 180, 90, 185, 120, 200, G_LINE, 6, 120, 70, 150, 52, 210, 45, 210, 180, 150, 185, 120, 200, G_END};
static const int16_t g_chart[] = {G_LINE, 2, 50, 200, 50, 140, G_LINE, 2, 100, 200, 100, 90, G_LINE, 2, 150, 200, 150, 120, G_LINE, 2, 200, 200, 200, 50, G_END};
static const int16_t g_compass[] = {G_RING, 120, 120, 90, G_LINE, 5, 155, 85, 134, 134, 85, 155, 106, 106, 155, 85, G_END};
static const int16_t g_globe[] = {G_RING, 120, 120, 90, G_LINE, 2, 30, 120, 210, 120, G_ARC, 201, 120, 121, 222, 318, G_ARC, 39, 120, 121, 42, 138, G_END};
static const int16_t g_cup[] = {G_LINE, 2, 40, 80, 180, 80, G_LINE, 2, 50, 80, 50, 150, G_LINE, 2, 170, 80, 170, 150, G_ARC, 90, 150, 40, 180, 270, G_ARC, 130, 150, 40, 90, 180, G_LINE, 2, 90, 190, 130, 190, G_ARC, 180, 125, 28, 0, 180, G_LINE, 2, 90, 25, 90, 45, G_LINE, 2, 130, 25, 130, 45, G_END};
static const int16_t g_bell[] = {G_ARC, 120, 100, 60, 270, 450, G_LINE, 2, 60, 100, 52, 165, G_LINE, 2, 180, 100, 188, 165, G_LINE, 2, 35, 170, 205, 170, G_ARC, 120, 190, 22, 90, 270, G_LINE, 2, 120, 25, 120, 40, G_END};
static const int16_t g_home[] = {G_LINE, 3, 30, 110, 120, 35, 210, 110, G_LINE, 4, 55, 90, 55, 200, 185, 200, 185, 90, G_LINE, 4, 100, 200, 100, 145, 140, 145, 140, 200, G_END};
static const int16_t g_pin[] = {G_ARC, 120, 95, 65, 237, 483, G_LINE, 3, 65, 130, 120, 215, 175, 130, G_RING, 120, 95, 23, G_END};
static const int16_t g_camera[] = {G_BOX, 25, 70, 190, 130, 30, G_RING, 120, 135, 36, G_LINE, 4, 80, 70, 95, 40, 145, 40, 160, 70, G_END};
static const int16_t g_flag[] = {G_LINE, 2, 50, 210, 50, 30, G_LINE, 5, 50, 40, 190, 40, 155, 85, 190, 130, 50, 130, G_END};
static const int16_t g_wind[] = {G_LINE, 2, 30, 80, 150, 80, G_ARC, 150, 50, 30, 300, 540, G_LINE, 2, 30, 160, 180, 160, G_ARC, 180, 190, 30, 0, 240, G_LINE, 2, 30, 120, 100, 120, G_END};

#define GLYPH(n) {#n, g_##n}
const muse_apps_glyph_t muse_apps_glyphs[] = {
    GLYPH(timer), GLYPH(clock), GLYPH(calendar), GLYPH(check), GLYPH(list),   GLYPH(note),   GLYPH(chat),
    GLYPH(mic),   GLYPH(music), GLYPH(heart),    GLYPH(pulse), GLYPH(moon),   GLYPH(sun),    GLYPH(cloud),
    GLYPH(drop),  GLYPH(leaf),  GLYPH(bolt),     GLYPH(star),  GLYPH(spark),  GLYPH(smile),  GLYPH(game),
    GLYPH(dice),  GLYPH(target), GLYPH(bricks),  GLYPH(book),  GLYPH(chart),  GLYPH(compass), GLYPH(globe),
    GLYPH(cup),   GLYPH(bell),  GLYPH(home),     GLYPH(pin),   GLYPH(camera), GLYPH(flag),   GLYPH(wind),
};
const int muse_apps_glyph_count = sizeof(muse_apps_glyphs) / sizeof(muse_apps_glyphs[0]);

const muse_apps_color_t muse_apps_colors[] = {
    {"red", 0xE5484D, 0xFFFFFF},    {"orange", 0xF2873A, 0xFFFFFF}, {"yellow", 0xF0B429, 0x1C1C1E},
    {"green", 0x34B865, 0xFFFFFF},  {"mint", 0x3CC7B0, 0xFFFFFF},   {"teal", 0x2A9BB8, 0xFFFFFF},
    {"blue", 0x3478F6, 0xFFFFFF},   {"indigo", 0x5856D6, 0xFFFFFF}, {"purple", 0xA05AE0, 0xFFFFFF},
    {"pink", 0xE84A7F, 0xFFFFFF},   {"brown", 0xA2845E, 0xFFFFFF},  {"gray", 0x6E6E76, 0xFFFFFF},
};
const int muse_apps_color_count = sizeof(muse_apps_colors) / sizeof(muse_apps_colors[0]);

int muse_apps_glyph_find(const char *name) {
    for (int i = 0; name && i < muse_apps_glyph_count; i++) {
        if (!strcmp(muse_apps_glyphs[i].name, name)) return i;
    }
    return -1;
}

int muse_apps_color_find(const char *name) {
    for (int i = 0; name && i < muse_apps_color_count; i++) {
        if (!strcmp(muse_apps_colors[i].name, name)) return i;
    }
    return -1;
}

int muse_apps_color_for_id(const char *id) {
    uint32_t h = 2166136261u;
    for (const char *c = id; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return (int)(h % (uint32_t)(muse_apps_color_count - 1));  // any but gray, the last
}

static void names(char *out, int cap, const char *(*name)(int), int n) {
    int used = 0;
    out[0] = '\0';
    for (int i = 0; i < n && used < cap; i++) used += snprintf(out + used, cap - used, "%s%s", i ? ", " : "", name(i));
}
static const char *glyph_name(int i) { return muse_apps_glyphs[i].name; }
static const char *color_name(int i) { return muse_apps_colors[i].name; }

void muse_apps_glyph_names(char *out, int cap) {
    names(out, cap, glyph_name, muse_apps_glyph_count);
}

void muse_apps_color_names(char *out, int cap) {
    names(out, cap, color_name, muse_apps_color_count);
}
