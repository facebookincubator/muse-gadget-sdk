# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Full-screen replies on small screens (CONFIG_MUSE_SMALL_FULL_REPLY): the
caption keeps the screen for every page of a reply, every line of a reply
reaches the screen through the caption buffer, accented and CJK ones included,
every page fits the screen in the Text size font (capitals and lines of W too)
or, for a page with CJK or accented letters, in the font that draws them, and
the band comes back when the reply ends, as tall as build_screen made it and
tall enough for its lines: two of unscii-8, the CJK band's, or one accented.

The layout functions and build_screen's band and setup are compiled from muse_ui.c
unchanged, against a fake LVGL with LVGL 9.5's font metrics (glyph advances of
Montserrat 14 and 20, the fixed cells of unscii and Unifont, and the box LVGL
measures for a glyph a font lacks) that records the caption label's geometry
and font and wraps text as a label does. Their pages go through the real
pager, muse_chat_text.c, into a MUSE_CAPTION_MAX buffer as the voice task's,
and through muse_text.c as muse_state's caption does."""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

# Compact screens, as muse_ui_start lays them out: width, height, tall.
SCREENS = {
    "AIPI Lite": (128, 128, 0),
    "StickS3": (135, 240, 1),
    "XIAO 1.47": (172, 320, 1),
    "AI Passport": (240, 320, 1),
    "ATK box": (320, 240, 0),
    "RoRoLee": (240, 120, 0),
}
# The SpotPear ESP32-AI-1.28 keeps a narrower, higher band inside its circle.
SPOTPEAR = {"SpotPear 1.28": (240, 240, 0)}

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_chat.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"

bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap);

#define CAPTION_LINE_SPACE 2
#define TAG "muse_ui"
#define ESP_LOGI(...) ((void)0)

/* LVGL 9.5's line heights and glyph advances, ' ' to '~'. */
static const unsigned char M14_W[95] = {4,4,6,10,9,12,10,3,5,5,6,8,3,5,3,5,9,5,8,8,9,8,9,8,9,9,3,3,8,8,8,8,15,10,11,10,12,9,9,11,11,4,7,10,8,13,11,12,10,12,10,9,8,11,10,16,9,9,9,5,5,5,8,7,8,8,10,8,10,9,5,10,10,4,4,9,4,15,10,9,10,10,6,7,6,10,8,13,8,8,7,5,4,5,8};
static const unsigned char M20_W[95] = {5,5,8,14,12,17,14,4,7,7,8,12,5,8,5,7,13,7,12,11,13,12,12,12,13,12,5,5,12,12,12,11,21,15,15,14,17,13,13,15,16,6,10,14,12,19,16,17,14,17,15,12,12,16,14,23,13,13,13,7,7,7,12,10,12,12,14,11,14,12,7,14,14,6,6,12,6,21,14,13,14,14,8,10,8,14,11,18,11,11,10,7,6,7,12};
/* wide: glyphs past ASCII (CJK, accented letters) are in the font. */
typedef struct { int line_h, glyph_w; const unsigned char *adv; bool wide; } lv_font_t;
static const lv_font_t lv_font_unscii_8 = { 9, 8, NULL, false };
static const lv_font_t lv_font_unscii_16 = { 17, 16, NULL, false };
static const lv_font_t lv_font_montserrat_14 = { 16, 10, M14_W, false };
static const lv_font_t lv_font_montserrat_20 = { 22, 14, M20_W, false };
static const lv_font_t cjk_font = { 17, 16, NULL, true };   /* unscii-16 with the CJK fallback */
static const lv_font_t muse_font_latin_16 = { 16, 8, NULL, true };   /* Unifont's 8x16 */
static const lv_font_t *caption_font(void) { return &cjk_font; }
static int lv_font_get_line_height(const lv_font_t *f) { return f->line_h; }
static int advance(const lv_font_t *f, uint32_t c)
{
    if (c >= ' ' && c <= '~') {
        return f->adv ? f->adv[c - ' '] : f->glyph_w;
    }
    /* LVGL draws a box for a glyph the font lacks, this wide. */
    return f->wide ? f->glyph_w : f->line_h / 2 + 2;
}
static int lv_font_get_glyph_width(const lv_font_t *f, uint32_t c, uint32_t next) { (void)next; return advance(f, c); }

static uint32_t utf8(const char **s)
{
    const unsigned char *u = (const unsigned char *)*s;
    int n = u[0] < 0x80 ? 1 : u[0] < 0xE0 ? 2 : u[0] < 0xF0 ? 3 : 4;
    uint32_t c = n == 1 ? u[0] : u[0] & (0x7F >> n);
    for (int i = 1; i < n && u[i]; i++) {
        c = c << 6 | (u[i] & 0x3F);
    }
    *s += n;
    return c;
}

/* Wraps as a label does: at spaces, and inside a word wider than the line. */
typedef struct { int32_t x, y; } lv_point_t;
enum { LV_TEXT_FLAG_NONE };
static void lv_text_get_size(lv_point_t *size, const char *text, const lv_font_t *f, int32_t letter_space,
                             int32_t line_space, int32_t max_w, int flag)
{
    (void)letter_space; (void)flag;
    int rows = 1, x = 0, widest = 0;
    for (const char *p = text; *p;) {
        if (*p == '\n') {
            rows++, x = 0, p++;
            continue;
        }
        if (*p == ' ') {
            x += advance(f, ' '), p++;
            continue;
        }
        int w = 0;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') {
            w += advance(f, utf8(&e));
        }
        if (x && x + w > max_w) {
            rows++, x = 0;
        }
        while (p < e) {
            int g = advance(f, utf8(&p));
            if (x && x + g > max_w) {
                rows++, x = 0;
            }
            x += g;
            widest = x > widest ? x : widest;
        }
    }
    size->x = widest;
    size->y = rows * f->line_h + (rows - 1) * line_space;
}

typedef struct { int w, h, pad_hor, pad_ver, line_space, opa, long_mode, align, y; bool hidden; const lv_font_t *font; char text[512]; } lv_obj_t;
static void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof(o->text), "%s", t); }
static char *lv_label_get_text(lv_obj_t *o) { return o->text; }
enum { LV_OPA_70 = 178, LV_OPA_COVER = 255 };
static int lv_color_black(void) { return 0; }
enum { LV_LABEL_LONG_MODE_WRAP, LV_LABEL_LONG_MODE_DOTS, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR };
enum { LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID };
enum { LV_OBJ_FLAG_HIDDEN = 1 };
static void lv_obj_set_size(lv_obj_t *o, int w, int h) { o->w = w; o->h = h; }
static void lv_obj_set_height(lv_obj_t *o, int h) { o->h = h; }
static void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s) { (void)s; o->font = f; }
static void lv_obj_set_style_text_line_space(lv_obj_t *o, int v, int s) { (void)s; o->line_space = v; }
static void lv_obj_set_style_pad_hor(lv_obj_t *o, int v, int s) { (void)s; o->pad_hor = v; }
static void lv_obj_set_style_pad_ver(lv_obj_t *o, int v, int s) { (void)s; o->pad_ver = v; }
static void lv_obj_set_style_bg_color(lv_obj_t *o, int c, int s) { (void)o; (void)c; (void)s; }
static void lv_obj_set_style_bg_opa(lv_obj_t *o, int v, int s) { (void)s; o->opa = v; }
static void lv_label_set_long_mode(lv_obj_t *o, int m) { o->long_mode = m; }
static void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)x; o->align = a; o->y = y; }
static void lv_obj_set_flag(lv_obj_t *o, int f, bool on) { (void)f; o->hidden = on; }

static int s_size = MUSE_TEXT_MEDIUM;
int muse_settings_text_size(void) { return s_size; }

/* muse_state.c's pages, with a mode that puts every line on one page. */
static int s_cols, s_lines, s_cjk_cols, s_cjk_page_lines, s_latin_cols, s_latin_page_lines;
static bool s_whole;
void muse_state_set_page(int cols, int lines) { s_cols = cols; s_lines = lines; }
void muse_state_set_cjk_page(int cols, int lines) { s_cjk_cols = cols; s_cjk_page_lines = lines; }
void muse_state_set_latin_page(int cols, int lines) { s_latin_cols = cols; s_latin_page_lines = lines; }
void muse_state_page(const char *text, int *cols, int *lines)
{
    if (s_cjk_cols > 0 && muse_text_has_cjk(text)) {
        *cols = s_cjk_cols;
        *lines = s_cjk_page_lines;
    } else if (s_latin_cols > 0 && muse_text_has_latin(text)) {
        *cols = s_latin_cols;
        *lines = s_latin_page_lines;
    } else {
        *cols = s_cols;
        *lines = s_lines;
    }
    if (s_whole) {
        *lines = 100000;
    }
}

static lv_obj_t caption_obj, canvas_obj;
static lv_obj_t *s_caption_lbl = &caption_obj, *s_canvas = &canvas_obj;
static int s_w, s_h;
static bool s_tall, s_tv, s_long;

#include "extracted.inc"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %dx%d %s:%d: ", s_w, s_h, __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* update_status with a new caption: the band's font for it (which a reply
 * that has the screen keeps), its text, the full screen's font, then the
 * screen for the mode. */
static void show(const char *page, bool replying)
{
    static char caption[MUSE_CAPTION_MAX];
    snprintf(caption, sizeof(caption), "%s", page);
    muse_text_to_caption(caption, sizeof(caption));   /* as muse_state_set_caption */
#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT
    set_caption_font(caption);
#endif
    lv_label_set_text(s_caption_lbl, caption);
    if (s_full_reply) {
        set_full_font();
    }
    s_caption_shown = caption[0] != '\0';
    apply_text_size();
    set_full_reply(s_caption_shown && replying);
}

/* Pages through `text` as the voice task does while it plays, and checks that
 * each of its lines is shown on some page. */
static void every_line_shown(const char *what, const char *text)
{
    static char all[1 << 16], page[MUSE_CAPTION_MAX];
    static char *lines[4096];
    static bool seen[4096];
    s_whole = true;
    muse_hatch_caption_at(text, 0, all, sizeof(all));
    s_whole = false;
    int n = 0;
    for (char *p = strtok(all, "\n"); p && n < 4096; p = strtok(NULL, "\n")) {
        lines[n] = p;
        seen[n++] = false;
    }
    /* The page only changes when playback reaches the next line. */
    const char *from = text;
    for (int k = 0; k < n; k++) {
        const char *start = strstr(from, lines[k]);
        if (!start || !muse_hatch_caption_at(text, (size_t)(start - text), page, sizeof(page))) {
            continue;
        }
        from = start + strlen(lines[k]);
        for (char *p = strtok(page, "\n"); p; p = strtok(NULL, "\n")) {
            for (int i = 0; i < n; i++) {
                if (!seen[i] && !strcmp(lines[i], p)) {
                    seen[i] = true;
                    break;
                }
            }
        }
    }
    int missing = 0;
    for (int i = 0; i < n; i++) {
        missing += !seen[i];
    }
    CHECK(n > 10 && missing == 0, "%s, size %d: %d of %d lines never shown", what, s_size, missing, n);
}

/* Words a whole line long, each different. */
static void ascii_reply(char *out, size_t cap, int cols)
{
    size_t o = 0;
    for (int i = 0; i < 150 && o + cols + 2 < cap; i++) {
        o += snprintf(out + o, cap - o, "%s%04d", i ? " " : "", i);
        for (int c = 4; c < cols; c++) {
            out[o++] = 'x';
        }
        out[o] = '\0';
    }
}

/* Words of a Vietnamese letter, three bytes in UTF-8 but one column as
 * captions show it, each word different. */
static void latin_reply(char *out, size_t cap, int cols, const char *letter)
{
    size_t o = 0, n = strlen(letter);
    for (int i = 0; i < 60 && o + n * cols + 4 < cap; i++) {
        if (i) {
            out[o++] = ' ';
        }
        for (int c = 2; c < cols; c++) {
            memcpy(out + o, letter, n);
            o += n;
        }
        o += snprintf(out + o, cap - o, "%02d", i);
    }
    out[o] = '\0';
}

/* Ideographs with no spaces, each different, three bytes each. */
static void cjk_reply(char *out, size_t cap, int count)
{
    size_t o = 0;
    for (uint32_t cp = 0x4E00; cp < 0x4E00 + (uint32_t)count && o + 4 < cap; cp++) {
        out[o++] = (char)(0xE0 | cp >> 12);
        out[o++] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    out[o] = '\0';
}

/* Pages through `text` as update_status shows them while the reply has the
 * screen, and checks each page fits the screen in the font it's drawn in: the
 * Text size font, or with their fonts, the one that draws CJK or accented
 * letters. */
static void pages_fit(const char *what, const char *text)
{
    static char page[MUSE_CAPTION_MAX];
    size_t len = strlen(text);
    int pages = 0;
    for (size_t at = 0; at < len; at += 7) {
        if (!muse_hatch_caption_at(text, at, page, sizeof(page))) {
            continue;
        }
        show(page, true);
        pages++;
        const lv_font_t *want = s_full_font;
#if CONFIG_MUSE_CJK_FONT
        if (muse_text_has_cjk(caption_obj.text)) {
            want = &cjk_font;
        } else
#endif
#if CONFIG_MUSE_LATIN_FONT
        if (muse_text_has_latin(caption_obj.text)) {
            want = &muse_font_latin_16;
        }
#endif
        CHECK(caption_obj.font == want, "%s, size %d: a page not drawn in its font", what, s_size);
        lv_point_t size;
        lv_text_get_size(&size, caption_obj.text, caption_obj.font, 0, caption_obj.line_space, s_w - 2 * caption_obj.pad_hor,
                         LV_TEXT_FLAG_NONE);
        CHECK(size.y <= s_full_reply_h - 4, "%s, size %d: a page %d px tall in a %d px screen", what, s_size, (int)size.y,
              s_full_reply_h - 4);
    }
    CHECK(pages > 1, "%s, size %d: %d pages", what, s_size, pages);
    show("", false);
}

static void full(const char *step)
{
    CHECK(caption_obj.h == s_full_reply_h && caption_obj.w == s_w, "%s: caption is %dx%d, not the full screen %dx%d",
          step, caption_obj.w, caption_obj.h, s_w, s_full_reply_h);
    CHECK(caption_obj.long_mode == LV_LABEL_LONG_MODE_WRAP && caption_obj.opa == LV_OPA_COVER, "%s: not wrapped on black", step);
    CHECK(canvas_obj.hidden, "%s: Muse still shown", step);
}

/* The band holds its lines inside its padding: two of unscii-8, as replies
 * are paged there, s_cjk_lines of CJK, or one accented line. */
static void band(const char *step)
{
    CHECK(!canvas_obj.hidden && caption_obj.opa == LV_OPA_70 && caption_obj.align == LV_ALIGN_BOTTOM_MID, "%s: no band", step);
    CHECK(caption_obj.long_mode != LV_LABEL_LONG_MODE_WRAP, "%s: the band wraps", step);
    int rows = caption_obj.font == &lv_font_unscii_8 ? 2 : 1;
#if CONFIG_MUSE_CJK_FONT
    if (caption_obj.font == &cjk_font) {
        rows = s_cjk_lines;
    }
#endif
    int need = rows * lv_font_get_line_height(caption_obj.font) + (rows - 1) * caption_obj.line_space;
    CHECK(caption_obj.h - 2 * caption_obj.pad_ver >= need, "%s: a %d px band can't hold %d lines (%d px) of its font", step,
          caption_obj.h, rows, need);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        return 2;
    }
    s_w = atoi(argv[1]);
    s_h = atoi(argv[2]);
    s_tall = atoi(argv[3]);
    static char text[8192];
    static const char prose[] = "Sure, it should be mild in town today, with a light breeze this morning and some sun "
                                "later on. Bring a jacket for the walk home, as it gets cooler after six. ";
    static const char caps[] = "IMPORTANT: MAINTENANCE WINDOW MOVED TO WEDNESDAY MORNING. WARNING: WATER MAIN "
                               "WORK NEAR MAPLE AVENUE WILL MEAN NO WATER FROM NOW UNTIL NOON. ";
    static const char vi[] = "H\xc3\xb4m nay tr\xe1\xbb\x9di \xe1\xba\xa5m \xc3\xa1p, c\xc3\xb3 gi\xc3\xb3 nh\xe1\xba\xb9 "
                             "bu\xe1\xbb\x95i s\xc3\xa1ng v\xc3\xa0 n\xe1\xba\xafng v\xe1\xbb\x81 chi\xe1\xbb\x81u. ";

    /* build_screen's band, then the option's setup. */
    lv_obj_set_style_text_font(s_caption_lbl, &lv_font_unscii_8, 0);
#include "band.inc"
    const int band_h = caption_obj.h, band_w = caption_obj.w, band_y = caption_obj.y;
    band("the band");
#if CONFIG_MUSE_CJK_FONT
    s_cjk_lines = !s_tall && s_h >= 200 ? 2 : 1;
    muse_state_set_cjk_page((s_w - 2 * CJK_BAND_PAD) / 16, s_cjk_lines);
#endif
#if CONFIG_MUSE_LATIN_FONT
    muse_text_keep_latin();
    muse_state_set_latin_page((s_w - 52) / 8, 1);
#endif
    full_reply_setup();

    /* Every line of a reply reaches the screen, at every size. */
    for (s_size = MUSE_TEXT_LARGE; s_size >= MUSE_TEXT_SMALL; s_size--) {
        apply_text_size();
        CHECK(s_lines >= 2 && s_cols >= 8, "size %d: page %dx%d", s_size, s_cols, s_lines);
        ascii_reply(text, sizeof(text), s_cols < 16 ? s_cols : 16);
        every_line_shown("ASCII", text);
        latin_reply(text, sizeof(text), s_cols < 16 ? s_cols : 16, "\xe1\xbb\x99");   /* U+1ED9 */
        every_line_shown("Vietnamese", text);
        latin_reply(text, sizeof(text), s_cols < 16 ? s_cols : 16, "\xc3\xa9");   /* U+00E9 */
        every_line_shown("accented", text);
        cjk_reply(text, sizeof(text), 1200);
        every_line_shown("CJK", text);
#if CONFIG_MUSE_CJK_FONT
        int lines_h = s_cjk_page_lines * cjk_font.line_h + (s_cjk_page_lines - 1) * CJK_LINE_SPACE;
        CHECK((s_cjk_page_lines > s_cjk_lines || s_cjk_page_lines == s_lines) && lines_h <= s_full_reply_h - 4 &&
              s_cjk_cols * 16 <= s_w - 6 && s_cjk_page_lines <= s_lines,
              "size %d: CJK page %dx%d is the band's or doesn't fit the screen", s_size, s_cjk_cols, s_cjk_page_lines);
#endif
#if CONFIG_MUSE_LATIN_FONT
        CHECK(s_latin_cols * 8 <= s_w - 6 && s_latin_page_lines <= s_lines && s_latin_page_lines >= 2,
              "size %d: accented page %dx%d doesn't fit the screen", s_size, s_latin_cols, s_latin_page_lines);
#endif
    }

    /* Every page fits at its Text size, capitals and lines of W too. */
    for (s_size = MUSE_TEXT_LARGE; s_size >= MUSE_TEXT_SMALL; s_size--) {
        apply_text_size();
        snprintf(text, sizeof(text), "%s%s%s", prose, prose, prose);
        pages_fit("prose", text);
        snprintf(text, sizeof(text), "%s%s%s", caps, caps, caps);
        pages_fit("capitals", text);
        /* Whole lines of the widest capital: every line wraps again. */
        size_t o = 0;
        for (int i = 0; i < 40; i++) {
            for (int c = 0; c < s_cols && c < 40; c++) {
                text[o++] = 'W';
            }
            text[o++] = ' ';
        }
        text[o] = '\0';
        pages_fit("lines of W", text);
        snprintf(text, sizeof(text), "%s%s%s%s", vi, vi, vi, vi);
        pages_fit("Vietnamese", text);
        /* A reply with CJK and pages without any, as Muse mixes them. */
        cjk_reply(text, sizeof(text), 60);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), " %s%s%s", prose, caps, prose);
        pages_fit("CJK and English", text);
        snprintf(text, sizeof(text), "%s%s%s%s", vi, caps, prose, caps);
        pages_fit("Vietnamese and English", text);
    }

    /* A reply: the first page, then the next ones as they come. */
    s_size = MUSE_TEXT_MEDIUM;
    apply_text_size();
    show("It's mild today.", false);
    band("before the reply");
    show("It's mild today.", true);
    full("first page");
    CHECK(caption_obj.font == &lv_font_montserrat_14, "first page: not the Text size font");
    show("The next page.", true);
    full("next page");
    CHECK(caption_obj.font == &lv_font_montserrat_14, "next page: not the Text size font");
#if CONFIG_MUSE_CJK_FONT
    show("\xe4\xbb\x8a\xe5\xa4\xa9", true);
    full("CJK page");
    CHECK(caption_obj.font == &cjk_font && caption_obj.line_space == CJK_LINE_SPACE, "CJK page: not the caption font");
    s_size = MUSE_TEXT_LARGE;
    show("\xe4\xbb\x8a\xe5\xa4\xa9", true);
    full("Text size changed");
    CHECK(caption_obj.font == &cjk_font, "Text size changed: CJK lost its font");
    show("\xe4\xbb\x8a\xe5\xa4\xa9", false);
    band("CJK reply over");
    CHECK(caption_obj.font == &cjk_font && caption_obj.pad_hor == CJK_BAND_PAD, "CJK reply over: not the CJK band's font");
    show("Hello.", false);
#endif
#if CONFIG_MUSE_LATIN_FONT
    show(vi, true);
    full("accented page");
    CHECK(caption_obj.font == &muse_font_latin_16, "accented page: not Unifont");
    show(vi, false);
    band("accented reply over");
    CHECK(caption_obj.font == &muse_font_latin_16 && caption_obj.long_mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR,
          "accented reply over: not the band's accented line");
    show("Hello.", false);
#endif
    s_size = MUSE_TEXT_MEDIUM;
    show("Bye.", true);
    full("last page");
    show("Bye.", false);
    band("reply over");
    CHECK(caption_obj.font == &lv_font_unscii_8 && caption_obj.h == band_h && caption_obj.pad_hor == 0 &&
          caption_obj.long_mode == LV_LABEL_LONG_MODE_DOTS, "reply over: not the band");
    CHECK(caption_obj.w == band_w && caption_obj.y == band_y, "reply over: band %d wide at %d, not build_screen's %d at %d",
          caption_obj.w, caption_obj.y, band_w, band_y);
    return failures != 0;
}
"""


def function(source: str, name: str) -> str:
    """One whole function definition from muse_ui.c, or "" if it isn't there."""
    match = re.search(r"^static (?:void|int) " + name + r"\([^;{]*\)\n\{", source, re.M)
    if not match:
        return ""
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def band(source: str) -> str:
    """build_screen's caption band on a small screen, up to its CJK setup."""
    match = re.search(r"^ +s_caption_lbl = make_label\(.*?\n +if \(s_small\) \{\n(.*?)^#if CONFIG_MUSE_CJK_FONT\n", source,
                      re.M | re.S)
    if not match:
        raise AssertionError("muse_ui.c no longer has build_screen's caption band")
    return match.group(1)


def extract(source: str) -> str:
    statics = re.search(r"^#if CONFIG_MUSE_SMALL_FULL_REPLY\n#define FULL_REPLY_TOP.*?^#endif\n", source, re.M | re.S)
    cjk = re.search(r"^#define CJK_BAND_PAD.*?^#endif\n", source, re.M | re.S)
    setup = re.search(r"^#if CONFIG_MUSE_SMALL_FULL_REPLY\n( +if \(!s_long\) \{.*?\n +\}\n)#endif\n", source, re.M | re.S)
    if not statics or not cjk or not setup:
        raise AssertionError("muse_ui.c no longer has the full-reply statics, the CJK band or build_screen's setup")
    place = re.search(r"^#if CONFIG_MUSE_BOARD_SPOTPEAR_ESP32_AI_128\n#define SMALL_BAND_W.*?^#endif\n", source, re.M | re.S)
    if not place:
        raise AssertionError("muse_ui.c no longer has the small band's place")
    parts = [statics.group(0), "#if CONFIG_MUSE_CJK_FONT\n", cjk.group(0), function(source, "caption_band_h"), place.group(0)]
    parts += ["#if CONFIG_MUSE_CJK_FONT || CONFIG_MUSE_LATIN_FONT\n", function(source, "set_caption_font"), "#endif\n"]
    for name in ("set_full_font", "full_glyph_px", "full_rows", "apply_text_size", "set_full_reply"):
        body = function(source, name)
        if not body:
            raise AssertionError(f"muse_ui.c no longer has {name}()")
        parts.append(body)
    parts.append("static void full_reply_setup(void)\n{\n" + setup.group(1) + "}\n")
    return "".join(parts)


@unittest.skipUnless(shutil.which(shlex.split(os.environ.get("CC", "cc"))[0]), "C compiler not available")
class FullReplyTest(unittest.TestCase):
    def run_harness(self, cjk: bool, latin: bool, spotpear: bool = False) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
            (stage / "sdkconfig.h").write_text("#pragma once\n")
            source = (MUSE / "muse_ui.c").read_text()
            (stage / "extracted.inc").write_text(extract(source))
            (stage / "band.inc").write_text(band(source))
            (stage / "harness.c").write_text(HARNESS)
            binary = stage / "harness"
            build = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-include", str(ROOT / "tests" / "host_compat.h"),
                 "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                 "-DCONFIG_MUSE_SMALL_FULL_REPLY=1", f"-DCONFIG_MUSE_CJK_FONT={int(cjk)}",
                 f"-DCONFIG_MUSE_LATIN_FONT={int(latin)}",
                 f"-DCONFIG_MUSE_BOARD_SPOTPEAR_ESP32_AI_128={int(spotpear)}",
                 "-I", str(stage), "-I", str(MUSE), str(stage / "harness.c"),
                 str(MUSE / "muse_chat_text.c"), str(MUSE / "muse_text.c"), "-o", str(binary)],
                text=True, capture_output=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for name, (w, h, tall) in (SPOTPEAR if spotpear else SCREENS).items():
                with self.subTest(screen=name):
                    run = subprocess.run([str(binary), str(w), str(h), str(tall)], text=True, capture_output=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_without_caption_fonts(self) -> None:
        """The AIPI Lite and StickS3 as they build."""
        self.run_harness(cjk=False, latin=False)

    def test_with_cjk_font(self) -> None:
        self.run_harness(cjk=True, latin=False)

    def test_with_latin_font(self) -> None:
        self.run_harness(cjk=False, latin=True)

    def test_with_both_fonts(self) -> None:
        self.run_harness(cjk=True, latin=True)

    def test_spotpear_band_comes_back_in_place(self) -> None:
        """A reply that took the screen gives the SpotPear its own band back."""
        for cjk, latin in ((False, False), (True, False), (True, True)):
            with self.subTest(cjk=cjk, latin=latin):
                self.run_harness(cjk=cjk, latin=latin, spotpear=True)


if __name__ == "__main__":
    unittest.main()
