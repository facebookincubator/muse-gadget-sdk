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

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Live call subtitles (mg_command_subtitle, mgcommands.h, Subtitles) ---- */
typedef struct {
    uint8_t flags;               /* mg_subtitle_cap_* bits: no_speaker, frame/ms timing, ... */
    uint8_t lines;               /* text lines drawn */
    uint8_t chars_per_line;      /* about, at its font */
    uint16_t max_words_bytes;    /* largest words payload taken */
} mg_subs_caps_t;

typedef struct {
    uint32_t show_at;
    const uint8_t *utf8;
    uint8_t len;
} mg_subs_word_t;

/* Words handed to the UI in one event: a words message carries up to 255 (its count byte), delivered in runs of
 * this many with consecutive offsets (replace on the first run only). */
#define MG_SUBS_MAX_WORDS 64

typedef struct {
    uint8_t sub;                 /* the sub-command: state 2, words 3, reply_end 4, clear 5, clock 6 */
    uint8_t active, timing;      /* state */
    uint16_t reply_id;           /* words, reply_end, clear, clock */
    uint8_t flags;               /* words: bit0 final, bit1 replace */
    uint16_t char_offset;        /* words */
    uint8_t count;               /* words */
    const mg_subs_word_t *words; /* words */
    uint32_t show_at;            /* reply_end */
    uint8_t reason;              /* clear */
    uint32_t playout_ms;         /* clock */
    bool proven;                 /* the central's token proof matched (or the gadget isn't set up), or the console's */
} mg_subs_event_t;

#ifdef __cplusplus
}
#endif
