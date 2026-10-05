/*
 * 中文本地化：文案表 + 点阵中文字体。
 */
#pragma once

#include "lvgl.h"

/* 点阵中文字体（Fusion Pixel 12px 1bpp），由 tools/make-zh-font.sh 生成 */
extern const lv_font_t zh_font_12;
extern const lv_font_t zh_font_24;

/* 界面统一用点阵字：正文 12px、大字幕 24px（2 倍整数放大，像素不糊）。
 * 两者都带 fallback，所以标签里的 LV_SYMBOL_* 图标会自动回退到 montserrat。 */
#define UI_FONT_TEXT (&zh_font_12)
#define UI_FONT_BIG  (&zh_font_24)

/* 英文原文 -> 中文；未命中回退英文 */
const char *tr(const char *en);

/* 图标 + 文案（"ICON  text" / "text ICON"），返回静态缓冲 */
const char *tr_icon(const char *icon, const char *en);
const char *tr_icon_tail(const char *en, const char *icon);
