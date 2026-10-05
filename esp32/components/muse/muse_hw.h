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
#include <stdint.h>

#include "sdkconfig.h"

/*
 * The board's hardware as Muse's agent reaches it (main/muse_hw_commands.c):
 * touch and wheel events it can read, input capture, and the RGB light. The
 * face and settings keep working alongside. Only while Muse captures input do
 * taps and the wheel stop reaching them. Every call is safe from any task.
 * Without CONFIG_MUSE_HW_COMMANDS, the hooks the board, input and UI call
 * (below) do nothing, and nothing else here exists.
 */

typedef enum {
    MUSE_HW_TAP,
    MUSE_HW_LONG_PRESS,
    MUSE_HW_SWIPE,
    MUSE_HW_WHEEL_CLICK,
    MUSE_HW_WHEEL_TURN,
    MUSE_HW_BUTTON,         /* a button on a display.show_ui screen; id names it */
    MUSE_HW_DETECTION,      /* the camera saw something (camera.watch); id is its label */
    MUSE_HW_UI,             /* something done on an app's page (muse_apps.h) */
    MUSE_HW_PET,            /* the pet (muse_pet.h): id is what happened, value a level */
} muse_hw_event_type_t;

typedef struct {
    uint32_t seq;           /* counts up from 1 */
    int64_t us;             /* esp_timer time */
    muse_hw_event_type_t type;
    int16_t x, y;           /* touch: where the finger went down */
    int16_t x2, y2;         /* swipe: where it lifted */
    int32_t value;          /* long press, wheel click: ms held; wheel turn: steps, + clockwise;
                               detection: score */
    char id[16];            /* button: its id; detection: its label; ui: the widget's ("" for the page) */
    char app[16];           /* ui: the app */
    char ui_event[12];      /* ui: click, long_press, change, submit, show, hide */
    char text[48];          /* ui: a roller's option, an input's text */
} muse_hw_event_t;

/* "tap", "long_press", "swipe", "wheel_click", "wheel_turn", "button", "detection", "ui", "pet". */
const char *muse_hw_event_name(muse_hw_event_type_t type);
/* A swipe's direction from its start and end: "left", "right", "up" or "down". */
const char *muse_hw_swipe_dir(const muse_hw_event_t *ev);

#if CONFIG_MUSE_HW_COMMANDS
/* From the board: every touch poll while a finger is down, and one at the lift. */
void muse_hw_touch(bool down, int x, int y);
/* From the board: the wheel's push edges, and whole turns once it rests. */
void muse_hw_wheel_push(bool down);
void muse_hw_wheel_turn(int steps);
/* From the UI: a display.show_ui button was pressed. */
void muse_hw_button(const char *id);
#else
static inline void muse_hw_touch(bool down, int x, int y) { (void)down; (void)x; (void)y; }
static inline void muse_hw_wheel_push(bool down) { (void)down; }
static inline void muse_hw_wheel_turn(int steps) { (void)steps; }
static inline void muse_hw_button(const char *id) { (void)id; }
#endif
/* From the camera: `label` seen with `score`, its box x, y, w, h (all 0 without one). */
void muse_hw_detection(const char *label, int score, int x, int y, int w, int h);
/* From an app's page: `event` on `widget` ("" for the page), with a value and text (may be NULL). */
void muse_hw_ui(const char *app, const char *widget, const char *event, int value, const char *text);
/* From the pet: a need ("hungry"), what befell it ("poop", "sick") or the care it had ("feed"). */
void muse_hw_pet(const char *what, int value);

/* Called with each event as it happens, on the task that raised it (the touch,
 * input, UI or camera task): must return quickly. For on-device scripts. */
typedef void (*muse_hw_listener_fn)(const muse_hw_event_t *ev);
void muse_hw_set_listener(muse_hw_listener_fn fn);

/* The newest event's seq, 0 before the first. */
uint32_t muse_hw_last_seq(void);
/* Copies up to max events newer than `after`, oldest first, waiting up to
 * wait_ms for the first. Returns the count. Only the last 32 are kept. */
int muse_hw_events(uint32_t after, muse_hw_event_t *out, int max, int wait_ms);

/* Until esp_timer time until_us, taps and the wheel reach only Muse's agent:
 * the UI doesn't see them and the wheel neither talks nor sleeps. 0 ends it. */
void muse_hw_capture(int64_t until_us);
#if CONFIG_MUSE_HW_COMMANDS
bool muse_hw_captured(void);
#else
static inline bool muse_hw_captured(void) { return false; }
#endif

/*
 * The serial console's ">hw {json}" lines (tools/muse/hw.py) run a hardware
 * command the way the agent would, without the cloud. Link sets the handler
 * (main/muse_hw_commands.c); it prints "@hw {...}" lines with the results.
 * False if none is set.
 */
typedef void (*muse_hw_console_fn)(const char *json);
void muse_hw_set_console(muse_hw_console_fn fn);
#if CONFIG_MUSE_HW_COMMANDS
bool muse_hw_console(const char *json);
#else
static inline bool muse_hw_console(const char *json) { (void)json; return false; }
#endif

typedef enum {
    MUSE_HW_LED_OFF,
    MUSE_HW_LED_SOLID,
    MUSE_HW_LED_BLINK,
    MUSE_HW_LED_BREATHE,
} muse_hw_led_mode_t;

/* False on a board without the light (muse_board->set_led). period_ms is one
 * blink or breath. */
bool muse_hw_led(uint8_t r, uint8_t g, uint8_t b, muse_hw_led_mode_t mode, int period_ms);
bool muse_hw_has_led(void);
