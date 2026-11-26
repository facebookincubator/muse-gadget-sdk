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

/*
 * Live call subtitles on the screen
 * (mg_command_subtitle, protocols/mgcommands.h, Live call subtitles). The
 * model is muse_subs_core.h; this draws it: a band at the bottom of the
 * panel's safe area, above the mic ring (which stays on top), white text on a
 * translucent dark backdrop, in the board's largest font that fits. During
 * video it's two lines blended into each frame's pieces on their way to the
 * panel (muse_video.c), revealed with the frame that carries each word's
 * sound; without video LVGL draws it on its system layer as the tall band:
 * as many lines as fit up to the top row or the clock,
 * growing as words come, newest at the bottom, scrolling a line at a time.
 */

#include <stdbool.h>
#include <stdint.h>

#include "mg_subs.h"
#include "muse_board_win.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the band up for the board (muse_ui.c, LVGL locked, after the UI is built). */
void muse_subs_ui_init(void);
/* What this board draws (mg_command_subtitle capabilities), or NULL: none. */
const mg_subs_caps_t *muse_subs_caps(void);
bool muse_subs_ready(void);   /* the band's buffers are allocated: subtitles can be drawn */
/* The band is on screen now. Any task. */
bool muse_subs_band_shown(void);
/* ...and its words are the owner's (a proven central's, or the console's): it holds a lit panel until it goes.
 * An unproven central's band never does. Any task. */
bool muse_subs_band_holds(void);
/* A card that asks for a press or a code is up or gone (LVGL's task, every frame and as the card changes): the
 * pairing confirm, the update prompt, the re-setup prompt, a passcode keypad. While it's up nothing of the band
 * draws, neither the tall band (hidden at once) nor the video's, so no caption ever lies over a card's Confirm. An
 * unproven central's words are cleared as a card comes up, so none of them comes back beside a prompt. */
void muse_subs_card(bool up);
/* Anything of the band is on the panel now: the tall band, or the video's (LVGL's task). */
bool muse_subs_band_drawn(void);
/* A message from the phone (the BLE worker). */
void muse_subs_event(const mg_subs_event_t *ev);
/* The call ended: the band goes. */
void muse_subs_call_end(void);
/* Video: a stream started (frame numbers count from it again), the frame now on the panel, no more video. */
void muse_subs_video_start(void);
void muse_subs_frame_shown(uint32_t frame32);
void muse_subs_video_stop(void);
/* The video handed the panel back to LVGL with its stream still open (a stall, the passcode hold): the tall band
 * shows the words until the next picture is drawn (muse_subs_frame_shown takes it back). */
void muse_subs_video_yield(void);
/* Blends the band into a piece of a frame (muse_video_overlay_t's arguments). */
void muse_subs_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows);
/* Where muse_subs_overlay draws now (panel coordinates; false: nothing), and a count that changes whenever what it
 * draws does (changed-only redraw of call video). Any task. */
bool muse_subs_overlay_rect(muse_board_win_t *r, uint32_t *gen);
/* LVGL's frame tick: the band as an LVGL object while no video draws. */
void muse_subs_tick(void);
/* Bench (>sub=...): a reply as if from the phone, one word every ms_per_word (0: all now). */
/* Bench: keep the video frame showing each reply's whole text (>shot.auto=1). */
void muse_subs_auto_shot(bool on);
/* A talk press's touch-down (on) pauses the band with the reply's audio: hidden, its words kept and still coming
 * due. A tap resumes it (off) where the reply now is; a pause nobody ends lapses as the audio's does. */
void muse_subs_talk_pause(bool on);
/* The gadget's own push-to-talk turn (the press claimed the turn): the band
 * clears at once (the phone's clear may come later), and the rest of that
 * reply is ignored. */
void muse_subs_talk_press(void);
void muse_subs_bench(const char *text, int ms_per_word);

#ifdef __cplusplus
}
#endif
