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
caption keeps the screen for every page of a reply, with or without CJK, every
line of a reply reaches the screen through the caption buffer, every page fits
the screen in the font it's drawn in, and the band comes back when the reply
ends.

The layout functions are compiled from muse_ui.c unchanged, against a fake
LVGL with LVGL 9.5's font metrics (glyph advances of Montserrat 14 and 20 and
unscii-8) that records the caption label's geometry and font and wraps text
as a label does. Their pages go through the real pager, muse_chat_text.c, into
a MUSE_CAPTION_MAX buffer as the voice task's."""

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

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_settings.h"
#include "muse_state.h"

bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap);

#define CAPTION_LINE_SPACE 2
#define TAG "muse_ui"
#define ESP_LOGI(...) ((void)0)

/* LVGL 9.5's line heights and glyph advances, ' ' to '~'. */
static const unsigned char M14_W[95] = {4,4,6,10,9,12,10,3,5,5,6,8,3,5,3,5,9,5,8,8,9,8,9,8,9,9,3,3,8,8,8,8,15,10,11,10,12,9,9,11,11,4,7,10,8,13,11,12,10,12,10,9,8,11,10,16,9,9,9,5,5,5,8,7,8,8,10,8,10,9,5,10,10,4,4,9,4,15,10,9,10,10,6,7,6,10,8,13,8,8,7,5,4,5,8};
static const unsigned char M20_W[95] = {5,5,8,14,12,17,14,4,7,7,8,12,5,8,5,7,13,7,12,11,13,12,12,12,13,12,5,5,12,12,12,11,21,15,15,14,17,13,13,15,16,6,10,14,12,19,16,17,14,17,15,12,12,16,14,23,13,13,13,7,7,7,12,10,12,12,14,11,14,12,7,14,14,6,6,12,6,21,14,13,14,14,8,10,8,14,11,18,11,11,10,7,6,7,12};
typedef struct { int line_h, glyph_w; const unsigned char *adv; } lv_font_t;
static const lv_font_t lv_font_unscii_8 = { 9, 8, NULL };
static const lv_font_t lv_font_unscii_16 = { 17, 16, NULL };
static const lv_font_t lv_font_montserrat_14 = { 16, 10, M14_W };
static const lv_font_t lv_font_montserrat_20 = { 22, 14, M20_W };
static const lv_font_t cjk_font = { 17, 16, NULL };   /* unscii-16 with the CJK fallback */
static const lv_font_t *caption_font(void) { return &cjk_font; }
static int lv_font_get_line_height(const lv_font_t *f) { return f->line_h; }
static int advance(const lv_font_t *f, int c) { return f->adv && c >= ' ' && c <= '~' ? f->adv[c - ' '] : f->glyph_w; }
static int lv_font_get_glyph_width(const lv_font_t *f, int c, int next) { (void)next; return advance(f, c); }

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
            w += advance(f, (unsigned char)*e++);
        }
        if (x && x + w > max_w) {
            rows++, x = 0;
        }
        for (; p < e; p++) {
            int g = advance(f, (unsigned char)*p);
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

typedef struct { int w, h, pad_hor, line_space, opa, long_mode, align, y; bool hidden; const lv_font_t *font; char text[512]; } lv_obj_t;
static void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof(o->text), "%s", t); }
static char *lv_label_get_text(lv_obj_t *o) { return o->text; }
enum { LV_OPA_70 = 178, LV_OPA_COVER = 255 };
enum { LV_LABEL_LONG_MODE_WRAP, LV_LABEL_LONG_MODE_DOTS };
enum { LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID };
enum { LV_OBJ_FLAG_HIDDEN = 1 };
static void lv_obj_set_size(lv_obj_t *o, int w, int h) { o->w = w; o->h = h; }
static void lv_obj_set_height(lv_obj_t *o, int h) { o->h = h; }
static void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s) { (void)s; o->font = f; }
static void lv_obj_set_style_text_line_space(lv_obj_t *o, int v, int s) { (void)s; o->line_space = v; }
static void lv_obj_set_style_pad_hor(lv_obj_t *o, int v, int s) { (void)s; o->pad_hor = v; }
static void lv_obj_set_style_bg_opa(lv_obj_t *o, int v, int s) { (void)s; o->opa = v; }
static void lv_label_set_long_mode(lv_obj_t *o, int m) { o->long_mode = m; }
static void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)x; o->align = a; o->y = y; }
static void lv_obj_set_flag(lv_obj_t *o, int f, bool on) { (void)f; o->hidden = on; }

static int s_size = MUSE_TEXT_MEDIUM;
int muse_settings_text_size(void) { return s_size; }

/* muse_state.c's pages, with a mode that puts every line on one page. */
static int s_cols, s_lines, s_cjk_cols, s_cjk_page_lines;
static bool s_whole;
void muse_state_set_page(int cols, int lines) { s_cols = cols; s_lines = lines; }
void muse_state_set_cjk_page(int cols, int lines) { s_cjk_cols = cols; s_cjk_page_lines = lines; }
void muse_state_page(bool cjk, int *cols, int *lines)
{
    bool own = cjk && s_cjk_cols > 0;
    *cols = own ? s_cjk_cols : s_cols;
    *lines = s_whole ? 100000 : own ? s_cjk_page_lines : s_lines;
}

static lv_obj_t caption_obj, canvas_obj;
static lv_obj_t *s_caption_lbl = &caption_obj, *s_canvas = &canvas_obj;
static int s_w, s_h;
static bool s_tall, s_tv;

#include "extracted.inc"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %dx%d %s:%d: ", s_w, s_h, __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

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
    CHECK(n > 10 && missing == 0, "%s: %d of %d lines never shown", what, missing, n);
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

/* Ideographs with no spaces, each different, three bytes each. */
static void cjk_reply(char *out, size_t cap)
{
    size_t o = 0;
    for (uint32_t cp = 0x4E00; cp < 0x4E00 + 1200 && o + 4 < cap; cp++) {
        out[o++] = (char)(0xE0 | cp >> 12);
        out[o++] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    out[o] = '\0';
}

/* Pages through `text` as update_status shows them while the reply has the
 * screen, and checks each page fits the screen in the font it's drawn in. A
 * page of ordinary prose keeps the Text size font. */
static int pages_fit(const char *what, const char *text, bool prose)
{
    static char page[MUSE_CAPTION_MAX];
    int small = 0;
    size_t len = strlen(text);
    for (size_t at = 0; at < len; at += 7) {
        if (!muse_hatch_caption_at(text, at, page, sizeof(page))) {
            continue;
        }
        lv_label_set_text(s_caption_lbl, page);
        set_full_font();
        lv_point_t size;
        lv_text_get_size(&size, page, caption_obj.font, 0, caption_obj.line_space, s_w - 6, LV_TEXT_FLAG_NONE);
        CHECK(size.y <= s_full_reply_h - 4, "%s, size %d: a page %d px tall in a %d px screen", what, s_size, (int)size.y,
              s_full_reply_h - 4);
        CHECK(!prose || caption_obj.font == s_full_font, "%s, size %d: prose drawn at Small", what, s_size);
        small += caption_obj.font != s_full_font;
    }
    return small;
}

static void full(const char *step)
{
    CHECK(caption_obj.h == s_full_reply_h && caption_obj.w == s_w, "%s: caption is %dx%d, not the full screen %dx%d",
          step, caption_obj.w, caption_obj.h, s_w, s_full_reply_h);
    CHECK(caption_obj.long_mode == LV_LABEL_LONG_MODE_WRAP && caption_obj.opa == LV_OPA_COVER, "%s: not wrapped on black", step);
    CHECK(canvas_obj.hidden, "%s: Muse still shown", step);
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

    /* build_screen's band, then the option's setup. */
    lv_obj_set_size(s_caption_lbl, s_w, 2 * 8 + 2 + 4);
    lv_obj_set_style_text_font(s_caption_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_line_space(s_caption_lbl, 2, 0);
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_70, 0);
#if CONFIG_MUSE_CJK_FONT
    s_cjk_lines = !s_tall && s_h >= 200 ? 2 : 1;
    muse_state_set_cjk_page((s_w - 2 * CJK_BAND_PAD) / 16, s_cjk_lines);
#endif
    s_full_reply_h = s_h - FULL_REPLY_TOP - 6;
    s_band_font = &lv_font_unscii_8;
    for (s_size = MUSE_TEXT_LARGE; s_size >= MUSE_TEXT_SMALL; s_size--) {
        apply_text_size();
        CHECK(s_lines >= 2, "size %d: page %dx%d", s_size, s_cols, s_lines);
        ascii_reply(text, sizeof(text), s_cols);
        every_line_shown("ASCII", text);
    }
    s_size = MUSE_TEXT_MEDIUM;
    apply_text_size();
    const lv_font_t *medium = &lv_font_montserrat_14;
#if CONFIG_MUSE_CJK_FONT
    int lines_h = s_cjk_page_lines * cjk_font.line_h + (s_cjk_page_lines - 1) * CJK_LINE_SPACE;
    CHECK(s_cjk_page_lines > s_cjk_lines && lines_h <= s_full_reply_h - 4 && s_cjk_cols * 16 <= s_w - 6,
          "CJK page %dx%d is the band's or doesn't fit the screen", s_cjk_cols, s_cjk_page_lines);
    cjk_reply(text, sizeof(text));
    every_line_shown("CJK", text);
#endif

    /* Every page of a reply fits; a page of capitals falls back to Small. */
#if CONFIG_MUSE_CJK_FONT
    set_caption_font(false);
#endif
    set_full_reply(true);
    for (s_size = MUSE_TEXT_LARGE; s_size >= MUSE_TEXT_MEDIUM; s_size--) {
        apply_text_size();
        static const char prose[] = "Sure, it should be mild in town today, with a light breeze this morning and some sun "
                                    "later on. Bring a jacket for the walk home, as it gets cooler after six. ";
        static const char caps[] = "IMPORTANT: MAINTENANCE WINDOW MOVED TO WEDNESDAY MORNING. WARNING: WATER MAIN "
                                   "WORK NEAR MAPLE AVENUE WILL MEAN NO WATER FROM NOW UNTIL NOON. ";
        snprintf(text, sizeof(text), "%s%s%s", prose, prose, prose);
        pages_fit("prose", text, true);
        snprintf(text, sizeof(text), "%s%s%s", caps, caps, caps);
        pages_fit("capitals", text, false);
        /* Whole lines of the widest capital: every line wraps again. */
        size_t o = 0;
        for (int i = 0; i < 40; i++) {
            for (int c = 0; c < s_cols; c++) {
                text[o++] = 'W';
            }
            text[o++] = ' ';
        }
        text[o] = '\0';
        int small = pages_fit("lines of W", text, false);
        CHECK(s_w != 135 || s_size != MUSE_TEXT_MEDIUM || small > 0, "lines of W on the StickS3 never fell back to Small");
    }
    s_size = MUSE_TEXT_MEDIUM;
    apply_text_size();
    set_full_reply(false);

    /* A reply: the first page, then the next ones as they come. */
#if CONFIG_MUSE_CJK_FONT
    set_caption_font(false);
#endif
    set_full_reply(true);
    full("first page");
    CHECK(caption_obj.font == medium, "first page: not the Text size font");
#if CONFIG_MUSE_CJK_FONT
    /* update_status: the band's font for the caption, its text, then the full screen's font. */
    set_caption_font(false);
    lv_label_set_text(s_caption_lbl, "The next page.");
    set_full_font();
    full("next page");
    CHECK(caption_obj.font == medium, "next page: not the Text size font");
    set_caption_font(true);
    lv_label_set_text(s_caption_lbl, "\xe4\xbb\x8a\xe5\xa4\xa9");
    set_full_font();
    full("CJK page");
    CHECK(caption_obj.font == &cjk_font, "CJK page: not the caption font");
    s_size = MUSE_TEXT_LARGE;
    apply_text_size();
    full("Text size changed");
    CHECK(caption_obj.font == &cjk_font, "Text size changed: CJK lost its font");
    set_full_reply(false);
    CHECK(!canvas_obj.hidden && caption_obj.long_mode == LV_LABEL_LONG_MODE_DOTS, "reply over: no band");
    CHECK(caption_obj.font == &cjk_font && caption_obj.pad_hor == CJK_BAND_PAD, "reply over: not the CJK band's font");
    set_caption_font(false);
    CHECK(caption_obj.font == &lv_font_unscii_8 && caption_obj.h == 2 * 8 + 2 + 4, "band: not two lines of unscii-8");
#else
    set_full_reply(false);
    CHECK(!canvas_obj.hidden && caption_obj.long_mode == LV_LABEL_LONG_MODE_DOTS, "reply over: no band");
    CHECK(caption_obj.font == &lv_font_unscii_8 && caption_obj.h == 2 * 8 + 2 + 4 && caption_obj.pad_hor == 0,
          "reply over: not the band");
#endif
    return failures != 0;
}
"""


def function(source: str, name: str) -> str:
    """One whole function definition from muse_ui.c, or "" if it isn't there."""
    match = re.search(r"^static void " + name + r"\([^;{]*\)\n\{", source, re.M)
    if not match:
        return ""
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def extract(source: str) -> str:
    statics = re.search(r"^#if CONFIG_MUSE_SMALL_FULL_REPLY\n#define FULL_REPLY_TOP.*?^#endif\n", source, re.M | re.S)
    cjk = re.search(r"^#define CJK_BAND_PAD.*?(?=^/\*.*\n.*\*/\nstatic void set_caption_font)", source, re.M | re.S)
    if not statics or not cjk:
        raise AssertionError("muse_ui.c no longer has the full-reply statics or the CJK band")
    parts = [statics.group(0), "#if CONFIG_MUSE_CJK_FONT\n", cjk.group(0), function(source, "set_caption_font"), "#endif\n"]
    for name in ("set_full_font", "apply_text_size", "set_full_reply"):
        parts.append(function(source, name))
    return "".join(parts)


@unittest.skipUnless(shutil.which(shlex.split(os.environ.get("CC", "cc"))[0]), "C compiler not available")
class FullReplyTest(unittest.TestCase):
    def run_harness(self, cjk: bool) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
            (stage / "extracted.inc").write_text(extract((MUSE / "muse_ui.c").read_text()))
            (stage / "harness.c").write_text(HARNESS)
            binary = stage / "harness"
            build = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-include", str(ROOT / "tests" / "host_compat.h"),
                 "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                 "-DCONFIG_MUSE_SMALL_FULL_REPLY=1", f"-DCONFIG_MUSE_CJK_FONT={int(cjk)}",
                 "-I", str(stage), "-I", str(MUSE), str(stage / "harness.c"),
                 str(MUSE / "muse_chat_text.c"), str(MUSE / "muse_text.c"), "-o", str(binary)],
                text=True, capture_output=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for name, (w, h, tall) in SCREENS.items():
                with self.subTest(screen=name):
                    run = subprocess.run([str(binary), str(w), str(h), str(tall)], text=True, capture_output=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_without_cjk_font(self) -> None:
        self.run_harness(cjk=False)

    def test_with_cjk_font(self) -> None:
        """Main's CJK band resizes the caption on every caption: not while a reply has the screen."""
        self.run_harness(cjk=True)


if __name__ == "__main__":
    unittest.main()
