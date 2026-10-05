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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/*
 * Apps: pages Muse's agent or a script adds below the face, swiped up to
 * (the face and settings keep left and right). Each is built from a JSON
 * description, a tree of widgets with ids:
 *
 *   {"id": "focus", "title": "Focus", "bg": "#101820", "order": 1,
 *    "children": [{"type": "label", "id": "t", "text": "25:00", "font": 48},
 *                 {"type": "button", "id": "go", "text": "Start"}]}
 *
 * Widgets change in place by id (muse_apps_update), and report what's done
 * to them (a click, a slider's value) to muse_hw as MUSE_HW_UI events, as a
 * page does when it's shown or hidden. main/muse_hw_apps.c keeps the
 * descriptions and turns this into agent commands; the format is documented
 * there. Every call is safe from any task.
 */

#define MUSE_APPS_MAX 16

/* Builds app def["id"], replacing one of that id. False with err set. */
bool muse_apps_define(const cJSON *def, char *err, size_t errlen);

/* set: {"widget_id": {prop: value, ...}, ...}; unknown ids are skipped. */
bool muse_apps_update(const char *app, const cJSON *set, char *err, size_t errlen);

bool muse_apps_remove(const char *app);

/* Slides to the app's page, or a built-in one: "face" (or NULL or ""),
 * "pet" or "settings". */
bool muse_apps_show(const char *app);

/* [{id, title, builtin, shown}] for the built-in pages, then
 * [{id, title, order, widgets, shown}] for the apps. */
cJSON *muse_apps_list(void);

/* An image widget's picture from a JPEG, shrunk (1/2, 1/4, 1/8) to fit the
 * widget and centred: for "sd:" and "url:" sources, fetched elsewhere, and
 * for camera frames. False if there's no such image widget or it won't decode. */
bool muse_apps_set_jpeg(const char *app, const char *widget, const uint8_t *jpeg, size_t len);

/* Fetches an image widget's "sd:" or "url:" source and calls
 * muse_apps_set_jpeg: set by main/muse_hw_apps.c, called from any task. */
typedef void (*muse_apps_loader_fn)(const char *app, const char *widget, const char *src);
void muse_apps_set_loader(muse_apps_loader_fn fn);

/* From the UI each settings tick, with the display lock held: page dots,
 * and show/hide events. */
void muse_apps_tick(void);

/* From the UI as the screen goes dark, with the display lock held: the page
 * on screen gets "hide" (its script can stop its camera or mic), the keyboard
 * closes, and waking brings "show" again. */
void muse_apps_screen_off(void);

/* A script has started: the app page on screen gets "show" again at the next
 * tick, now that its handlers are there to fill it. Only sets a flag, so it's
 * safe from any task, the scripting task's PSRAM stack included. */
void muse_apps_reshow(void);

/* The keyboard is open on an app's input: the pages mustn't slide away. */
bool muse_apps_typing(void);
