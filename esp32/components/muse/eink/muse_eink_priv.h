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
/* What the e-ink face (muse_eink_ui.cpp) and Settings (muse_eink_settings.cpp) share. */
#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>
#include "muse_eink_text.h"

namespace muse_eink {

using freeink::ui::Color;
using freeink::ui::FontId;
using freeink::ui::Paint;
using freeink::ui::Rect;

using App = freeink::ui::FreeInkApp<160, 96>;
using Screen = App::ScreenType;

/* The DisplayTarget's font slots: the LVGL UI's fonts. */
enum : FontId { FONT_UNSCII8, FONT_UNSCII16, FONT_M14, FONT_M16, FONT_M20, FONT_M28 };

/* Action ids: the face's are below 100, Settings' from 100. */
enum : freeink::ui::ActionId { ACTION_SETTINGS_BASE = 100 };

extern App *g_app;
extern int g_w, g_h;

/* Drawing in the LVGL UI's terms: text by its pen position or centred. */
int text_width(FontId font, const char *text, int letter_space = 0);
int line_height(FontId font);
void draw_text(int x, int y, FontId font, const char *text, Color color = Color::Black, int letter_space = 0);
void draw_text_centered(int cx, int y, FontId font, const char *text, Color color = Color::Black,
                        int letter_space = 0);
/* Word-wrapped into w px, at most max_lines, the last ending in "..." if
 * there's more. Returns the lines drawn. */
int draw_wrapped(int x, int y, int w, FontId font, const char *text, int max_lines, int line_space,
                 bool center, Color color = Color::Black);
int wrapped_lines(int w, FontId font, const char *text);
/* LVGL's LV_SYMBOL_* (lv_symbol_def.h): FontAwesome codepoints, drawn in
 * the Montserrat slots' chain as LVGL's own fonts draw them. */
#define LV_SYMBOL_WIFI "\xEF\x87\xAB"
#define LV_SYMBOL_BLUETOOTH "\xEF\x8A\x93"
#define LV_SYMBOL_OK "\xEF\x80\x8C"
#define LV_SYMBOL_LEFT "\xEF\x81\x93"
#define LV_SYMBOL_CHARGE "\xEF\x83\xA7"
#define LV_SYMBOL_EYE_CLOSE "\xEF\x81\xB0"
#define LV_SYMBOL_POWER "\xEF\x80\x91"
#define LV_SYMBOL_REFRESH "\xEF\x80\xA1"
#define LV_SYMBOL_EDIT "\xEF\x8C\x84"
#define LV_SYMBOL_HOME "\xEF\x80\x95"
#define LV_SYMBOL_BATTERY_FULL "\xEF\x89\x80"
#define LV_SYMBOL_VOLUME_MAX "\xEF\x80\xA8"

/* The fonts (muse_eink_fonts.cpp), into the target's slots. */
bool fonts_init(freeink::ui::DisplayTarget &target);

/* Settings (muse_eink_settings.cpp), drawn in place of the face. */
void settings_setup(App &app);
void settings_screen(Screen &screen);
bool settings_in_subpage(void);
bool settings_back(void);           /* false at its home page */
void settings_home(void);
bool settings_tick(void);           /* true when its page needs drawing again */
bool settings_typing(void);         /* the keyboard is up: swipes don't leave */
bool settings_keyboard_changed(void); /* rebuild key IDs before draining more input */
void settings_scroll(int dy);       /* a vertical swipe of dy px */

/* From a handler: draw again as soon as the current refresh is done. */
void request_draw(void);
/* Fence old-page touches until the replacement page is presented. */
void navigation_changed(void);
void request_face(void);

}  // namespace muse_eink
