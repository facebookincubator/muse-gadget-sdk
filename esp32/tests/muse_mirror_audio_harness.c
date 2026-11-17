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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "muse_mirror_audio.h"

static bool fail_alloc;
void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return fail_alloc ? NULL : malloc(size); }
void heap_caps_free(void *p) { free(p); }

static int16_t ramp[4000];
static uint8_t chunk[MUSE_MIRROR_AUDIO_CHUNK];

static int16_t sample(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static void mic_put(const int16_t *pcm, size_t samples)
{
    uint8_t wire[2 * 1601 + 1];
    for (size_t i = 0; i < samples; i++) {
        wire[1 + 2 * i] = (uint8_t)pcm[i];
        wire[2 + 2 * i] = (uint8_t)((uint16_t)pcm[i] >> 8);
    }
    muse_mirror_audio_mic_put(wire + 1, samples);   /* an odd address, as in the message */
}

int main(void)
{
    uint8_t source = 9;
    for (int i = 0; i < 4000; i++) ramp[i] = (int16_t)(i - 2000);

    /* Before the rings exist nothing is kept and nothing is routed. */
    fail_alloc = true;
    assert(!muse_mirror_audio_init());
    muse_mirror_audio_listen(MUSE_MIRROR_AUDIO_SPEAKER);
    muse_mirror_audio_speaker(true);
    assert(!muse_mirror_audio_playback(ramp, 320));
    assert(!muse_mirror_audio_take(chunk, &source));
    fail_alloc = false;
    assert(muse_mirror_audio_init() && muse_mirror_audio_init());

    /* Nobody listens: playback passes through, heard on the board. */
    assert(!muse_mirror_audio_playback(ramp, 320));
    assert(!muse_mirror_audio_take(chunk, &source));

    /* The speaker for the viewer: whole chunks, in order, across calls of any length. */
    muse_mirror_audio_listen(MUSE_MIRROR_AUDIO_SPEAKER);
    assert(!muse_mirror_audio_playback(ramp, 100));
    assert(!muse_mirror_audio_take(chunk, &source));
    assert(!muse_mirror_audio_playback(ramp + 100, 700));
    for (int k = 0; k < 2; k++) {
        assert(muse_mirror_audio_take(chunk, &source) && source == MUSE_MIRROR_AUDIO_SPEAKER);
        for (int i = 0; i < 320; i++) assert(sample(chunk + 2 * i) == ramp[320 * k + i]);
    }
    assert(!muse_mirror_audio_take(chunk, &source));

    /* Captured samples are not what this viewer asked for. */
    int16_t mic[320];
    memcpy(mic, ramp, sizeof(mic));
    muse_mirror_audio_capture(mic, 320);
    assert(!memcmp(mic, ramp, sizeof(mic)));
    assert(!muse_mirror_audio_take(chunk, &source));

    /* The board stays silent only while asked to. */
    muse_mirror_audio_speaker(true);
    assert(muse_mirror_audio_playback(ramp, 160));
    muse_mirror_audio_speaker(false);
    assert(!muse_mirror_audio_playback(ramp, 160));
    assert(muse_mirror_audio_take(chunk, &source));     /* 160 left over, 160 and 160 */

    /* A viewer that fell further behind than the ring hears the newest half of it. */
    for (int i = 0; i < 60; i++) muse_mirror_audio_playback(ramp + i, 320);
    assert(muse_mirror_audio_take(chunk, &source));
    int got = 1;
    while (muse_mirror_audio_take(chunk, &source)) got++;
    assert(got == 8192 / MUSE_MIRROR_AUDIO_CHUNK);
    assert(sample(chunk + 638) != 0);

    /* A new source starts from now. */
    muse_mirror_audio_playback(ramp, 320);
    muse_mirror_audio_listen(MUSE_MIRROR_AUDIO_MIC);
    assert(!muse_mirror_audio_take(chunk, &source));
    muse_mirror_audio_playback(ramp, 320);
    assert(!muse_mirror_audio_take(chunk, &source));
    muse_mirror_audio_capture(mic, 320);
    assert(muse_mirror_audio_take(chunk, &source) && source == MUSE_MIRROR_AUDIO_MIC);
    assert(sample(chunk) == ramp[0] && sample(chunk + 638) == ramp[319]);
    muse_mirror_audio_listen(3);                        /* no such source: unchanged */
    muse_mirror_audio_capture(mic, 320);
    assert(muse_mirror_audio_take(chunk, &source));

    /* The viewer's microphone with the board's, clipped. */
    int16_t voice[320], heard[320];
    for (int i = 0; i < 320; i++) voice[i] = (int16_t)(i < 2 ? 30000 : 100);
    mic_put(voice, 320);                                /* routing is off: dropped */
    memcpy(heard, voice, sizeof(heard));
    muse_mirror_audio_capture(heard, 320);
    assert(!memcmp(heard, voice, sizeof(heard)));
    muse_mirror_audio_mic(1);
    mic_put(voice, 320);
    heard[5] = 7;
    muse_mirror_audio_capture(heard, 320);              /* 20 ms is not the 60 ms it waits for: the board's alone */
    assert(heard[5] == 7);
    mic_put(voice, 320);
    mic_put(voice, 320);
    heard[0] = 30000; heard[1] = -30000; heard[2] = 5;
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 32767 && heard[1] == 0 && heard[2] == 105 && heard[319] == 200);
    for (int k = 0; k < 3; k++) assert(muse_mirror_audio_take(chunk, &source));
    assert(sample(chunk) == 32767 && sample(chunk + 4) == 105);   /* the viewer hears the mix */

    /* Instead of the board's, read a little slower as the cushion runs low; silence where the viewer ran
     * dry, then 60 ms gathered again. */
    muse_mirror_audio_mic(2);
    for (int i = 0; i < 320; i++) voice[i] = 100;
    for (int k = 0; k < 3; k++) mic_put(voice, 320);
    for (int k = 0; k < 3; k++) {
        muse_mirror_audio_capture(heard, 320);
        for (int i = 0; i < 320; i++) assert(heard[i] == 100);   /* 960 samples last three reads, stretched */
    }
    for (int i = 0; i < 320; i++) heard[i] = 1234;
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 100 && heard[319] == 0);          /* the last few, then silence */
    mic_put(voice, 320);
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 0 && heard[319] == 0);           /* 20 ms is not the 60 ms it gathers again */

    /* More than the ring holds: the newest 100 ms, read a little faster until the cushion is back. */
    for (int k = 0; k < 10; k++) {
        for (int i = 0; i < 320; i++) voice[i] = (int16_t)(k * 320 + i);
        mic_put(voice, 320);
    }
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 10 * 320 - 1600 && heard[319] > heard[0] + 319 && heard[319] < heard[0] + 325);
    mic_put(ramp, 1601);                                /* longer than the ring keeps: dropped */
    muse_mirror_audio_mic(3);                           /* no such mode: unchanged */
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 10 * 320 - 1600 + 324);

    /* The viewer left: the board plays and records as before. */
    muse_mirror_audio_speaker(true);
    muse_mirror_audio_reset();
    assert(!muse_mirror_audio_playback(ramp, 320));
    for (int i = 0; i < 320; i++) heard[i] = 1234;
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 1234 && heard[319] == 1234);
    assert(!muse_mirror_audio_take(chunk, &source));
    muse_mirror_audio_mic(1);                           /* a new viewer hears none of the last one */
    muse_mirror_audio_capture(heard, 320);
    assert(heard[0] == 1234);
    return 0;
}
