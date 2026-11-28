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
 * UI timing for the bench (CONFIG_MUSE_PERF, in devices/sdkconfig.muse-bench),
 * logged on mg.perf, so it also reaches the Nordic UART mirror:
 *
 * - every CONFIG_MUSE_PERF_PERIOD_MS: frames drawn and their rate, frame time
 *   split into render, waiting for the panel and the flush callback, pixels
 *   per frame, the avatar's frame tick, free internal RAM, and each task's
 *   share of the CPU over the period;
 * - each touch: touch interrupt -> LVGL reads it -> first flush -> frame done;
 * - each talk press: press -> mode change -> the UI picks it up -> first
 *   flush -> frame done.
 *
 * Bench console commands (muse_perf_console, from '>' lines) inject touch
 * gestures through the touch input's read callback, so LVGL's whole path runs
 * as for a finger (gesture detection, scrolling, the tileview's snap), with the
 * real controller ignored meanwhile:
 *
 *   swipe=left|right|up|down[,ms]   across the middle 3/4 of the screen (250 ms)
 *   drag=x0,y0,x1,y1,ms             press, move, release
 *   mark=text                       a marker line in the log
 *   bufs=N                          refused: the buffers are the adapter's;
 *                                   rebuild with another height
 * (A tap is the bench's press= or tap=<control>, muse_input.c.)
 *   cf=native|swapped               draw RGB565 or byte-swapped RGB565
 *   prof=on|off                     also log where each gesture's render time
 *                                   went, by LVGL function and draw task type
 *
 * Each gesture logs one line from its press to 300 ms after the last frame of
 * its scroll or animation: frames and rate, frame time (avg, p95, max; render,
 * panel wait, flush), pixels, the longest stretch without a flush, slow frames,
 * the pages it went between, and each task's CPU share meanwhile.
 *
 * Without CONFIG_MUSE_PERF every call is an empty inline.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_MUSE_PERF
#include "lvgl.h"

/* Under the display lock, once the display and touch are up. */
void muse_perf_start(lv_display_t *disp, lv_indev_t *touch);
/* From the touch controller's interrupt (IRAM-safe). */
void muse_perf_touch_irq(void);
/* The talk button went down (the input task saw it). */
void muse_perf_talk_press(void);
/* muse_state's mode changed. */
void muse_perf_mode(int mode);
/* The UI's frame tick saw the new mode, and how long one tick took. */
void muse_perf_face(void);
void muse_perf_ui_tick(int64_t us);
/* A bench console line ("swipe=left"); false if it isn't one of these. */
bool muse_perf_console(const char *line);
#else
#define muse_perf_start(disp, touch) ((void)(disp), (void)(touch))
static inline void muse_perf_touch_irq(void) {}
static inline void muse_perf_talk_press(void) {}
static inline void muse_perf_mode(int mode) { (void)mode; }
static inline void muse_perf_face(void) {}
static inline void muse_perf_ui_tick(int64_t us) { (void)us; }
static inline bool muse_perf_console(const char *line) { (void)line; return false; }
#endif
