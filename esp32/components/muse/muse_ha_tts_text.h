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

/* Sorts out the start of HA's audio however it's split across reads: holds
 * the first 10 bytes until they're all in, refuses WAV, skips an ID3v2 tag,
 * checks that what follows starts with an MP3 frame (an HTML error page
 * doesn't), and passes the audio to `emit`. Zero it to start. */
typedef struct {
    uint8_t head[10];
    size_t have;             /* of head, until started */
    size_t skip;             /* what's left of the ID3 tag */
    bool started;
    uint8_t first;           /* the audio's first byte, held until the second shows it's MP3 */
    bool held, synced;
} muse_ha_tts_mp3_t;

typedef enum {
    MUSE_HA_TTS_MP3_OK,
    MUSE_HA_TTS_MP3_WAV,     /* it's WAV: stop */
    MUSE_HA_TTS_MP3_STOPPED, /* emit returned false */
    MUSE_HA_TTS_MP3_NOT_MP3, /* no MP3 frame where the audio starts: stop */
} muse_ha_tts_mp3_result_t;

typedef bool (*muse_ha_tts_emit_t)(void *ctx, const uint8_t *data, size_t len);

/* Takes the next `len` bytes of the download. */
muse_ha_tts_mp3_result_t muse_ha_tts_mp3_feed(muse_ha_tts_mp3_t *m, const uint8_t *data, size_t len,
                                              muse_ha_tts_emit_t emit, void *ctx);

/* The download ended: sorts out a start too short to have been, and says
 * NOT_MP3 if no audio came at all. */
muse_ha_tts_mp3_result_t muse_ha_tts_mp3_end(muse_ha_tts_mp3_t *m, muse_ha_tts_emit_t emit, void *ctx);

/* Copies `base` without trailing slashes. */
void muse_ha_tts_base(char *out, size_t cap, const char *base);

/* Where to fetch tts_get_url's MP3: `path` on `base` when HA gave one, as
 * HA's `url` is built from its own idea of its address, which may be one this
 * device can't reach; else `url`. False when there's neither, or it doesn't fit. */
int muse_ha_tts_audio_url(char *out, size_t cap, const char *base, const char *path, const char *url);
