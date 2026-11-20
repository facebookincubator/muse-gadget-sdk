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
#include "muse_mirror_audio.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "muse_adpcm.h"
#include "muse_mem.h"

#define OUT_RING 16384          /* bytes for the viewer: half a second */
#define MIC_RING 2048           /* samples from the viewer: 128 ms */
#define MIC_KEEP 1600           /* a full ring keeps the newest 100 ms */
#define MIC_PRIME 960           /* 60 ms gathered before the viewer's microphone is used, and again after it runs dry */
#define PIECE    320            /* samples handled under the lock at a time */
_Static_assert((OUT_RING & (OUT_RING - 1)) == 0 && OUT_RING >= 4 * MUSE_MIRROR_AUDIO_CHUNK, "OUT_RING");
_Static_assert((MIC_RING & (MIC_RING - 1)) == 0 && MIC_PRIME < MIC_KEEP && MIC_KEEP < MIC_RING, "MIC_RING");

/* Written by the audio path, read by the mirror task, and the other way
 * round for the microphone: byte and sample counts so far, under s_mux. */
static uint8_t *s_out;
static int16_t *s_mic;
static uint8_t *s_mic_pcm;             /* a decoded ADPCM chunk from the viewer */
static uint32_t s_out_w, s_out_r, s_mic_w, s_mic_r;
static atomic_uchar s_source, s_mic_mode;
static atomic_bool s_viewer_only;
static bool s_mic_primed;               /* under s_mux */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

bool muse_mirror_audio_init(void)
{
    if (s_out) return true;
    uint8_t *out = heap_caps_malloc(OUT_RING, MUSE_BIG_CAPS);
    int16_t *mic = heap_caps_malloc(MIC_RING * sizeof(int16_t) + MUSE_MIRROR_AUDIO_CHUNK, MUSE_BIG_CAPS);
    if (!out || !mic) {
        heap_caps_free(out);
        heap_caps_free(mic);
        return false;
    }
    s_mic_pcm = (uint8_t *)(mic + MIC_RING);
    s_mic = mic;
    s_out = out;
    return true;
}

static void out_put(uint8_t source, const int16_t *pcm, size_t samples)
{
    while (samples && s_out && atomic_load(&s_source) == source) {
        size_t n = (samples < PIECE ? samples : PIECE) * 2;
        portENTER_CRITICAL(&s_mux);
        if (atomic_load(&s_source) == source) {
            size_t at = s_out_w % OUT_RING, first = OUT_RING - at < n ? OUT_RING - at : n;
            memcpy(s_out + at, pcm, first);
            memcpy(s_out, (const uint8_t *)pcm + first, n - first);
            s_out_w += n;
        }
        portEXIT_CRITICAL(&s_mux);
        pcm += n / 2;
        samples -= n / 2;
    }
}

void muse_mirror_audio_capture(int16_t *mono, size_t frames)
{
    uint8_t mode = atomic_load(&s_mic_mode);
    for (size_t at = 0; mode && s_mic && at < frames; at += PIECE) {
        size_t want = frames - at < PIECE ? frames - at : PIECE;
        portENTER_CRITICAL(&s_mux);
        uint32_t have = s_mic_w - s_mic_r;
        if (!s_mic_primed && have >= MIC_PRIME) s_mic_primed = true;
        /* The board's clock and the computer's differ: a ring drifting from its cushion is read about 1%
         * slower or faster, interpolated, to steer back, rather than running dry into silence or overflowing. */
        size_t take = want;
        if (have < MIC_PRIME) take = want - want / 100 - 1;
        else if (have > MIC_KEEP - PIECE) take = want + want / 100 + 1;
        bool whole = s_mic_primed && have > take;
        size_t n = !s_mic_primed ? 0 : whole ? want : have;
        if (!whole) s_mic_primed = false;
        for (size_t i = 0; i < n; i++) {
            uint32_t pos = whole ? (uint32_t)(i * take * 256 / want) : i * 256, k = pos >> 8, f = pos & 255;
            int32_t a = s_mic[(s_mic_r + k) % MIC_RING], b = whole ? s_mic[(s_mic_r + k + 1) % MIC_RING] : a;
            int32_t v = a + (b - a) * (int32_t)f / 256 + (mode == 2 ? 0 : mono[at + i]);
            mono[at + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        s_mic_r += whole ? take : n;
        portEXIT_CRITICAL(&s_mux);
        if (mode == 2) memset(mono + at + n, 0, (want - n) * sizeof(*mono));
    }
    out_put(MUSE_MIRROR_AUDIO_MIC, mono, frames);
}

bool muse_mirror_audio_playback(const int16_t *mono, size_t frames)
{
    out_put(MUSE_MIRROR_AUDIO_SPEAKER, mono, frames);
    return atomic_load(&s_viewer_only);
}

void muse_mirror_audio_listen(uint8_t source)
{
    if (source > MUSE_MIRROR_AUDIO_SPEAKER || !s_out) return;
    portENTER_CRITICAL(&s_mux);
    if (atomic_exchange(&s_source, source) != source) s_out_r = s_out_w;
    portEXIT_CRITICAL(&s_mux);
}

void muse_mirror_audio_speaker(bool viewer_only)
{
    atomic_store(&s_viewer_only, viewer_only && s_out);
}

void muse_mirror_audio_mic(uint8_t mode)
{
    if (mode > 2 || !s_mic) return;
    portENTER_CRITICAL(&s_mux);
    if (atomic_exchange(&s_mic_mode, mode) != mode) {
        s_mic_r = s_mic_w;
        s_mic_primed = false;
    }
    portEXIT_CRITICAL(&s_mux);
}

void muse_mirror_audio_mic_put(const uint8_t *pcm, size_t samples)
{
    if (!s_mic || !atomic_load(&s_mic_mode) || samples > MIC_KEEP) return;
    portENTER_CRITICAL(&s_mux);
    if (s_mic_w - s_mic_r + samples > MIC_RING) s_mic_r = s_mic_w + samples - MIC_KEEP;
    for (size_t i = 0; i < samples; i++) {
        s_mic[(s_mic_w + i) % MIC_RING] = (int16_t)(pcm[2 * i] | pcm[2 * i + 1] << 8);
    }
    s_mic_w += samples;
    portEXIT_CRITICAL(&s_mux);
}

bool muse_mirror_audio_take(uint8_t chunk[MUSE_MIRROR_AUDIO_CHUNK], uint8_t *source)
{
    uint8_t now = atomic_load(&s_source);
    if (!now || !s_out) return false;
    portENTER_CRITICAL(&s_mux);
    uint32_t w = s_out_w, r = s_out_r;
    if (w - r > OUT_RING) r = w - OUT_RING / 2;   /* too far behind: the newest half */
    bool have = atomic_load(&s_source) == now && w - r >= MUSE_MIRROR_AUDIO_CHUNK;
    if (have) {
        size_t at = r % OUT_RING;
        size_t first = OUT_RING - at < MUSE_MIRROR_AUDIO_CHUNK ? OUT_RING - at : MUSE_MIRROR_AUDIO_CHUNK;
        memcpy(chunk, s_out + at, first);
        memcpy(chunk + first, s_out, MUSE_MIRROR_AUDIO_CHUNK - first);
        r += MUSE_MIRROR_AUDIO_CHUNK;
    }
    s_out_r = r;
    portEXIT_CRITICAL(&s_mux);
    *source = now;
    return have;
}

void muse_mirror_adpcm(muse_mirror_adpcm_t *state, const uint8_t pcm[MUSE_MIRROR_AUDIO_CHUNK],
                       uint8_t out[MUSE_MIRROR_ADPCM_CHUNK])
{
    muse_adpcm_t s = { .pred = state->predictor, .index = (int8_t)state->index };
    out[0] = (uint8_t)s.pred;
    out[1] = (uint8_t)((uint16_t)s.pred >> 8);
    out[2] = (uint8_t)s.index;
    out[3] = 0;
    int16_t samples[MUSE_MIRROR_AUDIO_CHUNK / 2];
    memcpy(samples, pcm, sizeof(samples));
    muse_adpcm_encode_block(&s, samples, MUSE_MIRROR_AUDIO_CHUNK / 2, out + 4);
    state->predictor = s.pred;
    state->index = (uint8_t)s.index;
}

void muse_mirror_audio_mic_adpcm(const uint8_t chunk[MUSE_MIRROR_ADPCM_CHUNK])
{
    uint8_t *pcm = s_mic_pcm;
    if (!pcm) return;
    if (chunk[2] > 88) return;
    muse_adpcm_t s = { .pred = (int16_t)(chunk[0] | chunk[1] << 8), .index = (int8_t)chunk[2] };
    muse_adpcm_decode_block(&s, chunk + 4, MUSE_MIRROR_AUDIO_CHUNK / 2, (int16_t *)pcm);
    muse_mirror_audio_mic_put(pcm, MUSE_MIRROR_AUDIO_CHUNK / 2);
}

void muse_mirror_audio_reset(void)
{
    muse_mirror_audio_listen(0);
    muse_mirror_audio_speaker(false);
    muse_mirror_audio_mic(0);
}
