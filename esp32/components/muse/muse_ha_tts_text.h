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

/* The parts of muse_ha_tts.c that don't touch the network, so the host tests
 * (tests/test_muse_ha_tts.py) can run them. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A reply's whole text, for speech: the captions keep only TEXT_MAX of it. */
typedef struct {
    char *buf;               /* NUL-terminated, or NULL until the first append */
    size_t len, cap;
    bool cut;                /* it hit the cap, so the end is missing */
} muse_ha_tts_speech_t;

/* Appends `text`, growing the buffer with `grow` (realloc's contract) up to
 * `max` bytes with the NUL. Past that, or if `grow` fails, it keeps what fits,
 * never splitting a UTF-8 character, sets `cut` and returns false. */
bool muse_ha_tts_speech_append(muse_ha_tts_speech_t *s, const char *text, size_t max,
                               void *(*grow)(void *, size_t));

/* Frees the buffer with `release` and empties it. */
void muse_ha_tts_speech_free(muse_ha_tts_speech_t *s, void (*release)(void *));

/* Readies a reply for speech, in place: links keep their label, Markdown's
 * *, # and ` go, and newlines become spaces. */
void muse_ha_tts_clean(char *text);

/* How many bytes of an ID3v2 tag start the MP3 (0 if none, or if fewer than
 * its 10-byte header have arrived). */
size_t muse_ha_tts_id3_size(const uint8_t *data, size_t len);

/* Copies `base` without trailing slashes. */
void muse_ha_tts_base(char *out, size_t cap, const char *base);

/* Where to fetch tts_get_url's MP3: `path` on `base` when HA gave one, as
 * HA's `url` is built from its own idea of its address, which may be one this
 * device can't reach; else `url`. False when there's neither, or it doesn't fit. */
int muse_ha_tts_audio_url(char *out, size_t cap, const char *base, const char *path, const char *url);
