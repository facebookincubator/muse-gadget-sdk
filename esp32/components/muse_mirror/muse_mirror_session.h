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
#include "esp_err.h"

#define MUSE_MIRROR_MSG_MAX (32 * 1024)   /* the largest message a viewer is sent, on any transport */

typedef struct {
    int role;
    const char *name;
    int x, y;
} muse_mirror_button_t;

typedef struct {
    const char *name;
    int width, height;
    bool round, touch, keyboard;
    bool audio;   /* a microphone or a speaker */
    bool uart;    /* the console is a UART: sound only in a session that raised its speed */
    float diagonal;
    const muse_mirror_button_t *buttons;
    size_t button_count;
    void (*on_key)(int key, bool down);
} muse_mirror_config_t;

esp_err_t muse_mirror_session_start(const muse_mirror_config_t *config);
bool muse_mirror_connected(void);
bool muse_mirror_pointer_read(int *x, int *y, bool *down);
/* The caller releases the framebuffer lock through frame_end. */
uint16_t *muse_mirror_frame_begin(void);
void muse_mirror_frame_end(int x, int y, int width, int height, bool last);   /* last: LVGL's refresh is whole */
void muse_mirror_set_screen_state(uint8_t state);
/* LVGL context: redraw before publishing a newly admitted viewer's frames. */
bool muse_mirror_capture_begin(unsigned *generation);
void muse_mirror_capture_ready(unsigned generation);

/* UI task: report a focused field and consume queued edits in order. */
#define MUSE_MIRROR_TEXT_INSERT    0
#define MUSE_MIRROR_TEXT_BACKSPACE 1
#define MUSE_MIRROR_TEXT_DELETE    2
#define MUSE_MIRROR_TEXT_LEFT      3
#define MUSE_MIRROR_TEXT_RIGHT     4
#define MUSE_MIRROR_TEXT_ENTER     5

typedef struct {
    uint8_t edit;
    char text[64];
} muse_mirror_text_t;

void muse_mirror_text_open(bool open);
bool muse_mirror_text_read(muse_mirror_text_t *out);
