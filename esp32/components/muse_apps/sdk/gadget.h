/*
 * gadget.h: the Muse gadget apps API, version 1.1 (draft).
 *
 * Generated from components/muse_apps/sdk/api/api.json by components/muse_apps/sdk/api/gen_api.py. Don't edit it:
 * change api.json and run gen_api.py.
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

#ifndef GADGET_H
#define GADGET_H

#include <stdint.h>

#define GADGET_API_MAJOR 1
#define GADGET_API_MINOR 1

#ifdef __wasm__
#define GADGET_IMPORT(name) __attribute__((import_module("gadget"), import_name(#name)))
#define GADGET_EXPORT(name) __attribute__((export_name(#name)))
/* The app's manifest, as one JSON string literal, written to the custom section
 * gadget.manifest: GADGET_MANIFEST("{\"api\":\"1.0\", ...}"); */
#define GADGET_MANIFEST(json) \
    __asm__(".section .custom_section.gadget.manifest,\"\",@\n.ascii " #json "\n.text\n")
#else
#define GADGET_IMPORT(name)
#define GADGET_EXPORT(name)
#define GADGET_MANIFEST(json) extern int gadget_manifest_unused_
#endif

/*
 * Conventions. Every function is imported from the module "gadget". An app
 * that imports anything else isn't run. Integers are 32-bit and signed.
 * Colours are 0xRRGGBB. Opacity runs from 0 (transparent) to 255 (opaque).
 * Angles are whole degrees, clockwise, with 0 at 3 o'clock, as in LVGL.
 * Percentages run from 0 to 100. Strings are NUL-terminated UTF-8 in the app's
 * memory. A call copies what it needs before it returns. A handle or id is
 * positive; 0 is never a valid one. A function that can fail returns a
 * negative ERR_ constant. Nothing blocks. A call that needs the VM, or takes
 * time, returns an operation id at once and finishes later with EV_DONE (a:
 * the id, b: 0 or more on success, or an ERR_). A buffer given to such a call
 * must stay reserved until then. Changes to the screen made in one callback
 * appear together on the next frame after it returns. Getters see every change
 * made so far. The gadget describes itself: board facts come from sys_info,
 * and its sensors and other hardware from dev_ calls, each with a description
 * the firmware supplies. device.guide gives Muse the same before it writes an
 * app.
 */

/* ---- Values ---- */

/* The app's screen: the parent of top-level objects. It can't be deleted. */
#define UI_ROOT 1
/* A width or height that fits the object's content. */
#define SIZE_CONTENT 0x2FFFFFFF
/* p percent (0 to 1000) of the parent's content area. */
#define PCT(p) (0x20000000 + (p))
/* A radius that makes the object round, or its ends fully rounded. */
#define RADIUS_CIRCLE 0x7FFF
#define OPA_TRANSP 0  /* Fully transparent. */
#define OPA_COVER 255  /* Fully opaque. */
/* What ui_get returns for a wrong handle or property; the log says which. */
#define UI_INVALID (-0x7FFFFFFF - 1)

/* ---- Events: on_event(type, a, b) ---- */

#define EV_UI 1  /* a: handle, b: UI_ bit. Something happened to an object the app asked about with ui_on, or an image load finished. */
#define EV_BUTTON 2  /* a: BUTTON_, b: BTN_. A button. */
#define EV_KNOB 3  /* a: steps. The knob turned: positive clockwise. */
#define EV_TOUCH_DOWN 4  /* a: x, b: y. A touch began on the root, or on an object that isn't clickable. */
#define EV_TOUCH_MOVE 5  /* a: x, b: y. The touch moved. */
#define EV_TOUCH_UP 6  /* a: x, b: y. The touch ended. */
#define EV_GESTURE 7  /* a: GESTURE_. A swipe. */
#define EV_TIMER 8  /* a: timer id. A timer fired. */
#define EV_DONE 9  /* a: op id, b: result. An operation finished: b is 0 or more on success (bytes read, say), or an ERR_. */
#define EV_CHAT 10  /* a: CHAT_, b: message id or value. The chat changed, after chat_open. */
#define EV_MUSE 11  /* a: MUSE_. Muse's mode changed. */
#define EV_DEV_DATA 12  /* a: device, b: readings waiting. Readings to take with dev_take, after dev_open. At most one per frame per device. */
#define EV_DEV_EVENT 13  /* a: device, b: event index. A device's event (dev_event gives the index), after dev_open. */
#define EV_SCREEN 14  /* a: 1 on, 0 off. The screen went off (on_frame stops) or came back. */
#define EV_CONNECTION 15  /* a: 1 up, 0 down. The link to the VM came up or went down. */

/* ---- Constants ---- */

/*
 * Why a call failed. Every one is negative.
 */
#define ERR_ARG (-1)  /* An argument is out of range, a string is too long, or a buffer lies outside the app's memory. */
#define ERR_HANDLE (-2)  /* The handle or id isn't one this app holds, or is the wrong kind for the call. */
#define ERR_NO_MEMORY (-3)  /* The app's memory budget is used up. */
#define ERR_LIMIT (-4)  /* A per-app limit was reached (objects, styles, timers, operations, open sensors). */
#define ERR_BUSY (-5)  /* The thing is in use; try again later. */
#define ERR_OFFLINE (-6)  /* The gadget isn't connected to its VM. */
#define ERR_DENIED (-7)  /* The app's manifest doesn't ask for this permission, or the VM refused. */
#define ERR_UNSUPPORTED (-8)  /* This gadget doesn't have it: a sensor, an output, or a property on this widget. */
#define ERR_TOO_LARGE (-9)  /* The data is larger than the call takes. */
#define ERR_NOT_FOUND (-10)  /* The file, state or message doesn't exist. */
#define ERR_TIMEOUT (-11)  /* The VM didn't answer in time. */
#define ERR_FAILED (-12)  /* Anything else; the app's log says why. */

/*
 * What sys_info reports.
 */
#define INFO_API_VERSION 1  /* The API version the gadget serves, major * 100 + minor. */
#define INFO_SCREEN_WIDTH 2  /* The screen's width in pixels. */
#define INFO_SCREEN_HEIGHT 3  /* The screen's height in pixels. */
#define INFO_ROUND 4  /* 1 on a round screen. */
#define INFO_SAFE_INSET 5  /* Pixels to keep clear of the edges so nothing is cut off: on a round screen, the inset of the largest centred square. */
#define INFO_TOUCH 6  /* 1 if the screen takes touch. */
#define INFO_BUTTONS 7  /* The buttons this gadget reports, as bits: 1 << BUTTON_x. */
#define INFO_KNOB 8  /* 1 if the gadget has a knob or wheel (EV_KNOB). */
#define INFO_MEM_BUDGET 10  /* The app's memory budget in bytes: limits.budget_bytes, or less on a gadget short of PSRAM. */
#define INFO_MEM_USED 11  /* Bytes of the budget in use. */
#define INFO_CONNECTED 12  /* 1 while the gadget is connected to its VM. */

/*
 * Why an app is being closed (on_stop).
 */
#define STOP_USER 1  /* The person closed it (the power key, or the exit button held 2 s) or Muse ran app.stop. */
#define STOP_REPLACED 2  /* Muse started another app. */
#define STOP_CALL 3  /* A voice call started. */
#define STOP_VIDEO 4  /* The video view opened. */
#define STOP_EXIT 5  /* The app called sys_exit. */
#define STOP_UPDATE 6  /* The gadget is restarting, for an update or a setting. */

/*
 * Buttons, by role. Which exist depends on the board (INFO_BUTTONS). The power
 * key always closes the app and is never reported. Holding BUTTON_PRIMARY (the
 * talk button) for 2 s closes it too: device.guide names it as exit_hold; the
 * app sees its BTN_DOWN and BTN_LONG, then on_stop, and never its BTN_UP. If
 * the app gives the talk button back to Muse with muse_talk_key(1), that hold
 * is reserved for talking instead; provide an on-screen Close control or use
 * app.stop.
 */
#define BUTTON_PRIMARY 1  /* The talk button. Muse's push-to-talk while muse_talk_key(1). */
#define BUTTON_SECONDARY 2  /* The second button, where the board has one. */
#define BUTTON_UP 3  /* A navigation key. */
#define BUTTON_DOWN 4  /* A navigation key. */
#define BUTTON_LEFT 5  /* A navigation key. */
#define BUTTON_RIGHT 6  /* A navigation key. */
#define BUTTON_ENTER 7  /* Select, or a knob's press. */
#define BUTTON_BACK 8  /* Escape or back. */

/*
 * What a button did (EV_BUTTON's b).
 */
#define BTN_DOWN 1  /* Pressed. */
#define BTN_UP 2  /* Released. */
#define BTN_LONG 3  /* Still held 600 ms after DOWN; UP still follows. */

/*
 * Swipe directions (EV_GESTURE).
 */
#define GESTURE_LEFT 1
#define GESTURE_RIGHT 2
#define GESTURE_UP 3
#define GESTURE_DOWN 4

/*
 * What Muse is doing (muse_mode, EV_MUSE).
 */
#define MUSE_IDLE 0
#define MUSE_LISTENING 1  /* Recording the person. */
#define MUSE_THINKING 2  /* Waiting for the reply. */
#define MUSE_SPEAKING 3  /* Playing the reply. */
#define MUSE_ERROR 4  /* Something failed; the caption says what. */

/*
 * What changed in the chat (EV_CHAT's a).
 */
#define CHAT_MESSAGE 1  /* A new message; b is its id. */
#define CHAT_TEXT 2  /* A message's text grew while Muse writes it; b is its id. */
#define CHAT_DONE 3  /* A message is complete; b is its id. */
#define CHAT_BUSY 4  /* b is 1 while Muse is working on a reply, 0 once it's idle. */
#define CHAT_CONNECTED 5  /* b is 1 once the chat is followed, 0 when the link is lost (it reconnects by itself). */

/*
 * What chat_info reports about a message.
 */
#define CHAT_INFO_ROLE 1  /* A ROLE_ constant. */
#define CHAT_INFO_LENGTH 2  /* Bytes of text kept. */
#define CHAT_INFO_DONE 3  /* 1 once the message is complete. */
#define CHAT_INFO_TRUNCATED 4  /* 1 if the text was cut to chat_text_bytes. */

/*
 * Who wrote a message.
 */
#define ROLE_USER 1  /* The person. */
#define ROLE_MUSE 2  /* Muse. */
#define ROLE_APP 3  /* An app on a gadget, with chat_send. Its "[name on the gadget]" prefix is removed. */

/*
 * Object events, as bits for ui_on; EV_UI's b holds one.
 */
#define UI_CLICKED 1  /* Pressed and released without sliding off. */
#define UI_PRESSED 2
#define UI_RELEASED 4
#define UI_LONG_PRESSED 8  /* Held 400 ms. */
#define UI_VALUE_CHANGED 16  /* The person changed its value, selection, checked state or text. */
#define UI_FOCUSED 32
#define UI_DEFOCUSED 64
#define UI_READY 128  /* A keyboard's OK, or Enter in a one-line text area. */
#define UI_CANCEL 256  /* A keyboard's close key, or BACK while navigating. */
#define UI_SCROLL_END 512  /* Scrolling stopped. */
#define UI_ANIM_DONE 1024  /* An animation on the object finished. */
#define UI_LOADED 2048  /* ui_image_load finished. Sent without ui_on. */
#define UI_LOAD_FAILED 4096  /* ui_image_load failed; the log says why. Sent without ui_on. */

/*
 * Parts of a widget, for ui_style and ui_style_add. Combine with a STATE_.
 */
#define PART_MAIN 0  /* The background. */
#define PART_SCROLLBAR 256
#define PART_INDICATOR 512  /* A bar's, slider's or arc's filled part, a checkbox's box, a scale's major ticks. */
#define PART_KNOB 768  /* A slider's, arc's or switch's handle. */
#define PART_SELECTED 1024  /* The selected option or text. */
#define PART_ITEMS 1280  /* A table's cells, a chart's series, a scale's minor ticks, a keyboard's keys. */
#define PART_CURSOR 1536  /* A text area's cursor, a chart's cursor. */

/*
 * States, for ui_style and ui_style_add.
 */
#define STATE_DEFAULT 0
#define STATE_CHECKED 1
#define STATE_FOCUSED 2
#define STATE_PRESSED 4
#define STATE_DISABLED 8
#define STATE_EDITED 16  /* A slider or roller being changed with keys. */

/*
 * Where to put an object, as in LVGL.
 */
#define ALIGN_DEFAULT 0
#define ALIGN_TOP_LEFT 1
#define ALIGN_TOP_MID 2
#define ALIGN_TOP_RIGHT 3
#define ALIGN_BOTTOM_LEFT 4
#define ALIGN_BOTTOM_MID 5
#define ALIGN_BOTTOM_RIGHT 6
#define ALIGN_LEFT_MID 7
#define ALIGN_RIGHT_MID 8
#define ALIGN_CENTER 9
#define ALIGN_OUT_TOP_LEFT 10  /* Outside the base, for ui_align_to. */
#define ALIGN_OUT_TOP_MID 11
#define ALIGN_OUT_TOP_RIGHT 12
#define ALIGN_OUT_BOTTOM_LEFT 13
#define ALIGN_OUT_BOTTOM_MID 14
#define ALIGN_OUT_BOTTOM_RIGHT 15
#define ALIGN_OUT_LEFT_TOP 16
#define ALIGN_OUT_LEFT_MID 17
#define ALIGN_OUT_LEFT_BOTTOM 18
#define ALIGN_OUT_RIGHT_TOP 19
#define ALIGN_OUT_RIGHT_MID 20
#define ALIGN_OUT_RIGHT_BOTTOM 21

/*
 * Text align.
 */
#define TEXT_ALIGN_AUTO 0
#define TEXT_ALIGN_LEFT 1
#define TEXT_ALIGN_CENTER 2
#define TEXT_ALIGN_RIGHT 3

/*
 * The fonts every gadget has: Montserrat, with LVGL's symbols (SYM_).
 */
#define FONT_DEFAULT 0  /* The theme's font, 14 px. */
#define FONT_14 1
#define FONT_16 2
#define FONT_20 3
#define FONT_28 4
#define FONT_48 5  /* Digits, ':', '.', '-', '%' and the degree sign only. */

/*
 * Layout.
 */
#define LAYOUT_NONE 0  /* Children are placed by x, y and align. */
#define LAYOUT_FLEX 1  /* Children flow in rows or columns (P_FLEX_*). */
#define LAYOUT_GRID 2  /* Set with ui_grid. */

/*
 * Flex flow.
 */
#define FLEX_ROW 0
#define FLEX_COLUMN 1
#define FLEX_ROW_WRAP 2
#define FLEX_COLUMN_WRAP 3
#define FLEX_ROW_REVERSE 4
#define FLEX_COLUMN_REVERSE 5

/*
 * How flex places items and tracks.
 */
#define PLACE_START 0
#define PLACE_END 1
#define PLACE_CENTER 2
#define PLACE_SPACE_EVENLY 3
#define PLACE_SPACE_AROUND 4
#define PLACE_SPACE_BETWEEN 5

/*
 * Dir.
 */
#define DIR_NONE 0
#define DIR_LEFT 1
#define DIR_RIGHT 2
#define DIR_TOP 4
#define DIR_BOTTOM 8
#define DIR_HOR 3
#define DIR_VER 12
#define DIR_ALL 15

/*
 * Scrollbar mode.
 */
#define SCROLLBAR_OFF 0
#define SCROLLBAR_ON 1
#define SCROLLBAR_ACTIVE 2  /* While scrolling. */
#define SCROLLBAR_AUTO 3  /* When the content is larger. */

/*
 * Snap.
 */
#define SNAP_NONE 0
#define SNAP_START 1
#define SNAP_END 2
#define SNAP_CENTER 3

/*
 * What a label does with text that doesn't fit its size.
 */
#define LONG_WRAP 0  /* Wraps, growing the height. */
#define LONG_DOTS 1  /* Ends with ... */
#define LONG_SCROLL 2  /* Scrolls back and forth. */
#define LONG_SCROLL_CIRCULAR 3  /* Scrolls round. */
#define LONG_CLIP 4  /* Cuts it off. */

/*
 * Grad dir.
 */
#define GRAD_NONE 0
#define GRAD_VER 1
#define GRAD_HOR 2

/*
 * Text decor.
 */
#define DECOR_NONE 0
#define DECOR_UNDERLINE 1
#define DECOR_STRIKETHROUGH 2

/*
 * Border side.
 */
#define BORDER_NONE 0
#define BORDER_BOTTOM 1
#define BORDER_TOP 2
#define BORDER_LEFT 4
#define BORDER_RIGHT 8
#define BORDER_FULL 15

/*
 * Arc mode.
 */
#define ARC_NORMAL 0
#define ARC_SYMMETRICAL 1  /* Fills from the middle. */
#define ARC_REVERSE 2

/*
 * Bar mode.
 */
#define BAR_NORMAL 0
#define BAR_SYMMETRICAL 1  /* Fills from zero. */
#define BAR_RANGE 2  /* From P_START_VALUE to P_VALUE. */

/*
 * Orientation.
 */
#define ORIENT_AUTO 0  /* From the object's shape. */
#define ORIENT_HORIZONTAL 1
#define ORIENT_VERTICAL 2

/*
 * Keyboard mode.
 */
#define KB_TEXT_LOWER 0
#define KB_TEXT_UPPER 1
#define KB_SPECIAL 2
#define KB_NUMBER 3

/*
 * Scale mode.
 */
#define SCALE_HORIZONTAL_TOP 0
#define SCALE_HORIZONTAL_BOTTOM 1
#define SCALE_VERTICAL_LEFT 2
#define SCALE_VERTICAL_RIGHT 3
#define SCALE_ROUND_INNER 4  /* A dial, labels inside. */
#define SCALE_ROUND_OUTER 5  /* A dial, labels outside. */

/*
 * Chart type.
 */
#define CHART_NONE 0
#define CHART_LINE 1
#define CHART_BAR 2

/*
 * Chart update.
 */
#define UPDATE_SHIFT 0  /* ui_chart_push shifts the points left. */
#define UPDATE_CIRCULAR 1  /* ui_chart_push overwrites the oldest point. */

/*
 * How an image fills an image object larger or smaller than itself.
 */
#define IMAGE_CENTER 0
#define IMAGE_TOP_LEFT 1
#define IMAGE_STRETCH 2
#define IMAGE_TILE 3
#define IMAGE_CONTAIN 4
#define IMAGE_COVER 5

/*
 * Animation curves; add ANIM_ flags.
 */
#define EASE_LINEAR 0
#define EASE_IN 1
#define EASE_OUT 2
#define EASE_IN_OUT 3
#define EASE_OVERSHOOT 4
#define EASE_BOUNCE 5
#define EASE_STEP 6  /* Jumps at the end. */

/*
 * Anim flag.
 */
#define ANIM_REPEAT 256  /* Runs forever. */
#define ANIM_REVERSE 512  /* Runs back to the start after each run. */

/* Widget types, for ui_create. */
#define W_OBJ 1  /* A plain container: a box with a background, border and children. */
#define W_LABEL 2  /* Text. */
#define W_BUTTON 3  /* A container that presses; put a label in it. */
#define W_IMAGE 4  /* A PNG (8-bit channels) or JPEG (baseline) (ui_image_load, ui_image_set). */
#define W_LINE 5  /* A polyline through points (ui_line_points). */
#define W_ARC 6  /* A ring with a value, which touch can drag. */
#define W_BAR 7  /* A progress bar. */
#define W_SLIDER 8  /* A bar with a knob to drag. */
#define W_SWITCH 9  /* An on/off toggle; P_CHECKED is its state. */
#define W_CHECKBOX 10  /* A box with a label; P_CHECKED is its state. */
#define W_ROLLER 11  /* A drum of options to spin. */
#define W_DROPDOWN 12  /* A button that opens a list of options. */
#define W_TEXTAREA 13  /* Editable text, for a keyboard. */
#define W_KEYBOARD 14  /* An on-screen keyboard that types into a text area. */
#define W_SCALE 15  /* Ticks and labels in a line or a dial; ui_scale_needle points a line at a value. */
#define W_SPINNER 16  /* A spinning arc for 'working'. */
#define W_CHART 17  /* Line or bar series (ui_chart_series, ui_chart_set, ui_chart_push). */
#define W_TABLE 18  /* Rows and columns of text (ui_table_cell). */
#define W_CANVAS 19  /* Pixels to draw on with the cv_ calls. Its buffer is RGB565 and counts against the budget; P_WIDTH and P_HEIGHT size it, up to the screen. */

/* Properties, for ui_set, ui_get, ui_set_text, ui_style and ui_animate. */
#define P_X 1  /* coord, any object. Left edge, from the parent's content area, or the offset from P_ALIGN. */
#define P_Y 2  /* coord, any object. Top edge, or the offset from P_ALIGN. */
#define P_WIDTH 3  /* coord, any object */
#define P_HEIGHT 4  /* coord, any object */
#define P_MIN_WIDTH 5  /* coord, any object */
#define P_MAX_WIDTH 6  /* coord, any object */
#define P_MIN_HEIGHT 7  /* coord, any object */
#define P_MAX_HEIGHT 8  /* coord, any object */
#define P_ALIGN 9  /* align, any object. Where in the parent; P_X and P_Y offset it. */
#define P_HIDDEN 10  /* bool, any object. Not drawn, and takes no input or space in a layout. */
#define P_CLICKABLE 11  /* bool, any object. Takes touch. On for buttons and inputs, off for labels and images. */
#define P_SCROLLABLE 12  /* bool, any object. Scrolls when its content is larger. On for containers. */
#define P_CHECKABLE 13  /* bool, any object. A click toggles P_CHECKED. */
#define P_EVENT_BUBBLE 14  /* bool, any object. Its events also go to its parent (one ui_on on a list instead of one per row). */
#define P_FLOATING 15  /* bool, any object. Left out of the parent's layout and scrolling. */
#define P_CHECKED 16  /* bool, any object. The checked state: a switch's or checkbox's value. */
#define P_DISABLED 17  /* bool, any object. Greyed out; takes no input. */
#define P_SCROLL_DIR 18  /* dir, any object */
#define P_SCROLLBAR_MODE 19  /* scrollbar_mode, any object */
#define P_SCROLL_SNAP_X 20  /* snap, any object. Which part of a child to stop on after a horizontal scroll. */
#define P_SCROLL_SNAP_Y 21  /* snap, any object */
#define P_SCROLL_X 22  /* int, any object, read-only. How far it's scrolled; scroll with ui_scroll_to. */
#define P_SCROLL_Y 23  /* int, any object, read-only */
#define P_INDEX 24  /* int, any object. Its place among its siblings, which sets the drawing order: 0 is drawn first, -1 moves it last. */
#define P_CHILD_COUNT 25  /* int, any object, read-only */
#define P_LAYOUT 26  /* layout, any object */
#define P_FLEX_FLOW 27  /* flex_flow, any object */
#define P_FLEX_MAIN_PLACE 28  /* place, any object. Along the flow. */
#define P_FLEX_CROSS_PLACE 29  /* place, any object. Across the flow, within a track (see flex_track_place). */
#define P_FLEX_TRACK_PLACE 30  /* place, any object. Where the tracks sit across the flow. A row or column that doesn't wrap is one track, as wide (or tall) as its largest item, so centring its items across the flow takes PLACE_CENTER here and in flex_cross_place. */
#define P_FLEX_GROW 31  /* int, any object. A child's share of the free space in its parent's flex track; 0 keeps its own size. */
#define P_BG_COLOR 100  /* rgb, style */
#define P_BG_OPA 101  /* opa, style */
#define P_BG_GRAD_COLOR 102  /* rgb, style. The gradient's end colour. */
#define P_BG_GRAD_DIR 103  /* grad_dir, style */
#define P_BORDER_COLOR 104  /* rgb, style */
#define P_BORDER_WIDTH 105  /* int, style */
#define P_BORDER_OPA 106  /* opa, style */
#define P_BORDER_SIDE 107  /* border_side, style */
#define P_OUTLINE_COLOR 108  /* rgb, style */
#define P_OUTLINE_WIDTH 109  /* int, style */
#define P_OUTLINE_PAD 110  /* int, style. Gap between the object and its outline. */
#define P_SHADOW_COLOR 111  /* rgb, style */
#define P_SHADOW_WIDTH 112  /* int, style. Blur width, 0 to 64. Shadows cost drawing time; use them on a few objects. */
#define P_SHADOW_OPA 113  /* opa, style */
#define P_SHADOW_OFFSET_X 114  /* int, style. -64 to 64. */
#define P_SHADOW_OFFSET_Y 115  /* int, style. -64 to 64. */
#define P_SHADOW_SPREAD 116  /* int, style. 0 to 64. */
#define P_RADIUS 117  /* int, style. Corner radius, 0 to 64; RADIUS_CIRCLE for round. */
#define P_PAD_ALL 118  /* int, style, write-only. Sets the four paddings. */
#define P_PAD_LEFT 119  /* int, style */
#define P_PAD_RIGHT 120  /* int, style */
#define P_PAD_TOP 121  /* int, style */
#define P_PAD_BOTTOM 122  /* int, style */
#define P_PAD_ROW 123  /* int, style. Gap between rows of a layout. */
#define P_PAD_COLUMN 124  /* int, style. Gap between columns of a layout. */
#define P_TEXT_COLOR 125  /* rgb, style */
#define P_TEXT_OPA 126  /* opa, style */
#define P_TEXT_FONT 127  /* font, style */
#define P_TEXT_ALIGN 128  /* text_align, style */
#define P_TEXT_LETTER_SPACE 129  /* int, style */
#define P_TEXT_LINE_SPACE 130  /* int, style. Not negative. */
#define P_TEXT_DECOR 131  /* text_decor, style */
#define P_LINE_COLOR 132  /* rgb, style */
#define P_LINE_WIDTH 133  /* int, style */
#define P_LINE_OPA 134  /* opa, style */
#define P_LINE_ROUNDED 135  /* bool, style */
#define P_LINE_DASH_WIDTH 136  /* int, style. 0 to 64. */
#define P_LINE_DASH_GAP 137  /* int, style. 0 to 64. */
#define P_ARC_COLOR 138  /* rgb, style */
#define P_ARC_WIDTH 139  /* int, style */
#define P_ARC_OPA 140  /* opa, style */
#define P_ARC_ROUNDED 141  /* bool, style */
#define P_IMAGE_OPA 142  /* opa, style */
#define P_IMAGE_RECOLOR 143  /* rgb, style. Tints an image (or a symbol drawn as one). */
#define P_IMAGE_RECOLOR_OPA 144  /* opa, style */
#define P_OPA 145  /* opa, style. The whole object and its children. */
#define P_TRANSFORM_ROTATION 146  /* angle, style. Rotates the drawn object about its pivot. Costs drawing time and memory. */
#define P_TRANSFORM_SCALE 147  /* percent, style. Scales the drawn object about its pivot. */
#define P_TRANSFORM_PIVOT_X 148  /* coord, style */
#define P_TRANSFORM_PIVOT_Y 149  /* coord, style */
#define P_TRANSLATE_X 150  /* coord, style. Moves the drawn object without moving it in the layout. */
#define P_TRANSLATE_Y 151  /* coord, style */
#define P_LENGTH 152  /* int, style. A scale's tick length (PART_INDICATOR major, PART_ITEMS minor). */
#define P_CLIP_CORNER 153  /* bool, style. Clips children to the rounded corners. */
#define P_ANIM_DURATION 154  /* int, style. Milliseconds for a widget's own animations: a label's scrolling, a roller's spin. */
#define P_TEXT 200  /* text, label, checkbox, textarea, dropdown. The text. On a dropdown, fixed text to show in place of the selected option. */
#define P_LONG_MODE 201  /* long_mode, label */
#define P_VALUE 202  /* int, arc, bar, slider */
#define P_MIN_VALUE 203  /* int, arc, bar, slider, scale, chart. The range's low end; a chart's Y axis. */
#define P_MAX_VALUE 204  /* int, arc, bar, slider, scale, chart */
#define P_START_VALUE 205  /* int, bar, slider. The low end in BAR_RANGE mode. */
#define P_BAR_MODE 206  /* bar_mode, bar, slider */
#define P_ORIENTATION 207  /* orientation, bar, slider, switch */
#define P_ARC_MODE 208  /* arc_mode, arc */
#define P_START_ANGLE 209  /* angle, arc. The indicator's start; setting P_VALUE moves it too. */
#define P_END_ANGLE 210  /* angle, arc */
#define P_BG_START_ANGLE 211  /* angle, arc. The ring's start. */
#define P_BG_END_ANGLE 212  /* angle, arc */
#define P_ROTATION 213  /* angle, arc, scale, image. Turns the whole widget. */
#define P_OPTIONS 214  /* text, roller, dropdown. The options, one per line. */
#define P_SELECTED 215  /* int, roller, dropdown. The selected option, from 0. */
#define P_VISIBLE_ROWS 216  /* int, roller */
#define P_INFINITE 217  /* bool, roller. Spins round instead of stopping at the ends. ERR_ARG if letter spacing leaves its options no height. */
#define P_LIST_DIR 218  /* dir, dropdown. Which way the list opens. */
#define P_PLACEHOLDER 219  /* text, textarea. Text shown while it's empty. */
#define P_ONE_LINE 220  /* bool, textarea */
#define P_PASSWORD 221  /* bool, textarea. Shows bullets. */
#define P_MAX_LENGTH 222  /* int, textarea */
#define P_ACCEPTED_CHARS 223  /* text, textarea. The only characters it takes. */
#define P_CURSOR_POS 224  /* int, textarea */
#define P_TEXTAREA 225  /* handle, keyboard. The text area it types into. */
#define P_KEYBOARD_MODE 226  /* keyboard_mode, keyboard */
#define P_SCALE_MODE 227  /* scale_mode, scale */
#define P_TOTAL_TICKS 228  /* int, scale */
#define P_MAJOR_TICK_EVERY 229  /* int, scale. Every nth tick is major and labelled. */
#define P_LABEL_SHOW 230  /* bool, scale */
#define P_ANGLE_RANGE 231  /* angle, scale. A dial's sweep. */
#define P_SPIN_TIME 232  /* int, spinner. Milliseconds per turn. */
#define P_ARC_SWEEP 233  /* angle, spinner. The spinning arc's length. */
#define P_CHART_TYPE 234  /* chart_type, chart */
#define P_POINT_COUNT 235  /* int, chart. Points per series. */
#define P_UPDATE_MODE 236  /* chart_update, chart */
#define P_HOR_DIV_LINES 237  /* int, chart. Horizontal grid lines. */
#define P_VER_DIV_LINES 238  /* int, chart. Vertical grid lines. */
#define P_ROW_COUNT 239  /* int, table */
#define P_COLUMN_COUNT 240  /* int, table */
#define P_IMAGE_SCALE 241  /* percent, image. Zoom. */
#define P_OFFSET_X 242  /* int, image. Shifts the picture inside the object, wrapping round. */
#define P_OFFSET_Y 243  /* int, image */
#define P_INNER_ALIGN 244  /* image_align, image */
#define P_Y_INVERT 245  /* bool, line. Y points up from the bottom. */

/*
 * Well-known devices, for dev_find, and where their fields, actions and events
 * are. A gadget may have none of these, and devices with any other name: it
 * lists its own.
 */
#define DEV_TEMPERATURE "temperature"  /* Air temperature. */
#define DEV_TEMPERATURE_C 0  /* field, °C */
#define DEV_HUMIDITY "humidity"  /* Relative humidity. */
#define DEV_HUMIDITY_RH 0  /* field, % */
#define DEV_PRESSURE "pressure"  /* Air pressure. */
#define DEV_PRESSURE_HPA 0  /* field, hPa */
#define DEV_CO2 "co2"  /* CO2. */
#define DEV_CO2_PPM 0  /* field, ppm */
#define DEV_VOC "voc"  /* Volatile organic compounds, as Sensirion's VOC index (1 to 500). */
#define DEV_VOC_INDEX 0  /* field */
#define DEV_LIGHT "light"  /* Ambient light. */
#define DEV_LIGHT_LUX 0  /* field, lx */
#define DEV_ACCEL "accel"  /* Acceleration in the screen's frame: x right, y up, z out of the screen. Events from the IMU's own feature engine where it has one. */
#define DEV_ACCEL_X 0  /* field, g */
#define DEV_ACCEL_Y 1  /* field, g */
#define DEV_ACCEL_Z 2  /* field, g */
#define DEV_ACCEL_TAP 0  /* event */
#define DEV_ACCEL_DOUBLE_TAP 1  /* event */
#define DEV_ACCEL_SHAKE 2  /* event */
#define DEV_ACCEL_FACE_UP 3  /* event */
#define DEV_ACCEL_FACE_DOWN 4  /* event */
#define DEV_GYRO "gyro"  /* Rotation rate, in accel's axes. */
#define DEV_GYRO_X 0  /* field, deg/s */
#define DEV_GYRO_Y 1  /* field, deg/s */
#define DEV_GYRO_Z 2  /* field, deg/s */
#define DEV_STEPS "steps"  /* Steps counted since dev_open, from the IMU's step counter. */
#define DEV_STEPS_COUNT 0  /* field */
#define DEV_BATTERY "battery"  /* charging and usb are 0 or 1; volts is 0 where the board can't measure it. */
#define DEV_BATTERY_PERCENT 0  /* field, % */
#define DEV_BATTERY_VOLTS 1  /* field, V */
#define DEV_BATTERY_CHARGING 2  /* field */
#define DEV_BATTERY_USB 3  /* field */
#define DEV_VIBRATION "vibration"  /* A vibration motor. */
#define DEV_VIBRATION_BUZZ 0  /* action: Buzzes; a new buzz replaces one still running. Params: ms (ms, 1 to 1000). */
#define DEV_SPEAKER "speaker"  /* Beeps on the gadget's speaker; ERR_BUSY while Muse listens or speaks. */
#define DEV_SPEAKER_TONE 0  /* action: Plays a tone. Params: hz (Hz, 100 to 8000), ms (ms, 1 to 2000), volume (%, 0 to 100). */

/* LVGL's symbols, as UTF-8 strings, for labels in any FONT_ but FONT_48. */
#define SYM_AUDIO "\xEF\x80\x81"
#define SYM_VIDEO "\xEF\x80\x88"
#define SYM_LIST "\xEF\x80\x8B"
#define SYM_OK "\xEF\x80\x8C"
#define SYM_CLOSE "\xEF\x80\x8D"
#define SYM_POWER "\xEF\x80\x91"
#define SYM_SETTINGS "\xEF\x80\x93"
#define SYM_HOME "\xEF\x80\x95"
#define SYM_DOWNLOAD "\xEF\x80\x99"
#define SYM_REFRESH "\xEF\x80\xA1"
#define SYM_MUTE "\xEF\x80\xA6"
#define SYM_VOLUME_MID "\xEF\x80\xA7"
#define SYM_VOLUME_MAX "\xEF\x80\xA8"
#define SYM_IMAGE "\xEF\x80\xBE"
#define SYM_PREV "\xEF\x81\x88"
#define SYM_PLAY "\xEF\x81\x8B"
#define SYM_PAUSE "\xEF\x81\x8C"
#define SYM_STOP "\xEF\x81\x8D"
#define SYM_NEXT "\xEF\x81\x91"
#define SYM_LEFT "\xEF\x81\x93"
#define SYM_RIGHT "\xEF\x81\x94"
#define SYM_PLUS "\xEF\x81\xA7"
#define SYM_MINUS "\xEF\x81\xA8"
#define SYM_WARNING "\xEF\x81\xB1"
#define SYM_SHUFFLE "\xEF\x81\xB4"
#define SYM_UP "\xEF\x81\xB7"
#define SYM_DOWN "\xEF\x81\xB8"
#define SYM_LOOP "\xEF\x81\xB9"
#define SYM_CALL "\xEF\x82\x95"
#define SYM_ENVELOPE "\xEF\x83\xA0"
#define SYM_CHARGE "\xEF\x83\xA7"
#define SYM_BELL "\xEF\x83\xB3"
#define SYM_KEYBOARD "\xEF\x84\x9C"
#define SYM_GPS "\xEF\x84\xA4"
#define SYM_WIFI "\xEF\x87\xAB"
#define SYM_BATTERY_FULL "\xEF\x89\x80"
#define SYM_BATTERY_3 "\xEF\x89\x81"
#define SYM_BATTERY_2 "\xEF\x89\x82"
#define SYM_BATTERY_1 "\xEF\x89\x83"
#define SYM_BATTERY_EMPTY "\xEF\x89\x84"
#define SYM_BLUETOOTH "\xEF\x8A\x93"
#define SYM_TRASH "\xEF\x8B\xAD"
#define SYM_EDIT "\xEF\x8C\x84"
#define SYM_BACKSPACE "\xEF\x95\x9A"

/*
 * One reading of a device.
 */
typedef struct {
    int32_t t_ms;  /* sys_millis() when it was taken. */
    int32_t count;  /* How many values it holds. */
    float v[8];  /* The values, in the order and units of the device's fields. */
} gadget_reading_t;

/* ---- Functions ---- */

/* == System == */
/*
 * Time, logging, board facts, timers and the app's own lifecycle.
 */

/*
 * A clock for timing. Wraps after 24 days. Returns milliseconds since the
 * gadget started.
 */
GADGET_IMPORT(sys_millis) int32_t sys_millis(void);

/*
 * The time of day. 0 until the gadget has set its clock from the VM, soon
 * after it connects. Returns UTC milliseconds since 1970, or 0.
 */
GADGET_IMPORT(sys_time) int64_t sys_time(void);

/*
 * Adds a line to the app's log, which Muse reads with app.logs. text: The
 * line.
 */
GADGET_IMPORT(sys_log) void sys_log(const char *text);

/*
 * Board facts and the app's memory use. key: What to report. Returns the
 * value, or ERR_ARG.
 */
GADGET_IMPORT(sys_info) int32_t sys_info(int32_t key);

/*
 * From the hardware random number generator. Returns 32 random bits.
 */
GADGET_IMPORT(sys_random) int32_t sys_random(void);

/*
 * Starts or restarts a timer: EV_TIMER after ms milliseconds, while the app
 * runs, screen on or off. id: 1 to 8. ms: 10 or more. repeat: Fire every ms,
 * not once.
 */
GADGET_IMPORT(sys_timer) int32_t sys_timer(int32_t id, int32_t ms, int32_t repeat);

/*
 * Stops a timer.
 */
GADGET_IMPORT(sys_timer_stop) int32_t sys_timer_stop(int32_t id);

/*
 * How often on_frame is called while the screen is on. 30 to start with, if
 * the app exports on_frame. fps: 0 to 30.
 */
GADGET_IMPORT(sys_frame_rate) int32_t sys_frame_rate(int32_t fps);

/*
 * Keeps the screen on while on is 1. Use it only while showing something the
 * person is watching.
 */
GADGET_IMPORT(sys_keep_awake) void sys_keep_awake(int32_t on);

/*
 * Ends the app once the current callback returns, after on_stop(STOP_EXIT).
 */
GADGET_IMPORT(sys_exit) void sys_exit(void);

/*
 * Sets the screen's brightness while the app runs. percent: 1 to 100, or -1
 * for the person's own setting.
 */
GADGET_IMPORT(sys_brightness) int32_t sys_brightness(int32_t percent);

/* == UI objects == */
/*
 * LVGL widgets, kept by the firmware. Create them once, change their
 * properties, and ask for their events with ui_on. Properties are P_ constants
 * (Properties, below); style properties set this way apply to the main part in
 * the default state.
 */

/*
 * Creates a widget with the theme's look. type: A W_ constant. parent:
 * UI_ROOT, or another object other than a roller. Returns the new object.
 */
GADGET_IMPORT(ui_create) int32_t ui_create(int32_t type, int32_t parent);

/*
 * Deletes an object and its children. Their handles become invalid.
 */
GADGET_IMPORT(ui_delete) int32_t ui_delete(int32_t obj);

/*
 * Deletes an object's children.
 */
GADGET_IMPORT(ui_clean) int32_t ui_clean(int32_t obj);

/*
 * Sets a property that isn't text. prop: A P_ constant.
 */
GADGET_IMPORT(ui_set) int32_t ui_set(int32_t obj, int32_t prop, int32_t value);

/*
 * Reads a property that isn't text. Returns the value, or UI_INVALID.
 */
GADGET_IMPORT(ui_get) int32_t ui_get(int32_t obj, int32_t prop);

/*
 * Sets a text property. prop: A text property: P_TEXT, P_OPTIONS...
 */
GADGET_IMPORT(ui_set_text) int32_t ui_set_text(int32_t obj, int32_t prop, const char *text);

/*
 * Reads a text property, such as what was typed into a text area. buf: Gets
 * the text, cut to fit and always NUL-terminated. Returns the text's full
 * length in bytes.
 */
GADGET_IMPORT(ui_get_text) int32_t ui_get_text(int32_t obj, int32_t prop, void *buf, int32_t buf_size);

/*
 * Chooses which events of this object come to on_event as EV_UI. Replaces the
 * earlier choice. events: UI_ bits, 0 for none.
 */
GADGET_IMPORT(ui_on) int32_t ui_on(int32_t obj, int32_t events);

/*
 * Places an object in its parent, offset by dx, dy.
 */
GADGET_IMPORT(ui_align) int32_t ui_align(int32_t obj, int32_t align, int32_t dx, int32_t dy);

/*
 * Places an object relative to another, once (it doesn't follow base later).
 * base: Another object. align: ALIGN_OUT_ puts it outside base.
 */
GADGET_IMPORT(ui_align_to) int32_t ui_align_to(int32_t obj, int32_t base, int32_t align, int32_t dx, int32_t dy);

/*
 * Moves an object to another parent. parent: Not a roller, nor the object or
 * one inside it.
 */
GADGET_IMPORT(ui_set_parent) int32_t ui_set_parent(int32_t obj, int32_t parent);

/*
 * Lays out the object's children in a grid. Sizes are space-separated: pixels,
 * "auto" (fits the content) or "Nfr" (N shares of what's left), up to 8 each:
 * ui_grid(obj, "1fr 1fr 1fr", "auto 1fr"). Place children with ui_grid_cell.
 * columns: Column sizes. rows: Row sizes.
 */
GADGET_IMPORT(ui_grid) int32_t ui_grid(int32_t obj, const char *columns, const char *rows);

/*
 * Puts a child in a grid cell; the child stretches to fill it. obj: A child of
 * a grid.
 */
GADGET_IMPORT(ui_grid_cell) int32_t ui_grid_cell(int32_t obj, int32_t col, int32_t col_span, int32_t row, int32_t row_span);

/*
 * Scrolls an object's content.
 */
GADGET_IMPORT(ui_scroll_to) int32_t ui_scroll_to(int32_t obj, int32_t x, int32_t y, int32_t animate);

/*
 * Scrolls its parents until the object is visible: the newest message in a
 * chat, say.
 */
GADGET_IMPORT(ui_scroll_to_view) int32_t ui_scroll_to_view(int32_t obj, int32_t animate);

/*
 * On boards without touch: buttons and the knob move focus between the app's
 * buttons, sliders and other inputs (UP and DOWN, or the knob), ENTER presses
 * or edits, BACK sends UI_CANCEL. Buttons it uses don't come as EV_BUTTON
 * while it's on.
 */
GADGET_IMPORT(ui_navigation) int32_t ui_navigation(int32_t on);

/*
 * How wide a line of text is. Returns pixels.
 */
GADGET_IMPORT(ui_text_width) int32_t ui_text_width(int32_t font, const char *text);

/*
 * How tall text is, wrapped at width. width: Wrap width. Returns pixels.
 */
GADGET_IMPORT(ui_text_height) int32_t ui_text_height(int32_t font, int32_t width, const char *text);

/*
 * How much of row y shows, less margin at each end: on a round screen the
 * circle's chord, else the screen's width. Size text and rows near the top and
 * bottom of a round screen by it. y: A row of the screen. margin: Pixels to
 * keep clear at each end. Returns pixels, centred on the screen; 0 off the
 * screen.
 */
GADGET_IMPORT(ui_span) int32_t ui_span(int32_t y, int32_t margin);

/*
 * Fits a label's text on one line width wide: in font, or the next smaller
 * font it fits in, down to FONT_14, then cut short with "...". Sets the
 * label's width, font and long mode. Call it after ui_set_text, and again when
 * the text changes, for any text whose length you don't control: a name, a
 * message, a number that can grow. obj: A label. width: The most it may take.
 * font: The font it would like. Returns the FONT_ it got.
 */
GADGET_IMPORT(ui_fit_text) int32_t ui_fit_text(int32_t obj, int32_t width, int32_t font);

/*
 * Loads a PNG or JPEG into an image object, from the VM or from the gadget's
 * own files (which work offline). EV_UI with UI_LOADED or UI_LOAD_FAILED says
 * when it's done. The decoded image counts against the budget. obj: An image
 * object. path: In the app's folder on the VM, or from "/" on the gadget
 * (files.gadget).
 */
GADGET_IMPORT(ui_image_load) int32_t ui_image_load(int32_t obj, const char *path);

/*
 * Shows a PNG or JPEG from the app's memory, decoded at once. obj: An image
 * object. data: A PNG or JPEG file.
 */
GADGET_IMPORT(ui_image_set) int32_t ui_image_set(int32_t obj, const void *data, int32_t data_len);

/*
 * Sets a line's points. obj: A line object. xy: int32_t x, y pairs.
 */
GADGET_IMPORT(ui_line_points) int32_t ui_line_points(int32_t obj, const void *xy, int32_t xy_len);

/*
 * Adds a series to a chart. Returns the series, 1 to 4.
 */
GADGET_IMPORT(ui_chart_series) int32_t ui_chart_series(int32_t chart, int32_t color);

/*
 * Sets a series' points. values: int32_t values, up to P_POINT_COUNT.
 */
GADGET_IMPORT(ui_chart_set) int32_t ui_chart_set(int32_t chart, int32_t series, const void *values, int32_t values_len);

/*
 * Adds a point to a series, as P_UPDATE_MODE says.
 */
GADGET_IMPORT(ui_chart_push) int32_t ui_chart_push(int32_t chart, int32_t series, int32_t value);

/*
 * Sets a cell's text, adding rows and columns as needed.
 */
GADGET_IMPORT(ui_table_cell) int32_t ui_table_cell(int32_t table, int32_t row, int32_t col, const char *text);

GADGET_IMPORT(ui_table_column_width) int32_t ui_table_column_width(int32_t table, int32_t col, int32_t width);

/*
 * Points a line at a value on a round scale. line: A line object, child of the
 * scale. length: Pixels from the centre.
 */
GADGET_IMPORT(ui_scale_needle) int32_t ui_scale_needle(int32_t scale, int32_t line, int32_t length, int32_t value);

/* == Styles == */
/*
 * Style properties for other parts and states, and shared styles.
 */

/*
 * Sets a style property for one part and state: ui_style(slider,
 * PART_INDICATOR, P_BG_COLOR, 0x00C8FF). Each selector counts once toward
 * limits.styles_per_object. selector: A PART_ plus STATE_ bits. prop: A style
 * property.
 */
GADGET_IMPORT(ui_style) int32_t ui_style(int32_t obj, int32_t selector, int32_t prop, int32_t value);

/*
 * Creates a shared style, to give many objects the same look with one set of
 * calls. Returns A style.
 */
GADGET_IMPORT(ui_style_create) int32_t ui_style_create(void);

/*
 * Sets a property in a shared style. Objects using it update. prop: A style
 * property.
 */
GADGET_IMPORT(ui_style_set) int32_t ui_style_set(int32_t style, int32_t prop, int32_t value);

/*
 * Gives an object a shared style. With the selectors ui_style has set on it,
 * 32 per object (limits.styles_per_object). Properties set on the object
 * itself still win. selector: A PART_ plus STATE_ bits.
 */
GADGET_IMPORT(ui_style_add) int32_t ui_style_add(int32_t obj, int32_t style, int32_t selector);

/* == Animation == */
/*
 * Animating a property to a value.
 */

/*
 * Animates a property from its value now to to. UI_ANIM_DONE follows, if asked
 * for with ui_on. A new animation of the same property replaces the old one.
 * prop: A property that isn't text. easing: An EASE_ plus ANIM_ flags.
 */
GADGET_IMPORT(ui_animate) int32_t ui_animate(int32_t obj, int32_t prop, int32_t to, int32_t ms, int32_t easing);

/*
 * Stops animations where they are. prop: 0 for all.
 */
GADGET_IMPORT(ui_animate_stop) int32_t ui_animate_stop(int32_t obj, int32_t prop);

/* == Canvas == */
/*
 * Immediate-mode drawing on a canvas object (W_CANVAS). Drawing shows on the
 * next frame.
 */

/*
 * Fills the whole canvas.
 */
GADGET_IMPORT(cv_fill) int32_t cv_fill(int32_t canvas, int32_t color);

/*
 * Opacity for the drawing calls that follow on this canvas. Starts at
 * OPA_COVER.
 */
GADGET_IMPORT(cv_opa) int32_t cv_opa(int32_t canvas, int32_t opa);

/*
 * A filled rectangle; with radius RADIUS_CIRCLE, a circle or pill.
 */
GADGET_IMPORT(cv_rect) int32_t cv_rect(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t color);

/*
 * Draws a rectangle with a shared style: P_RADIUS; P_BG_COLOR, P_BG_OPA,
 * P_BG_GRAD_COLOR and P_BG_GRAD_DIR (GRAD_HOR or GRAD_VER, two colour stops);
 * P_BORDER_COLOR, P_BORDER_WIDTH, P_BORDER_OPA and P_BORDER_SIDE;
 * P_SHADOW_COLOR, P_SHADOW_WIDTH, P_SHADOW_OPA, P_SHADOW_OFFSET_X,
 * P_SHADOW_OFFSET_Y and P_SHADOW_SPREAD. P_OPA and cv_opa multiply the fill,
 * border and shadow opacities. Missing properties give a white fill, square
 * corners and no border or shadow. Other properties are ignored. Border and
 * shadow widths are clamped to 0..64 px, shadow spread to 0..64, and offsets
 * to -4096..4096. Drawing, including shadows, is clipped to the canvas buffer:
 * leave room around the rectangle. It paints once; later style changes affect
 * only later calls, not already drawn pixels. No buffer yet means no drawing.
 * Available since API 1.1. w: Positive pixels, no wider than the canvas. h:
 * Positive pixels, no taller than the canvas. style: A shared style from
 * ui_style_create, painted as it is now.
 */
GADGET_IMPORT(cv_rect_style) int32_t cv_rect_style(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t style);

/*
 * A rectangle's outline, width pixels thick, inside it.
 */
GADGET_IMPORT(cv_border) int32_t cv_border(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width, int32_t color);

/*
 * A line with round ends.
 */
GADGET_IMPORT(cv_line) int32_t cv_line(int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, int32_t color);

/*
 * An arc, clockwise from start to end. r: Outer radius.
 */
GADGET_IMPORT(cv_arc) int32_t cv_arc(int32_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t width, int32_t start, int32_t end, int32_t color);

/*
 * A filled triangle.
 */
GADGET_IMPORT(cv_triangle) int32_t cv_triangle(int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t color);

/*
 * Text, wrapped at w. y: The top of the first line. w: Wrap width. Returns the
 * height drawn.
 */
GADGET_IMPORT(cv_text) int32_t cv_text(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t font, int32_t color, int32_t align, const char *text);

/*
 * Copies pixels in, clipped to the canvas. data: w * h RGB565 pixels, row by
 * row, little-endian.
 */
GADGET_IMPORT(cv_pixels) int32_t cv_pixels(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, const void *data, int32_t data_len);

/* == Muse == */
/*
 * What Muse is doing, and the talk key.
 */

/*
 * What Muse is doing now; EV_MUSE says when it changes.
 */
GADGET_IMPORT(muse_mode) int32_t muse_mode(void);

/*
 * The caption the face would show: what Muse heard, or an error. Returns the
 * caption's full length.
 */
GADGET_IMPORT(muse_caption) int32_t muse_caption(void *buf, int32_t buf_size);

/*
 * With 1, the talk button is Muse's push-to-talk again, as outside apps, and
 * its presses don't come as EV_BUTTON. Off when the app starts.
 */
GADGET_IMPORT(muse_talk_key) void muse_talk_key(int32_t on);

/* == Chat == */
/*
 * Following the chat and sending to it, through the gadget's link to the VM.
 * The gadget parses the VM's formats; the app sees messages.
 */

/*
 * Starts following the chat: fetches the latest message, then reports new ones
 * as EV_CHAT. The gadget reconnects by itself. Needs the chat.read permission.
 */
GADGET_IMPORT(chat_open) int32_t chat_open(void);

/*
 * Stops following the chat and frees its messages. Needs the chat.read
 * permission.
 */
GADGET_IMPORT(chat_close) void chat_close(void);

/*
 * The app keeps the last chat_messages messages. n: 0 for the newest, 1 for
 * the one before... Returns A message id, or 0 if there's no such message.
 * Needs the chat.read permission.
 */
GADGET_IMPORT(chat_latest) int32_t chat_latest(int32_t n);

/*
 * About a message. id: A message id. Needs the chat.read permission.
 */
GADGET_IMPORT(chat_info) int32_t chat_info(int32_t id, int32_t key);

/*
 * A message's text, without attachments or markup the gadget can't show.
 * offset: Byte offset into the text. buf: Gets the text from offset, cut to
 * fit and NUL-terminated. Returns bytes copied, not counting the NUL. Needs
 * the chat.read permission.
 */
GADGET_IMPORT(chat_text) int32_t chat_text(int32_t id, int32_t offset, void *buf, int32_t buf_size);

/*
 * Sends a message to Muse, as "[<app> on the gadget] text". EV_DONE when the
 * VM has it; the reply comes through chat_open. Needs the chat.send
 * permission.
 */
GADGET_IMPORT(chat_send) int32_t chat_send(const char *text);

/* == Files == */
/*
 * Reading files on the VM (the app's own folder, or the home folder with the
 * files.home permission) and the gadget's own files on its flash, which Muse
 * puts there with files.put (paths from "/", with the files.gadget
 * permission).
 */

/*
 * Reads part of a file on the VM, or on the gadget itself (a path from "/").
 * EV_DONE's b is the bytes read, 0 at the end of the file, or ERR_NOT_FOUND.
 * path: Relative to the app's folder, from "~/" for the home folder
 * (files.home), or from "/" for the gadget's own files (files.gadget). buf:
 * Gets the bytes; keep it reserved until EV_DONE.
 */
GADGET_IMPORT(file_read) int32_t file_read(const char *path, int32_t offset, void *buf, int32_t buf_size);

/* == State == */
/*
 * Small files the app keeps between runs, on the VM under the gadget's folder
 * (devices/<gadget>/state/<app>/).
 */

/*
 * Saves a state file on the VM, replacing the last one whole. EV_DONE when
 * it's written. name: a-z, 0-9, '-', '_' and '.'. data: Copied during the
 * call.
 */
GADGET_IMPORT(state_save) int32_t state_save(const char *name, const void *data, int32_t data_len);

/*
 * Loads a state file. EV_DONE's b is its size, or ERR_NOT_FOUND. buf: Keep it
 * reserved until EV_DONE.
 */
GADGET_IMPORT(state_load) int32_t state_load(const char *name, void *buf, int32_t buf_size);

/* == Devices == */
/*
 * Sensors, motors, relays and anything else the firmware has a driver for. The
 * firmware describes each device: a name, its readings (fields with units),
 * its actions (with their parameters) and its events. Apps find devices by
 * name and read, stream or call them through these few functions, so new
 * hardware needs no new imports and no change to this file. Well-known names
 * (Devices, below) have fixed fields and units so apps work across boards; any
 * other name carries its own description.
 */

/*
 * With dev_name, lists them all. Returns how many devices this gadget has.
 */
GADGET_IMPORT(dev_count) int32_t dev_count(void);

/*
 * A device's name, by position. index: 0 to dev_count() - 1. buf: Gets the
 * name, NUL-terminated. Returns the name's length.
 */
GADGET_IMPORT(dev_name) int32_t dev_name(int32_t index, void *buf, int32_t buf_size);

/*
 * Finds a device by name. ERR_DENIED if the app's manifest names it in neither
 * requires nor uses. name: A well-known name (DEV_ constants) or one the
 * gadget lists. Returns the device.
 */
GADGET_IMPORT(dev_find) int32_t dev_find(const char *name);

/*
 * The device's description, as device.guide gives it (Devices, below). Apps
 * written for one device rarely need it: Muse already read it. buf: Gets the
 * description as JSON, cut to fit and NUL-terminated. Returns the
 * description's full length.
 */
GADGET_IMPORT(dev_describe) int32_t dev_describe(int32_t device, void *buf, int32_t buf_size);

/*
 * Where a field is in a reading. Returns the field's index in a reading's
 * values.
 */
GADGET_IMPORT(dev_field) int32_t dev_field(int32_t device, const char *name);

/*
 * The latest reading, without opening the device. ERR_NOT_FOUND until there is
 * one: a sensor that streams starts on the first dev_read (taking a moment),
 * and runs until the app stops or calls dev_close. Its t_ms says how fresh it
 * is. To get every reading, use dev_open and dev_take instead. reading: One
 * gadget_reading_t. Returns how many values it holds.
 */
GADGET_IMPORT(dev_read) int32_t dev_read(int32_t device, void *reading, int32_t reading_size);

/*
 * Starts streaming readings into a queue, and the device's events. EV_DEV_DATA
 * says when readings wait, EV_DEV_EVENT brings events. Close it once the app
 * stops needing it: a running sensor costs battery. rate_hz: Up to the
 * device's rate; 0 for events only.
 */
GADGET_IMPORT(dev_open) int32_t dev_open(int32_t device, int32_t rate_hz);

/*
 * Takes waiting readings off the queue. readings: gadget_reading_t array.
 * Returns readings copied, oldest first.
 */
GADGET_IMPORT(dev_take) int32_t dev_take(int32_t device, void *readings, int32_t readings_size);

/*
 * Stops streaming and events.
 */
GADGET_IMPORT(dev_close) int32_t dev_close(int32_t device);

/*
 * Finds an action by name. Returns the action's index.
 */
GADGET_IMPORT(dev_action) int32_t dev_action(int32_t device, const char *name);

/*
 * Runs an action. It doesn't block: a driver whose action takes time starts it
 * and returns. action: Its index. params: float parameters, in the order the
 * description gives. text: For actions that take text; "" otherwise. Returns
 * the action's result, 0 or more.
 */
GADGET_IMPORT(dev_call) int32_t dev_call(int32_t device, int32_t action, const void *params, int32_t params_len, const char *text);

/*
 * Finds an event by name. Returns the event's index, as EV_DEV_EVENT gives it.
 */
GADGET_IMPORT(dev_event) int32_t dev_event(int32_t device, const char *name);

/* == What the app exports == */

/*
 * Required. Called once the app is loaded: build the screen here.
 */
/* GADGET_EXPORT(on_start) void on_start(void); */

/*
 * Called for each event (EV_ constants; the table under Events says what a and
 * b hold).
 */
/* GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b); */

/*
 * Called up to sys_frame_rate times a second while the screen is on, with
 * sys_millis(). Leave it out unless the app animates something itself:
 * widgets, animations and timers don't need it.
 */
/* GADGET_EXPORT(on_frame) void on_frame(int32_t now_ms); */

/*
 * Called before the app is closed, unless it trapped. Save state here:
 * state_save calls made in on_stop finish (for up to 2 s) before the app's
 * memory is freed.
 */
/* GADGET_EXPORT(on_stop) void on_stop(int32_t reason); */

#endif /* GADGET_H */
