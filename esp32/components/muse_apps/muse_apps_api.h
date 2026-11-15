/*
 * muse_apps_api.h: the firmware's side of the apps API 1.1.
 * Generated from components/muse_apps/sdk/api/api.json by gen_api.py. Don't edit it.
 *
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

#define MUSE_APPS_API_MAJOR 1
#define MUSE_APPS_API_MINOR 1

/* Every import: GADGET_FN(name, WAMR signature, permission or NULL). */
#define MUSE_APPS_FUNCTIONS(GADGET_FN) \
    GADGET_FN(sys_millis, "()i", NULL) \
    GADGET_FN(sys_time, "()I", NULL) \
    GADGET_FN(sys_log, "($)", NULL) \
    GADGET_FN(sys_info, "(i)i", NULL) \
    GADGET_FN(sys_random, "()i", NULL) \
    GADGET_FN(sys_timer, "(iii)i", NULL) \
    GADGET_FN(sys_timer_stop, "(i)i", NULL) \
    GADGET_FN(sys_frame_rate, "(i)i", NULL) \
    GADGET_FN(sys_keep_awake, "(i)", NULL) \
    GADGET_FN(sys_exit, "()", NULL) \
    GADGET_FN(sys_brightness, "(i)i", NULL) \
    GADGET_FN(ui_create, "(ii)i", NULL) \
    GADGET_FN(ui_delete, "(i)i", NULL) \
    GADGET_FN(ui_clean, "(i)i", NULL) \
    GADGET_FN(ui_set, "(iii)i", NULL) \
    GADGET_FN(ui_get, "(ii)i", NULL) \
    GADGET_FN(ui_set_text, "(ii$)i", NULL) \
    GADGET_FN(ui_get_text, "(ii*~)i", NULL) \
    GADGET_FN(ui_on, "(ii)i", NULL) \
    GADGET_FN(ui_align, "(iiii)i", NULL) \
    GADGET_FN(ui_align_to, "(iiiii)i", NULL) \
    GADGET_FN(ui_set_parent, "(ii)i", NULL) \
    GADGET_FN(ui_grid, "(i$$)i", NULL) \
    GADGET_FN(ui_grid_cell, "(iiiii)i", NULL) \
    GADGET_FN(ui_scroll_to, "(iiii)i", NULL) \
    GADGET_FN(ui_scroll_to_view, "(ii)i", NULL) \
    GADGET_FN(ui_navigation, "(i)i", NULL) \
    GADGET_FN(ui_text_width, "(i$)i", NULL) \
    GADGET_FN(ui_text_height, "(ii$)i", NULL) \
    GADGET_FN(ui_span, "(ii)i", NULL) \
    GADGET_FN(ui_fit_text, "(iii)i", NULL) \
    GADGET_FN(ui_image_load, "(i$)i", NULL) \
    GADGET_FN(ui_image_set, "(i*~)i", NULL) \
    GADGET_FN(ui_line_points, "(i*~)i", NULL) \
    GADGET_FN(ui_chart_series, "(ii)i", NULL) \
    GADGET_FN(ui_chart_set, "(ii*~)i", NULL) \
    GADGET_FN(ui_chart_push, "(iii)i", NULL) \
    GADGET_FN(ui_table_cell, "(iii$)i", NULL) \
    GADGET_FN(ui_table_column_width, "(iii)i", NULL) \
    GADGET_FN(ui_scale_needle, "(iiii)i", NULL) \
    GADGET_FN(ui_style, "(iiii)i", NULL) \
    GADGET_FN(ui_style_create, "()i", NULL) \
    GADGET_FN(ui_style_set, "(iii)i", NULL) \
    GADGET_FN(ui_style_add, "(iii)i", NULL) \
    GADGET_FN(ui_animate, "(iiiii)i", NULL) \
    GADGET_FN(ui_animate_stop, "(ii)i", NULL) \
    GADGET_FN(cv_fill, "(ii)i", NULL) \
    GADGET_FN(cv_opa, "(ii)i", NULL) \
    GADGET_FN(cv_rect, "(iiiiiii)i", NULL) \
    GADGET_FN(cv_rect_style, "(iiiiii)i", NULL) \
    GADGET_FN(cv_border, "(iiiiiiii)i", NULL) \
    GADGET_FN(cv_line, "(iiiiiii)i", NULL) \
    GADGET_FN(cv_arc, "(iiiiiiii)i", NULL) \
    GADGET_FN(cv_triangle, "(iiiiiiii)i", NULL) \
    GADGET_FN(cv_text, "(iiiiiii$)i", NULL) \
    GADGET_FN(cv_pixels, "(iiiii*~)i", NULL) \
    GADGET_FN(muse_mode, "()i", NULL) \
    GADGET_FN(muse_caption, "(*~)i", NULL) \
    GADGET_FN(muse_talk_key, "(i)", NULL) \
    GADGET_FN(chat_open, "()i", "chat.read") \
    GADGET_FN(chat_close, "()", "chat.read") \
    GADGET_FN(chat_latest, "(i)i", "chat.read") \
    GADGET_FN(chat_info, "(ii)i", "chat.read") \
    GADGET_FN(chat_text, "(ii*~)i", "chat.read") \
    GADGET_FN(chat_send, "($)i", "chat.send") \
    GADGET_FN(file_read, "($i*~)i", NULL) \
    GADGET_FN(state_save, "($*~)i", NULL) \
    GADGET_FN(state_load, "($*~)i", NULL) \
    GADGET_FN(dev_count, "()i", NULL) \
    GADGET_FN(dev_name, "(i*~)i", NULL) \
    GADGET_FN(dev_find, "($)i", NULL) \
    GADGET_FN(dev_describe, "(i*~)i", NULL) \
    GADGET_FN(dev_field, "(i$)i", NULL) \
    GADGET_FN(dev_read, "(i*~)i", NULL) \
    GADGET_FN(dev_open, "(ii)i", NULL) \
    GADGET_FN(dev_take, "(i*~)i", NULL) \
    GADGET_FN(dev_close, "(i)i", NULL) \
    GADGET_FN(dev_action, "(i$)i", NULL) \
    GADGET_FN(dev_call, "(ii*~$)i", NULL) \
    GADGET_FN(dev_event, "(i$)i", NULL) \


/* The API's constants, as apps' gadget.h names them, with GADGET_ in front. */
#define GADGET_UI_ROOT 1
#define GADGET_SIZE_CONTENT 0x2FFFFFFF
#define GADGET_PCT(p) (0x20000000 + (p))
#define GADGET_RADIUS_CIRCLE 0x7FFF
#define GADGET_OPA_TRANSP 0
#define GADGET_OPA_COVER 255
#define GADGET_UI_INVALID (-0x7FFFFFFF - 1)
#define GADGET_EV_UI 1
#define GADGET_EV_BUTTON 2
#define GADGET_EV_KNOB 3
#define GADGET_EV_TOUCH_DOWN 4
#define GADGET_EV_TOUCH_MOVE 5
#define GADGET_EV_TOUCH_UP 6
#define GADGET_EV_GESTURE 7
#define GADGET_EV_TIMER 8
#define GADGET_EV_DONE 9
#define GADGET_EV_CHAT 10
#define GADGET_EV_MUSE 11
#define GADGET_EV_DEV_DATA 12
#define GADGET_EV_DEV_EVENT 13
#define GADGET_EV_SCREEN 14
#define GADGET_EV_CONNECTION 15
#define GADGET_ERR_ARG (-1)
#define GADGET_ERR_HANDLE (-2)
#define GADGET_ERR_NO_MEMORY (-3)
#define GADGET_ERR_LIMIT (-4)
#define GADGET_ERR_BUSY (-5)
#define GADGET_ERR_OFFLINE (-6)
#define GADGET_ERR_DENIED (-7)
#define GADGET_ERR_UNSUPPORTED (-8)
#define GADGET_ERR_TOO_LARGE (-9)
#define GADGET_ERR_NOT_FOUND (-10)
#define GADGET_ERR_TIMEOUT (-11)
#define GADGET_ERR_FAILED (-12)
#define GADGET_INFO_API_VERSION (1)
#define GADGET_INFO_SCREEN_WIDTH (2)
#define GADGET_INFO_SCREEN_HEIGHT (3)
#define GADGET_INFO_ROUND (4)
#define GADGET_INFO_SAFE_INSET (5)
#define GADGET_INFO_TOUCH (6)
#define GADGET_INFO_BUTTONS (7)
#define GADGET_INFO_KNOB (8)
#define GADGET_INFO_MEM_BUDGET (10)
#define GADGET_INFO_MEM_USED (11)
#define GADGET_INFO_CONNECTED (12)
#define GADGET_STOP_USER (1)
#define GADGET_STOP_REPLACED (2)
#define GADGET_STOP_CALL (3)
#define GADGET_STOP_VIDEO (4)
#define GADGET_STOP_EXIT (5)
#define GADGET_STOP_UPDATE (6)
#define GADGET_BUTTON_PRIMARY (1)
#define GADGET_BUTTON_SECONDARY (2)
#define GADGET_BUTTON_UP (3)
#define GADGET_BUTTON_DOWN (4)
#define GADGET_BUTTON_LEFT (5)
#define GADGET_BUTTON_RIGHT (6)
#define GADGET_BUTTON_ENTER (7)
#define GADGET_BUTTON_BACK (8)
#define GADGET_BTN_DOWN (1)
#define GADGET_BTN_UP (2)
#define GADGET_BTN_LONG (3)
#define GADGET_GESTURE_LEFT (1)
#define GADGET_GESTURE_RIGHT (2)
#define GADGET_GESTURE_UP (3)
#define GADGET_GESTURE_DOWN (4)
#define GADGET_MUSE_IDLE (0)
#define GADGET_MUSE_LISTENING (1)
#define GADGET_MUSE_THINKING (2)
#define GADGET_MUSE_SPEAKING (3)
#define GADGET_MUSE_ERROR (4)
#define GADGET_CHAT_MESSAGE (1)
#define GADGET_CHAT_TEXT (2)
#define GADGET_CHAT_DONE (3)
#define GADGET_CHAT_BUSY (4)
#define GADGET_CHAT_CONNECTED (5)
#define GADGET_CHAT_INFO_ROLE (1)
#define GADGET_CHAT_INFO_LENGTH (2)
#define GADGET_CHAT_INFO_DONE (3)
#define GADGET_CHAT_INFO_TRUNCATED (4)
#define GADGET_ROLE_USER (1)
#define GADGET_ROLE_MUSE (2)
#define GADGET_ROLE_APP (3)
#define GADGET_UI_CLICKED (1)
#define GADGET_UI_PRESSED (2)
#define GADGET_UI_RELEASED (4)
#define GADGET_UI_LONG_PRESSED (8)
#define GADGET_UI_VALUE_CHANGED (16)
#define GADGET_UI_FOCUSED (32)
#define GADGET_UI_DEFOCUSED (64)
#define GADGET_UI_READY (128)
#define GADGET_UI_CANCEL (256)
#define GADGET_UI_SCROLL_END (512)
#define GADGET_UI_ANIM_DONE (1024)
#define GADGET_UI_LOADED (2048)
#define GADGET_UI_LOAD_FAILED (4096)
#define GADGET_PART_MAIN (0)
#define GADGET_PART_SCROLLBAR (256)
#define GADGET_PART_INDICATOR (512)
#define GADGET_PART_KNOB (768)
#define GADGET_PART_SELECTED (1024)
#define GADGET_PART_ITEMS (1280)
#define GADGET_PART_CURSOR (1536)
#define GADGET_STATE_DEFAULT (0)
#define GADGET_STATE_CHECKED (1)
#define GADGET_STATE_FOCUSED (2)
#define GADGET_STATE_PRESSED (4)
#define GADGET_STATE_DISABLED (8)
#define GADGET_STATE_EDITED (16)
#define GADGET_ALIGN_DEFAULT (0)
#define GADGET_ALIGN_TOP_LEFT (1)
#define GADGET_ALIGN_TOP_MID (2)
#define GADGET_ALIGN_TOP_RIGHT (3)
#define GADGET_ALIGN_BOTTOM_LEFT (4)
#define GADGET_ALIGN_BOTTOM_MID (5)
#define GADGET_ALIGN_BOTTOM_RIGHT (6)
#define GADGET_ALIGN_LEFT_MID (7)
#define GADGET_ALIGN_RIGHT_MID (8)
#define GADGET_ALIGN_CENTER (9)
#define GADGET_ALIGN_OUT_TOP_LEFT (10)
#define GADGET_ALIGN_OUT_TOP_MID (11)
#define GADGET_ALIGN_OUT_TOP_RIGHT (12)
#define GADGET_ALIGN_OUT_BOTTOM_LEFT (13)
#define GADGET_ALIGN_OUT_BOTTOM_MID (14)
#define GADGET_ALIGN_OUT_BOTTOM_RIGHT (15)
#define GADGET_ALIGN_OUT_LEFT_TOP (16)
#define GADGET_ALIGN_OUT_LEFT_MID (17)
#define GADGET_ALIGN_OUT_LEFT_BOTTOM (18)
#define GADGET_ALIGN_OUT_RIGHT_TOP (19)
#define GADGET_ALIGN_OUT_RIGHT_MID (20)
#define GADGET_ALIGN_OUT_RIGHT_BOTTOM (21)
#define GADGET_TEXT_ALIGN_AUTO (0)
#define GADGET_TEXT_ALIGN_LEFT (1)
#define GADGET_TEXT_ALIGN_CENTER (2)
#define GADGET_TEXT_ALIGN_RIGHT (3)
#define GADGET_FONT_DEFAULT (0)
#define GADGET_FONT_14 (1)
#define GADGET_FONT_16 (2)
#define GADGET_FONT_20 (3)
#define GADGET_FONT_28 (4)
#define GADGET_FONT_48 (5)
#define GADGET_LAYOUT_NONE (0)
#define GADGET_LAYOUT_FLEX (1)
#define GADGET_LAYOUT_GRID (2)
#define GADGET_FLEX_ROW (0)
#define GADGET_FLEX_COLUMN (1)
#define GADGET_FLEX_ROW_WRAP (2)
#define GADGET_FLEX_COLUMN_WRAP (3)
#define GADGET_FLEX_ROW_REVERSE (4)
#define GADGET_FLEX_COLUMN_REVERSE (5)
#define GADGET_PLACE_START (0)
#define GADGET_PLACE_END (1)
#define GADGET_PLACE_CENTER (2)
#define GADGET_PLACE_SPACE_EVENLY (3)
#define GADGET_PLACE_SPACE_AROUND (4)
#define GADGET_PLACE_SPACE_BETWEEN (5)
#define GADGET_DIR_NONE (0)
#define GADGET_DIR_LEFT (1)
#define GADGET_DIR_RIGHT (2)
#define GADGET_DIR_TOP (4)
#define GADGET_DIR_BOTTOM (8)
#define GADGET_DIR_HOR (3)
#define GADGET_DIR_VER (12)
#define GADGET_DIR_ALL (15)
#define GADGET_SCROLLBAR_OFF (0)
#define GADGET_SCROLLBAR_ON (1)
#define GADGET_SCROLLBAR_ACTIVE (2)
#define GADGET_SCROLLBAR_AUTO (3)
#define GADGET_SNAP_NONE (0)
#define GADGET_SNAP_START (1)
#define GADGET_SNAP_END (2)
#define GADGET_SNAP_CENTER (3)
#define GADGET_LONG_WRAP (0)
#define GADGET_LONG_DOTS (1)
#define GADGET_LONG_SCROLL (2)
#define GADGET_LONG_SCROLL_CIRCULAR (3)
#define GADGET_LONG_CLIP (4)
#define GADGET_GRAD_NONE (0)
#define GADGET_GRAD_VER (1)
#define GADGET_GRAD_HOR (2)
#define GADGET_DECOR_NONE (0)
#define GADGET_DECOR_UNDERLINE (1)
#define GADGET_DECOR_STRIKETHROUGH (2)
#define GADGET_BORDER_NONE (0)
#define GADGET_BORDER_BOTTOM (1)
#define GADGET_BORDER_TOP (2)
#define GADGET_BORDER_LEFT (4)
#define GADGET_BORDER_RIGHT (8)
#define GADGET_BORDER_FULL (15)
#define GADGET_ARC_NORMAL (0)
#define GADGET_ARC_SYMMETRICAL (1)
#define GADGET_ARC_REVERSE (2)
#define GADGET_BAR_NORMAL (0)
#define GADGET_BAR_SYMMETRICAL (1)
#define GADGET_BAR_RANGE (2)
#define GADGET_ORIENT_AUTO (0)
#define GADGET_ORIENT_HORIZONTAL (1)
#define GADGET_ORIENT_VERTICAL (2)
#define GADGET_KB_TEXT_LOWER (0)
#define GADGET_KB_TEXT_UPPER (1)
#define GADGET_KB_SPECIAL (2)
#define GADGET_KB_NUMBER (3)
#define GADGET_SCALE_HORIZONTAL_TOP (0)
#define GADGET_SCALE_HORIZONTAL_BOTTOM (1)
#define GADGET_SCALE_VERTICAL_LEFT (2)
#define GADGET_SCALE_VERTICAL_RIGHT (3)
#define GADGET_SCALE_ROUND_INNER (4)
#define GADGET_SCALE_ROUND_OUTER (5)
#define GADGET_CHART_NONE (0)
#define GADGET_CHART_LINE (1)
#define GADGET_CHART_BAR (2)
#define GADGET_UPDATE_SHIFT (0)
#define GADGET_UPDATE_CIRCULAR (1)
#define GADGET_IMAGE_CENTER (0)
#define GADGET_IMAGE_TOP_LEFT (1)
#define GADGET_IMAGE_STRETCH (2)
#define GADGET_IMAGE_TILE (3)
#define GADGET_IMAGE_CONTAIN (4)
#define GADGET_IMAGE_COVER (5)
#define GADGET_EASE_LINEAR (0)
#define GADGET_EASE_IN (1)
#define GADGET_EASE_OUT (2)
#define GADGET_EASE_IN_OUT (3)
#define GADGET_EASE_OVERSHOOT (4)
#define GADGET_EASE_BOUNCE (5)
#define GADGET_EASE_STEP (6)
#define GADGET_ANIM_REPEAT (256)
#define GADGET_ANIM_REVERSE (512)
#define GADGET_W_OBJ 1
#define GADGET_W_LABEL 2
#define GADGET_W_BUTTON 3
#define GADGET_W_IMAGE 4
#define GADGET_W_LINE 5
#define GADGET_W_ARC 6
#define GADGET_W_BAR 7
#define GADGET_W_SLIDER 8
#define GADGET_W_SWITCH 9
#define GADGET_W_CHECKBOX 10
#define GADGET_W_ROLLER 11
#define GADGET_W_DROPDOWN 12
#define GADGET_W_TEXTAREA 13
#define GADGET_W_KEYBOARD 14
#define GADGET_W_SCALE 15
#define GADGET_W_SPINNER 16
#define GADGET_W_CHART 17
#define GADGET_W_TABLE 18
#define GADGET_W_CANVAS 19
#define GADGET_P_X 1
#define GADGET_P_Y 2
#define GADGET_P_WIDTH 3
#define GADGET_P_HEIGHT 4
#define GADGET_P_MIN_WIDTH 5
#define GADGET_P_MAX_WIDTH 6
#define GADGET_P_MIN_HEIGHT 7
#define GADGET_P_MAX_HEIGHT 8
#define GADGET_P_ALIGN 9
#define GADGET_P_HIDDEN 10
#define GADGET_P_CLICKABLE 11
#define GADGET_P_SCROLLABLE 12
#define GADGET_P_CHECKABLE 13
#define GADGET_P_EVENT_BUBBLE 14
#define GADGET_P_FLOATING 15
#define GADGET_P_CHECKED 16
#define GADGET_P_DISABLED 17
#define GADGET_P_SCROLL_DIR 18
#define GADGET_P_SCROLLBAR_MODE 19
#define GADGET_P_SCROLL_SNAP_X 20
#define GADGET_P_SCROLL_SNAP_Y 21
#define GADGET_P_SCROLL_X 22
#define GADGET_P_SCROLL_Y 23
#define GADGET_P_INDEX 24
#define GADGET_P_CHILD_COUNT 25
#define GADGET_P_LAYOUT 26
#define GADGET_P_FLEX_FLOW 27
#define GADGET_P_FLEX_MAIN_PLACE 28
#define GADGET_P_FLEX_CROSS_PLACE 29
#define GADGET_P_FLEX_TRACK_PLACE 30
#define GADGET_P_FLEX_GROW 31
#define GADGET_P_BG_COLOR 100
#define GADGET_P_BG_OPA 101
#define GADGET_P_BG_GRAD_COLOR 102
#define GADGET_P_BG_GRAD_DIR 103
#define GADGET_P_BORDER_COLOR 104
#define GADGET_P_BORDER_WIDTH 105
#define GADGET_P_BORDER_OPA 106
#define GADGET_P_BORDER_SIDE 107
#define GADGET_P_OUTLINE_COLOR 108
#define GADGET_P_OUTLINE_WIDTH 109
#define GADGET_P_OUTLINE_PAD 110
#define GADGET_P_SHADOW_COLOR 111
#define GADGET_P_SHADOW_WIDTH 112
#define GADGET_P_SHADOW_OPA 113
#define GADGET_P_SHADOW_OFFSET_X 114
#define GADGET_P_SHADOW_OFFSET_Y 115
#define GADGET_P_SHADOW_SPREAD 116
#define GADGET_P_RADIUS 117
#define GADGET_P_PAD_ALL 118
#define GADGET_P_PAD_LEFT 119
#define GADGET_P_PAD_RIGHT 120
#define GADGET_P_PAD_TOP 121
#define GADGET_P_PAD_BOTTOM 122
#define GADGET_P_PAD_ROW 123
#define GADGET_P_PAD_COLUMN 124
#define GADGET_P_TEXT_COLOR 125
#define GADGET_P_TEXT_OPA 126
#define GADGET_P_TEXT_FONT 127
#define GADGET_P_TEXT_ALIGN 128
#define GADGET_P_TEXT_LETTER_SPACE 129
#define GADGET_P_TEXT_LINE_SPACE 130
#define GADGET_P_TEXT_DECOR 131
#define GADGET_P_LINE_COLOR 132
#define GADGET_P_LINE_WIDTH 133
#define GADGET_P_LINE_OPA 134
#define GADGET_P_LINE_ROUNDED 135
#define GADGET_P_LINE_DASH_WIDTH 136
#define GADGET_P_LINE_DASH_GAP 137
#define GADGET_P_ARC_COLOR 138
#define GADGET_P_ARC_WIDTH 139
#define GADGET_P_ARC_OPA 140
#define GADGET_P_ARC_ROUNDED 141
#define GADGET_P_IMAGE_OPA 142
#define GADGET_P_IMAGE_RECOLOR 143
#define GADGET_P_IMAGE_RECOLOR_OPA 144
#define GADGET_P_OPA 145
#define GADGET_P_TRANSFORM_ROTATION 146
#define GADGET_P_TRANSFORM_SCALE 147
#define GADGET_P_TRANSFORM_PIVOT_X 148
#define GADGET_P_TRANSFORM_PIVOT_Y 149
#define GADGET_P_TRANSLATE_X 150
#define GADGET_P_TRANSLATE_Y 151
#define GADGET_P_LENGTH 152
#define GADGET_P_CLIP_CORNER 153
#define GADGET_P_ANIM_DURATION 154
#define GADGET_P_TEXT 200
#define GADGET_P_LONG_MODE 201
#define GADGET_P_VALUE 202
#define GADGET_P_MIN_VALUE 203
#define GADGET_P_MAX_VALUE 204
#define GADGET_P_START_VALUE 205
#define GADGET_P_BAR_MODE 206
#define GADGET_P_ORIENTATION 207
#define GADGET_P_ARC_MODE 208
#define GADGET_P_START_ANGLE 209
#define GADGET_P_END_ANGLE 210
#define GADGET_P_BG_START_ANGLE 211
#define GADGET_P_BG_END_ANGLE 212
#define GADGET_P_ROTATION 213
#define GADGET_P_OPTIONS 214
#define GADGET_P_SELECTED 215
#define GADGET_P_VISIBLE_ROWS 216
#define GADGET_P_INFINITE 217
#define GADGET_P_LIST_DIR 218
#define GADGET_P_PLACEHOLDER 219
#define GADGET_P_ONE_LINE 220
#define GADGET_P_PASSWORD 221
#define GADGET_P_MAX_LENGTH 222
#define GADGET_P_ACCEPTED_CHARS 223
#define GADGET_P_CURSOR_POS 224
#define GADGET_P_TEXTAREA 225
#define GADGET_P_KEYBOARD_MODE 226
#define GADGET_P_SCALE_MODE 227
#define GADGET_P_TOTAL_TICKS 228
#define GADGET_P_MAJOR_TICK_EVERY 229
#define GADGET_P_LABEL_SHOW 230
#define GADGET_P_ANGLE_RANGE 231
#define GADGET_P_SPIN_TIME 232
#define GADGET_P_ARC_SWEEP 233
#define GADGET_P_CHART_TYPE 234
#define GADGET_P_POINT_COUNT 235
#define GADGET_P_UPDATE_MODE 236
#define GADGET_P_HOR_DIV_LINES 237
#define GADGET_P_VER_DIV_LINES 238
#define GADGET_P_ROW_COUNT 239
#define GADGET_P_COLUMN_COUNT 240
#define GADGET_P_IMAGE_SCALE 241
#define GADGET_P_OFFSET_X 242
#define GADGET_P_OFFSET_Y 243
#define GADGET_P_INNER_ALIGN 244
#define GADGET_P_Y_INVERT 245
#define GADGET_DEV_TEMPERATURE "temperature"
#define GADGET_DEV_TEMPERATURE_C 0
#define GADGET_DEV_HUMIDITY "humidity"
#define GADGET_DEV_HUMIDITY_RH 0
#define GADGET_DEV_PRESSURE "pressure"
#define GADGET_DEV_PRESSURE_HPA 0
#define GADGET_DEV_CO2 "co2"
#define GADGET_DEV_CO2_PPM 0
#define GADGET_DEV_VOC "voc"
#define GADGET_DEV_VOC_INDEX 0
#define GADGET_DEV_LIGHT "light"
#define GADGET_DEV_LIGHT_LUX 0
#define GADGET_DEV_ACCEL "accel"
#define GADGET_DEV_ACCEL_X 0
#define GADGET_DEV_ACCEL_Y 1
#define GADGET_DEV_ACCEL_Z 2
#define GADGET_DEV_ACCEL_TAP 0
#define GADGET_DEV_ACCEL_DOUBLE_TAP 1
#define GADGET_DEV_ACCEL_SHAKE 2
#define GADGET_DEV_ACCEL_FACE_UP 3
#define GADGET_DEV_ACCEL_FACE_DOWN 4
#define GADGET_DEV_GYRO "gyro"
#define GADGET_DEV_GYRO_X 0
#define GADGET_DEV_GYRO_Y 1
#define GADGET_DEV_GYRO_Z 2
#define GADGET_DEV_STEPS "steps"
#define GADGET_DEV_STEPS_COUNT 0
#define GADGET_DEV_BATTERY "battery"
#define GADGET_DEV_BATTERY_PERCENT 0
#define GADGET_DEV_BATTERY_VOLTS 1
#define GADGET_DEV_BATTERY_CHARGING 2
#define GADGET_DEV_BATTERY_USB 3
#define GADGET_DEV_VIBRATION "vibration"
#define GADGET_DEV_VIBRATION_BUZZ 0
#define GADGET_DEV_SPEAKER "speaker"
#define GADGET_DEV_SPEAKER_TONE 0

#ifdef MUSE_APPS_NATIVES
#include "wasm_export.h"

/* Each import's implementation: gadget_<name>, in the module for its group. */
int32_t gadget_sys_millis(wasm_exec_env_t env);
int64_t gadget_sys_time(wasm_exec_env_t env);
void gadget_sys_log(wasm_exec_env_t env, const char *text);
int32_t gadget_sys_info(wasm_exec_env_t env, int32_t key);
int32_t gadget_sys_random(wasm_exec_env_t env);
int32_t gadget_sys_timer(wasm_exec_env_t env, int32_t id, int32_t ms, int32_t repeat);
int32_t gadget_sys_timer_stop(wasm_exec_env_t env, int32_t id);
int32_t gadget_sys_frame_rate(wasm_exec_env_t env, int32_t fps);
void gadget_sys_keep_awake(wasm_exec_env_t env, int32_t on);
void gadget_sys_exit(wasm_exec_env_t env);
int32_t gadget_sys_brightness(wasm_exec_env_t env, int32_t percent);
int32_t gadget_ui_create(wasm_exec_env_t env, int32_t type, int32_t parent);
int32_t gadget_ui_delete(wasm_exec_env_t env, int32_t obj);
int32_t gadget_ui_clean(wasm_exec_env_t env, int32_t obj);
int32_t gadget_ui_set(wasm_exec_env_t env, int32_t obj, int32_t prop, int32_t value);
int32_t gadget_ui_get(wasm_exec_env_t env, int32_t obj, int32_t prop);
int32_t gadget_ui_set_text(wasm_exec_env_t env, int32_t obj, int32_t prop, const char *text);
int32_t gadget_ui_get_text(wasm_exec_env_t env, int32_t obj, int32_t prop, void *buf, uint32_t buf_size);
int32_t gadget_ui_on(wasm_exec_env_t env, int32_t obj, int32_t events);
int32_t gadget_ui_align(wasm_exec_env_t env, int32_t obj, int32_t align, int32_t dx, int32_t dy);
int32_t gadget_ui_align_to(wasm_exec_env_t env, int32_t obj, int32_t base, int32_t align, int32_t dx, int32_t dy);
int32_t gadget_ui_set_parent(wasm_exec_env_t env, int32_t obj, int32_t parent);
int32_t gadget_ui_grid(wasm_exec_env_t env, int32_t obj, const char *columns, const char *rows);
int32_t gadget_ui_grid_cell(wasm_exec_env_t env, int32_t obj, int32_t col, int32_t col_span, int32_t row, int32_t row_span);
int32_t gadget_ui_scroll_to(wasm_exec_env_t env, int32_t obj, int32_t x, int32_t y, int32_t animate);
int32_t gadget_ui_scroll_to_view(wasm_exec_env_t env, int32_t obj, int32_t animate);
int32_t gadget_ui_navigation(wasm_exec_env_t env, int32_t on);
int32_t gadget_ui_text_width(wasm_exec_env_t env, int32_t font, const char *text);
int32_t gadget_ui_text_height(wasm_exec_env_t env, int32_t font, int32_t width, const char *text);
int32_t gadget_ui_span(wasm_exec_env_t env, int32_t y, int32_t margin);
int32_t gadget_ui_fit_text(wasm_exec_env_t env, int32_t obj, int32_t width, int32_t font);
int32_t gadget_ui_image_load(wasm_exec_env_t env, int32_t obj, const char *path);
int32_t gadget_ui_image_set(wasm_exec_env_t env, int32_t obj, const void *data, uint32_t data_len);
int32_t gadget_ui_line_points(wasm_exec_env_t env, int32_t obj, const void *xy, uint32_t xy_len);
int32_t gadget_ui_chart_series(wasm_exec_env_t env, int32_t chart, int32_t color);
int32_t gadget_ui_chart_set(wasm_exec_env_t env, int32_t chart, int32_t series, const void *values, uint32_t values_len);
int32_t gadget_ui_chart_push(wasm_exec_env_t env, int32_t chart, int32_t series, int32_t value);
int32_t gadget_ui_table_cell(wasm_exec_env_t env, int32_t table, int32_t row, int32_t col, const char *text);
int32_t gadget_ui_table_column_width(wasm_exec_env_t env, int32_t table, int32_t col, int32_t width);
int32_t gadget_ui_scale_needle(wasm_exec_env_t env, int32_t scale, int32_t line, int32_t length, int32_t value);
int32_t gadget_ui_style(wasm_exec_env_t env, int32_t obj, int32_t selector, int32_t prop, int32_t value);
int32_t gadget_ui_style_create(wasm_exec_env_t env);
int32_t gadget_ui_style_set(wasm_exec_env_t env, int32_t style, int32_t prop, int32_t value);
int32_t gadget_ui_style_add(wasm_exec_env_t env, int32_t obj, int32_t style, int32_t selector);
int32_t gadget_ui_animate(wasm_exec_env_t env, int32_t obj, int32_t prop, int32_t to, int32_t ms, int32_t easing);
int32_t gadget_ui_animate_stop(wasm_exec_env_t env, int32_t obj, int32_t prop);
int32_t gadget_cv_fill(wasm_exec_env_t env, int32_t canvas, int32_t color);
int32_t gadget_cv_opa(wasm_exec_env_t env, int32_t canvas, int32_t opa);
int32_t gadget_cv_rect(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t color);
int32_t gadget_cv_rect_style(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t style);
int32_t gadget_cv_border(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width, int32_t color);
int32_t gadget_cv_line(wasm_exec_env_t env, int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, int32_t color);
int32_t gadget_cv_arc(wasm_exec_env_t env, int32_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t width, int32_t start, int32_t end, int32_t color);
int32_t gadget_cv_triangle(wasm_exec_env_t env, int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t color);
int32_t gadget_cv_text(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t font, int32_t color, int32_t align, const char *text);
int32_t gadget_cv_pixels(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, const void *data, uint32_t data_len);
int32_t gadget_muse_mode(wasm_exec_env_t env);
int32_t gadget_muse_caption(wasm_exec_env_t env, void *buf, uint32_t buf_size);
void gadget_muse_talk_key(wasm_exec_env_t env, int32_t on);
int32_t gadget_chat_open(wasm_exec_env_t env);
void gadget_chat_close(wasm_exec_env_t env);
int32_t gadget_chat_latest(wasm_exec_env_t env, int32_t n);
int32_t gadget_chat_info(wasm_exec_env_t env, int32_t id, int32_t key);
int32_t gadget_chat_text(wasm_exec_env_t env, int32_t id, int32_t offset, void *buf, uint32_t buf_size);
int32_t gadget_chat_send(wasm_exec_env_t env, const char *text);
int32_t gadget_file_read(wasm_exec_env_t env, const char *path, int32_t offset, void *buf, uint32_t buf_size);
int32_t gadget_state_save(wasm_exec_env_t env, const char *name, const void *data, uint32_t data_len);
int32_t gadget_state_load(wasm_exec_env_t env, const char *name, void *buf, uint32_t buf_size);
int32_t gadget_dev_count(wasm_exec_env_t env);
int32_t gadget_dev_name(wasm_exec_env_t env, int32_t index, void *buf, uint32_t buf_size);
int32_t gadget_dev_find(wasm_exec_env_t env, const char *name);
int32_t gadget_dev_describe(wasm_exec_env_t env, int32_t device, void *buf, uint32_t buf_size);
int32_t gadget_dev_field(wasm_exec_env_t env, int32_t device, const char *name);
int32_t gadget_dev_read(wasm_exec_env_t env, int32_t device, void *reading, uint32_t reading_size);
int32_t gadget_dev_open(wasm_exec_env_t env, int32_t device, int32_t rate_hz);
int32_t gadget_dev_take(wasm_exec_env_t env, int32_t device, void *readings, uint32_t readings_size);
int32_t gadget_dev_close(wasm_exec_env_t env, int32_t device);
int32_t gadget_dev_action(wasm_exec_env_t env, int32_t device, const char *name);
int32_t gadget_dev_call(wasm_exec_env_t env, int32_t device, int32_t action, const void *params, uint32_t params_len, const char *text);
int32_t gadget_dev_event(wasm_exec_env_t env, int32_t device, const char *name);
#endif

#ifdef MUSE_APPS_DEFINE_STUBS
/* Weak defaults: a function no module implements yet answers ERR_UNSUPPORTED
 * (or nothing). The component links whole, so a module's own definition wins. */
__attribute__((weak)) int32_t gadget_sys_millis(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) int64_t gadget_sys_time(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) void gadget_sys_log(wasm_exec_env_t env, const char *text) { (void)env; (void)text; }
__attribute__((weak)) int32_t gadget_sys_info(wasm_exec_env_t env, int32_t key) { (void)env; (void)key; return -8; }
__attribute__((weak)) int32_t gadget_sys_random(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) int32_t gadget_sys_timer(wasm_exec_env_t env, int32_t id, int32_t ms, int32_t repeat) { (void)env; (void)id; (void)ms; (void)repeat; return -8; }
__attribute__((weak)) int32_t gadget_sys_timer_stop(wasm_exec_env_t env, int32_t id) { (void)env; (void)id; return -8; }
__attribute__((weak)) int32_t gadget_sys_frame_rate(wasm_exec_env_t env, int32_t fps) { (void)env; (void)fps; return -8; }
__attribute__((weak)) void gadget_sys_keep_awake(wasm_exec_env_t env, int32_t on) { (void)env; (void)on; }
__attribute__((weak)) void gadget_sys_exit(wasm_exec_env_t env) { (void)env; }
__attribute__((weak)) int32_t gadget_sys_brightness(wasm_exec_env_t env, int32_t percent) { (void)env; (void)percent; return -8; }
__attribute__((weak)) int32_t gadget_ui_create(wasm_exec_env_t env, int32_t type, int32_t parent) { (void)env; (void)type; (void)parent; return -8; }
__attribute__((weak)) int32_t gadget_ui_delete(wasm_exec_env_t env, int32_t obj) { (void)env; (void)obj; return -8; }
__attribute__((weak)) int32_t gadget_ui_clean(wasm_exec_env_t env, int32_t obj) { (void)env; (void)obj; return -8; }
__attribute__((weak)) int32_t gadget_ui_set(wasm_exec_env_t env, int32_t obj, int32_t prop, int32_t value) { (void)env; (void)obj; (void)prop; (void)value; return -8; }
__attribute__((weak)) int32_t gadget_ui_get(wasm_exec_env_t env, int32_t obj, int32_t prop) { (void)env; (void)obj; (void)prop; return -8; }
__attribute__((weak)) int32_t gadget_ui_set_text(wasm_exec_env_t env, int32_t obj, int32_t prop, const char *text) { (void)env; (void)obj; (void)prop; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_ui_get_text(wasm_exec_env_t env, int32_t obj, int32_t prop, void *buf, uint32_t buf_size) { (void)env; (void)obj; (void)prop; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) int32_t gadget_ui_on(wasm_exec_env_t env, int32_t obj, int32_t events) { (void)env; (void)obj; (void)events; return -8; }
__attribute__((weak)) int32_t gadget_ui_align(wasm_exec_env_t env, int32_t obj, int32_t align, int32_t dx, int32_t dy) { (void)env; (void)obj; (void)align; (void)dx; (void)dy; return -8; }
__attribute__((weak)) int32_t gadget_ui_align_to(wasm_exec_env_t env, int32_t obj, int32_t base, int32_t align, int32_t dx, int32_t dy) { (void)env; (void)obj; (void)base; (void)align; (void)dx; (void)dy; return -8; }
__attribute__((weak)) int32_t gadget_ui_set_parent(wasm_exec_env_t env, int32_t obj, int32_t parent) { (void)env; (void)obj; (void)parent; return -8; }
__attribute__((weak)) int32_t gadget_ui_grid(wasm_exec_env_t env, int32_t obj, const char *columns, const char *rows) { (void)env; (void)obj; (void)columns; (void)rows; return -8; }
__attribute__((weak)) int32_t gadget_ui_grid_cell(wasm_exec_env_t env, int32_t obj, int32_t col, int32_t col_span, int32_t row, int32_t row_span) { (void)env; (void)obj; (void)col; (void)col_span; (void)row; (void)row_span; return -8; }
__attribute__((weak)) int32_t gadget_ui_scroll_to(wasm_exec_env_t env, int32_t obj, int32_t x, int32_t y, int32_t animate) { (void)env; (void)obj; (void)x; (void)y; (void)animate; return -8; }
__attribute__((weak)) int32_t gadget_ui_scroll_to_view(wasm_exec_env_t env, int32_t obj, int32_t animate) { (void)env; (void)obj; (void)animate; return -8; }
__attribute__((weak)) int32_t gadget_ui_navigation(wasm_exec_env_t env, int32_t on) { (void)env; (void)on; return -8; }
__attribute__((weak)) int32_t gadget_ui_text_width(wasm_exec_env_t env, int32_t font, const char *text) { (void)env; (void)font; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_ui_text_height(wasm_exec_env_t env, int32_t font, int32_t width, const char *text) { (void)env; (void)font; (void)width; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_ui_span(wasm_exec_env_t env, int32_t y, int32_t margin) { (void)env; (void)y; (void)margin; return -8; }
__attribute__((weak)) int32_t gadget_ui_fit_text(wasm_exec_env_t env, int32_t obj, int32_t width, int32_t font) { (void)env; (void)obj; (void)width; (void)font; return -8; }
__attribute__((weak)) int32_t gadget_ui_image_load(wasm_exec_env_t env, int32_t obj, const char *path) { (void)env; (void)obj; (void)path; return -8; }
__attribute__((weak)) int32_t gadget_ui_image_set(wasm_exec_env_t env, int32_t obj, const void *data, uint32_t data_len) { (void)env; (void)obj; (void)data; (void)data_len; return -8; }
__attribute__((weak)) int32_t gadget_ui_line_points(wasm_exec_env_t env, int32_t obj, const void *xy, uint32_t xy_len) { (void)env; (void)obj; (void)xy; (void)xy_len; return -8; }
__attribute__((weak)) int32_t gadget_ui_chart_series(wasm_exec_env_t env, int32_t chart, int32_t color) { (void)env; (void)chart; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_ui_chart_set(wasm_exec_env_t env, int32_t chart, int32_t series, const void *values, uint32_t values_len) { (void)env; (void)chart; (void)series; (void)values; (void)values_len; return -8; }
__attribute__((weak)) int32_t gadget_ui_chart_push(wasm_exec_env_t env, int32_t chart, int32_t series, int32_t value) { (void)env; (void)chart; (void)series; (void)value; return -8; }
__attribute__((weak)) int32_t gadget_ui_table_cell(wasm_exec_env_t env, int32_t table, int32_t row, int32_t col, const char *text) { (void)env; (void)table; (void)row; (void)col; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_ui_table_column_width(wasm_exec_env_t env, int32_t table, int32_t col, int32_t width) { (void)env; (void)table; (void)col; (void)width; return -8; }
__attribute__((weak)) int32_t gadget_ui_scale_needle(wasm_exec_env_t env, int32_t scale, int32_t line, int32_t length, int32_t value) { (void)env; (void)scale; (void)line; (void)length; (void)value; return -8; }
__attribute__((weak)) int32_t gadget_ui_style(wasm_exec_env_t env, int32_t obj, int32_t selector, int32_t prop, int32_t value) { (void)env; (void)obj; (void)selector; (void)prop; (void)value; return -8; }
__attribute__((weak)) int32_t gadget_ui_style_create(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) int32_t gadget_ui_style_set(wasm_exec_env_t env, int32_t style, int32_t prop, int32_t value) { (void)env; (void)style; (void)prop; (void)value; return -8; }
__attribute__((weak)) int32_t gadget_ui_style_add(wasm_exec_env_t env, int32_t obj, int32_t style, int32_t selector) { (void)env; (void)obj; (void)style; (void)selector; return -8; }
__attribute__((weak)) int32_t gadget_ui_animate(wasm_exec_env_t env, int32_t obj, int32_t prop, int32_t to, int32_t ms, int32_t easing) { (void)env; (void)obj; (void)prop; (void)to; (void)ms; (void)easing; return -8; }
__attribute__((weak)) int32_t gadget_ui_animate_stop(wasm_exec_env_t env, int32_t obj, int32_t prop) { (void)env; (void)obj; (void)prop; return -8; }
__attribute__((weak)) int32_t gadget_cv_fill(wasm_exec_env_t env, int32_t canvas, int32_t color) { (void)env; (void)canvas; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_cv_opa(wasm_exec_env_t env, int32_t canvas, int32_t opa) { (void)env; (void)canvas; (void)opa; return -8; }
__attribute__((weak)) int32_t gadget_cv_rect(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t color) { (void)env; (void)canvas; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_cv_rect_style(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t style) { (void)env; (void)canvas; (void)x; (void)y; (void)w; (void)h; (void)style; return -8; }
__attribute__((weak)) int32_t gadget_cv_border(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width, int32_t color) { (void)env; (void)canvas; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)width; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_cv_line(wasm_exec_env_t env, int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, int32_t color) { (void)env; (void)canvas; (void)x1; (void)y1; (void)x2; (void)y2; (void)width; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_cv_arc(wasm_exec_env_t env, int32_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t width, int32_t start, int32_t end, int32_t color) { (void)env; (void)canvas; (void)cx; (void)cy; (void)r; (void)width; (void)start; (void)end; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_cv_triangle(wasm_exec_env_t env, int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t color) { (void)env; (void)canvas; (void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3; (void)color; return -8; }
__attribute__((weak)) int32_t gadget_cv_text(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t font, int32_t color, int32_t align, const char *text) { (void)env; (void)canvas; (void)x; (void)y; (void)w; (void)font; (void)color; (void)align; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_cv_pixels(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, const void *data, uint32_t data_len) { (void)env; (void)canvas; (void)x; (void)y; (void)w; (void)h; (void)data; (void)data_len; return -8; }
__attribute__((weak)) int32_t gadget_muse_mode(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) int32_t gadget_muse_caption(wasm_exec_env_t env, void *buf, uint32_t buf_size) { (void)env; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) void gadget_muse_talk_key(wasm_exec_env_t env, int32_t on) { (void)env; (void)on; }
__attribute__((weak)) int32_t gadget_chat_open(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) void gadget_chat_close(wasm_exec_env_t env) { (void)env; }
__attribute__((weak)) int32_t gadget_chat_latest(wasm_exec_env_t env, int32_t n) { (void)env; (void)n; return -8; }
__attribute__((weak)) int32_t gadget_chat_info(wasm_exec_env_t env, int32_t id, int32_t key) { (void)env; (void)id; (void)key; return -8; }
__attribute__((weak)) int32_t gadget_chat_text(wasm_exec_env_t env, int32_t id, int32_t offset, void *buf, uint32_t buf_size) { (void)env; (void)id; (void)offset; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) int32_t gadget_chat_send(wasm_exec_env_t env, const char *text) { (void)env; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_file_read(wasm_exec_env_t env, const char *path, int32_t offset, void *buf, uint32_t buf_size) { (void)env; (void)path; (void)offset; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) int32_t gadget_state_save(wasm_exec_env_t env, const char *name, const void *data, uint32_t data_len) { (void)env; (void)name; (void)data; (void)data_len; return -8; }
__attribute__((weak)) int32_t gadget_state_load(wasm_exec_env_t env, const char *name, void *buf, uint32_t buf_size) { (void)env; (void)name; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) int32_t gadget_dev_count(wasm_exec_env_t env) { (void)env; return -8; }
__attribute__((weak)) int32_t gadget_dev_name(wasm_exec_env_t env, int32_t index, void *buf, uint32_t buf_size) { (void)env; (void)index; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) int32_t gadget_dev_find(wasm_exec_env_t env, const char *name) { (void)env; (void)name; return -8; }
__attribute__((weak)) int32_t gadget_dev_describe(wasm_exec_env_t env, int32_t device, void *buf, uint32_t buf_size) { (void)env; (void)device; (void)buf; (void)buf_size; return -8; }
__attribute__((weak)) int32_t gadget_dev_field(wasm_exec_env_t env, int32_t device, const char *name) { (void)env; (void)device; (void)name; return -8; }
__attribute__((weak)) int32_t gadget_dev_read(wasm_exec_env_t env, int32_t device, void *reading, uint32_t reading_size) { (void)env; (void)device; (void)reading; (void)reading_size; return -8; }
__attribute__((weak)) int32_t gadget_dev_open(wasm_exec_env_t env, int32_t device, int32_t rate_hz) { (void)env; (void)device; (void)rate_hz; return -8; }
__attribute__((weak)) int32_t gadget_dev_take(wasm_exec_env_t env, int32_t device, void *readings, uint32_t readings_size) { (void)env; (void)device; (void)readings; (void)readings_size; return -8; }
__attribute__((weak)) int32_t gadget_dev_close(wasm_exec_env_t env, int32_t device) { (void)env; (void)device; return -8; }
__attribute__((weak)) int32_t gadget_dev_action(wasm_exec_env_t env, int32_t device, const char *name) { (void)env; (void)device; (void)name; return -8; }
__attribute__((weak)) int32_t gadget_dev_call(wasm_exec_env_t env, int32_t device, int32_t action, const void *params, uint32_t params_len, const char *text) { (void)env; (void)device; (void)action; (void)params; (void)params_len; (void)text; return -8; }
__attribute__((weak)) int32_t gadget_dev_event(wasm_exec_env_t env, int32_t device, const char *name) { (void)env; (void)device; (void)name; return -8; }
#endif


/* GADGET_WIDGET(id, name, LVGL class) */
#define MUSE_APPS_WIDGETS(GADGET_WIDGET) \
    GADGET_WIDGET(1, obj, lv_obj_class) \
    GADGET_WIDGET(2, label, lv_label_class) \
    GADGET_WIDGET(3, button, lv_button_class) \
    GADGET_WIDGET(4, image, lv_image_class) \
    GADGET_WIDGET(5, line, lv_line_class) \
    GADGET_WIDGET(6, arc, lv_arc_class) \
    GADGET_WIDGET(7, bar, lv_bar_class) \
    GADGET_WIDGET(8, slider, lv_slider_class) \
    GADGET_WIDGET(9, switch, lv_switch_class) \
    GADGET_WIDGET(10, checkbox, lv_checkbox_class) \
    GADGET_WIDGET(11, roller, lv_roller_class) \
    GADGET_WIDGET(12, dropdown, lv_dropdown_class) \
    GADGET_WIDGET(13, textarea, lv_textarea_class) \
    GADGET_WIDGET(14, keyboard, lv_keyboard_class) \
    GADGET_WIDGET(15, scale, lv_scale_class) \
    GADGET_WIDGET(16, spinner, lv_spinner_class) \
    GADGET_WIDGET(17, chart, lv_chart_class) \
    GADGET_WIDGET(18, table, lv_table_class) \
    GADGET_WIDGET(19, canvas, lv_canvas_class) \


/* GADGET_PROP(id, name, type, applies to: obj, style or widget, access) */
#define MUSE_APPS_PROPS(GADGET_PROP) \
    GADGET_PROP(1, x, coord, obj, rw) \
    GADGET_PROP(2, y, coord, obj, rw) \
    GADGET_PROP(3, width, coord, obj, rw) \
    GADGET_PROP(4, height, coord, obj, rw) \
    GADGET_PROP(5, min_width, coord, obj, rw) \
    GADGET_PROP(6, max_width, coord, obj, rw) \
    GADGET_PROP(7, min_height, coord, obj, rw) \
    GADGET_PROP(8, max_height, coord, obj, rw) \
    GADGET_PROP(9, align, align, obj, rw) \
    GADGET_PROP(10, hidden, bool, obj, rw) \
    GADGET_PROP(11, clickable, bool, obj, rw) \
    GADGET_PROP(12, scrollable, bool, obj, rw) \
    GADGET_PROP(13, checkable, bool, obj, rw) \
    GADGET_PROP(14, event_bubble, bool, obj, rw) \
    GADGET_PROP(15, floating, bool, obj, rw) \
    GADGET_PROP(16, checked, bool, obj, rw) \
    GADGET_PROP(17, disabled, bool, obj, rw) \
    GADGET_PROP(18, scroll_dir, dir, obj, rw) \
    GADGET_PROP(19, scrollbar_mode, scrollbar_mode, obj, rw) \
    GADGET_PROP(20, scroll_snap_x, snap, obj, rw) \
    GADGET_PROP(21, scroll_snap_y, snap, obj, rw) \
    GADGET_PROP(22, scroll_x, int, obj, r) \
    GADGET_PROP(23, scroll_y, int, obj, r) \
    GADGET_PROP(24, index, int, obj, rw) \
    GADGET_PROP(25, child_count, int, obj, r) \
    GADGET_PROP(26, layout, layout, obj, rw) \
    GADGET_PROP(27, flex_flow, flex_flow, obj, rw) \
    GADGET_PROP(28, flex_main_place, place, obj, rw) \
    GADGET_PROP(29, flex_cross_place, place, obj, rw) \
    GADGET_PROP(30, flex_track_place, place, obj, rw) \
    GADGET_PROP(31, flex_grow, int, obj, rw) \
    GADGET_PROP(100, bg_color, rgb, style, rw) \
    GADGET_PROP(101, bg_opa, opa, style, rw) \
    GADGET_PROP(102, bg_grad_color, rgb, style, rw) \
    GADGET_PROP(103, bg_grad_dir, grad_dir, style, rw) \
    GADGET_PROP(104, border_color, rgb, style, rw) \
    GADGET_PROP(105, border_width, int, style, rw) \
    GADGET_PROP(106, border_opa, opa, style, rw) \
    GADGET_PROP(107, border_side, border_side, style, rw) \
    GADGET_PROP(108, outline_color, rgb, style, rw) \
    GADGET_PROP(109, outline_width, int, style, rw) \
    GADGET_PROP(110, outline_pad, int, style, rw) \
    GADGET_PROP(111, shadow_color, rgb, style, rw) \
    GADGET_PROP(112, shadow_width, int, style, rw) \
    GADGET_PROP(113, shadow_opa, opa, style, rw) \
    GADGET_PROP(114, shadow_offset_x, int, style, rw) \
    GADGET_PROP(115, shadow_offset_y, int, style, rw) \
    GADGET_PROP(116, shadow_spread, int, style, rw) \
    GADGET_PROP(117, radius, int, style, rw) \
    GADGET_PROP(118, pad_all, int, style, w) \
    GADGET_PROP(119, pad_left, int, style, rw) \
    GADGET_PROP(120, pad_right, int, style, rw) \
    GADGET_PROP(121, pad_top, int, style, rw) \
    GADGET_PROP(122, pad_bottom, int, style, rw) \
    GADGET_PROP(123, pad_row, int, style, rw) \
    GADGET_PROP(124, pad_column, int, style, rw) \
    GADGET_PROP(125, text_color, rgb, style, rw) \
    GADGET_PROP(126, text_opa, opa, style, rw) \
    GADGET_PROP(127, text_font, font, style, rw) \
    GADGET_PROP(128, text_align, text_align, style, rw) \
    GADGET_PROP(129, text_letter_space, int, style, rw) \
    GADGET_PROP(130, text_line_space, int, style, rw) \
    GADGET_PROP(131, text_decor, text_decor, style, rw) \
    GADGET_PROP(132, line_color, rgb, style, rw) \
    GADGET_PROP(133, line_width, int, style, rw) \
    GADGET_PROP(134, line_opa, opa, style, rw) \
    GADGET_PROP(135, line_rounded, bool, style, rw) \
    GADGET_PROP(136, line_dash_width, int, style, rw) \
    GADGET_PROP(137, line_dash_gap, int, style, rw) \
    GADGET_PROP(138, arc_color, rgb, style, rw) \
    GADGET_PROP(139, arc_width, int, style, rw) \
    GADGET_PROP(140, arc_opa, opa, style, rw) \
    GADGET_PROP(141, arc_rounded, bool, style, rw) \
    GADGET_PROP(142, image_opa, opa, style, rw) \
    GADGET_PROP(143, image_recolor, rgb, style, rw) \
    GADGET_PROP(144, image_recolor_opa, opa, style, rw) \
    GADGET_PROP(145, opa, opa, style, rw) \
    GADGET_PROP(146, transform_rotation, angle, style, rw) \
    GADGET_PROP(147, transform_scale, percent, style, rw) \
    GADGET_PROP(148, transform_pivot_x, coord, style, rw) \
    GADGET_PROP(149, transform_pivot_y, coord, style, rw) \
    GADGET_PROP(150, translate_x, coord, style, rw) \
    GADGET_PROP(151, translate_y, coord, style, rw) \
    GADGET_PROP(152, length, int, style, rw) \
    GADGET_PROP(153, clip_corner, bool, style, rw) \
    GADGET_PROP(154, anim_duration, int, style, rw) \
    GADGET_PROP(200, text, text, widget, rw) \
    GADGET_PROP(201, long_mode, long_mode, widget, rw) \
    GADGET_PROP(202, value, int, widget, rw) \
    GADGET_PROP(203, min_value, int, widget, rw) \
    GADGET_PROP(204, max_value, int, widget, rw) \
    GADGET_PROP(205, start_value, int, widget, rw) \
    GADGET_PROP(206, bar_mode, bar_mode, widget, rw) \
    GADGET_PROP(207, orientation, orientation, widget, rw) \
    GADGET_PROP(208, arc_mode, arc_mode, widget, rw) \
    GADGET_PROP(209, start_angle, angle, widget, rw) \
    GADGET_PROP(210, end_angle, angle, widget, rw) \
    GADGET_PROP(211, bg_start_angle, angle, widget, rw) \
    GADGET_PROP(212, bg_end_angle, angle, widget, rw) \
    GADGET_PROP(213, rotation, angle, widget, rw) \
    GADGET_PROP(214, options, text, widget, rw) \
    GADGET_PROP(215, selected, int, widget, rw) \
    GADGET_PROP(216, visible_rows, int, widget, rw) \
    GADGET_PROP(217, infinite, bool, widget, rw) \
    GADGET_PROP(218, list_dir, dir, widget, rw) \
    GADGET_PROP(219, placeholder, text, widget, rw) \
    GADGET_PROP(220, one_line, bool, widget, rw) \
    GADGET_PROP(221, password, bool, widget, rw) \
    GADGET_PROP(222, max_length, int, widget, rw) \
    GADGET_PROP(223, accepted_chars, text, widget, rw) \
    GADGET_PROP(224, cursor_pos, int, widget, rw) \
    GADGET_PROP(225, textarea, handle, widget, rw) \
    GADGET_PROP(226, keyboard_mode, keyboard_mode, widget, rw) \
    GADGET_PROP(227, scale_mode, scale_mode, widget, rw) \
    GADGET_PROP(228, total_ticks, int, widget, rw) \
    GADGET_PROP(229, major_tick_every, int, widget, rw) \
    GADGET_PROP(230, label_show, bool, widget, rw) \
    GADGET_PROP(231, angle_range, angle, widget, rw) \
    GADGET_PROP(232, spin_time, int, widget, rw) \
    GADGET_PROP(233, arc_sweep, angle, widget, rw) \
    GADGET_PROP(234, chart_type, chart_type, widget, rw) \
    GADGET_PROP(235, point_count, int, widget, rw) \
    GADGET_PROP(236, update_mode, chart_update, widget, rw) \
    GADGET_PROP(237, hor_div_lines, int, widget, rw) \
    GADGET_PROP(238, ver_div_lines, int, widget, rw) \
    GADGET_PROP(239, row_count, int, widget, rw) \
    GADGET_PROP(240, column_count, int, widget, rw) \
    GADGET_PROP(241, image_scale, percent, widget, rw) \
    GADGET_PROP(242, offset_x, int, widget, rw) \
    GADGET_PROP(243, offset_y, int, widget, rw) \
    GADGET_PROP(244, inner_align, image_align, widget, rw) \
    GADGET_PROP(245, y_invert, bool, widget, rw) \


/* Well-known devices: a driver registering one of these names must match it.
 * GADGET_DEV(name, "fields", "actions", "events") */
#define MUSE_APPS_WELL_KNOWN(GADGET_DEV) \
    GADGET_DEV(temperature, "c", "", "") \
    GADGET_DEV(humidity, "rh", "", "") \
    GADGET_DEV(pressure, "hpa", "", "") \
    GADGET_DEV(co2, "ppm", "", "") \
    GADGET_DEV(voc, "index", "", "") \
    GADGET_DEV(light, "lux", "", "") \
    GADGET_DEV(accel, "x,y,z", "", "tap,double_tap,shake,face_up,face_down") \
    GADGET_DEV(gyro, "x,y,z", "", "") \
    GADGET_DEV(steps, "count", "", "") \
    GADGET_DEV(battery, "percent,volts,charging,usb", "", "") \
    GADGET_DEV(vibration, "", "buzz", "") \
    GADGET_DEV(speaker, "", "tone", "") \

