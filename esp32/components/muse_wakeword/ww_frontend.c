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

#include "ww_frontend.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WW_N_BINS (WW_FRONTEND_N_FFT / 2 + 1)
#define WW_WINDOW_OFFSET ((WW_FRONTEND_N_FFT - WW_FRONTEND_WINDOW_SAMPLES) / 2)
#define WW_RETAINED_SAMPLES (WW_FRONTEND_N_FFT - WW_FRONTEND_HOP_SAMPLES)
#define WW_PI 3.14159265358979323846f

_Static_assert(WW_FRONTEND_SAMPLE_RATE_HZ == 16000, "frontend is 16 kHz only");
_Static_assert(WW_FRONTEND_WINDOW_SAMPLES == 480, "unexpected window size");
_Static_assert(WW_FRONTEND_HOP_SAMPLES == 320, "unexpected hop size");
_Static_assert(WW_FRONTEND_N_FFT == 512, "unexpected FFT size");
_Static_assert(WW_FRONTEND_N_MELS == 40, "unexpected mel count");

struct ww_frontend {
    float samples[WW_FRONTEND_N_FFT];
    size_t fill;

    float hann[WW_FRONTEND_WINDOW_SAMPLES];
    float mel[WW_FRONTEND_N_MELS][WW_N_BINS];
    float twiddle_re[WW_FRONTEND_N_FFT / 2];
    float twiddle_im[WW_FRONTEND_N_FFT / 2];
    uint16_t bit_reverse[WW_FRONTEND_N_FFT];

    float fft_re[WW_FRONTEND_N_FFT];
    float fft_im[WW_FRONTEND_N_FFT];
};

static float ww_hz_to_mel_slaney(float hz)
{
    const float f_sp = 200.0f / 3.0f;
    if (hz < 1000.0f) {
        return hz / f_sp;
    }
    return 15.0f + logf(hz / 1000.0f) / (logf(6.4f) / 27.0f);
}

static float ww_mel_to_hz_slaney(float mel)
{
    const float f_sp = 200.0f / 3.0f;
    if (mel < 15.0f) {
        return mel * f_sp;
    }
    return 1000.0f * expf((logf(6.4f) / 27.0f) * (mel - 15.0f));
}

static uint16_t ww_reverse_bits(uint16_t value)
{
    uint16_t out = 0;
    for (int i = 0; i < 9; i++) {
        out = (uint16_t)((out << 1) | (value & 1u));
        value >>= 1;
    }
    return out;
}

ww_frontend_t *ww_frontend_create(void)
{
    ww_frontend_t *frontend = calloc(1, sizeof(*frontend));
    if (frontend == NULL) {
        return NULL;
    }

    for (int i = 0; i < WW_FRONTEND_WINDOW_SAMPLES; i++) {
        frontend->hann[i] =
            0.5f - 0.5f * cosf(2.0f * WW_PI * (float)i / (float)WW_FRONTEND_WINDOW_SAMPLES);
    }

    for (int i = 0; i < WW_FRONTEND_N_FFT / 2; i++) {
        const float angle = -2.0f * WW_PI * (float)i / (float)WW_FRONTEND_N_FFT;
        frontend->twiddle_re[i] = cosf(angle);
        frontend->twiddle_im[i] = sinf(angle);
    }
    for (int i = 0; i < WW_FRONTEND_N_FFT; i++) {
        frontend->bit_reverse[i] = ww_reverse_bits((uint16_t)i);
    }

    float mel_points[WW_FRONTEND_N_MELS + 2];
    const float mel_min = ww_hz_to_mel_slaney(0.0f);
    const float mel_max = ww_hz_to_mel_slaney((float)WW_FRONTEND_SAMPLE_RATE_HZ / 2.0f);
    for (int i = 0; i < WW_FRONTEND_N_MELS + 2; i++) {
        const float mel =
            mel_min + (mel_max - mel_min) * (float)i / (float)(WW_FRONTEND_N_MELS + 1);
        mel_points[i] = ww_mel_to_hz_slaney(mel);
    }

    for (int m = 0; m < WW_FRONTEND_N_MELS; m++) {
        const float left = mel_points[m];
        const float center = mel_points[m + 1];
        const float right = mel_points[m + 2];
        const float norm = 2.0f / (right - left);
        for (int bin = 0; bin < WW_N_BINS; bin++) {
            const float freq =
                (float)bin * (float)WW_FRONTEND_SAMPLE_RATE_HZ / (float)WW_FRONTEND_N_FFT;
            const float lower = (freq - left) / (center - left);
            const float upper = (right - freq) / (right - center);
            float weight = lower < upper ? lower : upper;
            if (weight < 0.0f) {
                weight = 0.0f;
            }
            frontend->mel[m][bin] = weight * norm;
        }
    }

    return frontend;
}

void ww_frontend_destroy(ww_frontend_t *frontend)
{
    free(frontend);
}

void ww_frontend_reset(ww_frontend_t *frontend)
{
    if (frontend != NULL) {
        frontend->fill = 0;
    }
}

static void ww_frontend_fft(ww_frontend_t *frontend)
{
    /* Bit-reversed permutation, in place. */
    for (int i = 0; i < WW_FRONTEND_N_FFT; i++) {
        const int j = frontend->bit_reverse[i];
        if (j > i) {
            const float tr = frontend->fft_re[i];
            const float ti = frontend->fft_im[i];
            frontend->fft_re[i] = frontend->fft_re[j];
            frontend->fft_im[i] = frontend->fft_im[j];
            frontend->fft_re[j] = tr;
            frontend->fft_im[j] = ti;
        }
    }

    for (int len = 2; len <= WW_FRONTEND_N_FFT; len <<= 1) {
        const int half = len / 2;
        const int twiddle_step = WW_FRONTEND_N_FFT / len;
        for (int base = 0; base < WW_FRONTEND_N_FFT; base += len) {
            for (int k = 0; k < half; k++) {
                const int twiddle = k * twiddle_step;
                const float wr = frontend->twiddle_re[twiddle];
                const float wi = frontend->twiddle_im[twiddle];
                const int a = base + k;
                const int b = a + half;
                const float br = frontend->fft_re[b] * wr - frontend->fft_im[b] * wi;
                const float bi = frontend->fft_re[b] * wi + frontend->fft_im[b] * wr;
                frontend->fft_re[b] = frontend->fft_re[a] - br;
                frontend->fft_im[b] = frontend->fft_im[a] - bi;
                frontend->fft_re[a] += br;
                frontend->fft_im[a] += bi;
            }
        }
    }
}

static void ww_frontend_emit(ww_frontend_t *frontend, ww_frontend_frame_fn callback, void *ctx)
{
    for (int i = 0; i < WW_FRONTEND_N_FFT; i++) {
        frontend->fft_re[i] = 0.0f;
        frontend->fft_im[i] = 0.0f;
    }
    /* Match torch.stft semantics for win_length < n_fft: the 480-point
     * Hann window is centered in the 512-point FFT frame. */
    for (int i = 0; i < WW_FRONTEND_WINDOW_SAMPLES; i++) {
        frontend->fft_re[WW_WINDOW_OFFSET + i] =
            frontend->samples[WW_WINDOW_OFFSET + i] * frontend->hann[i];
    }

    ww_frontend_fft(frontend);

    float frame[WW_FRONTEND_N_MELS];
    for (int m = 0; m < WW_FRONTEND_N_MELS; m++) {
        float energy = 0.0f;
        for (int bin = 0; bin < WW_N_BINS; bin++) {
            const float weight = frontend->mel[m][bin];
            if (weight == 0.0f) {
                continue;
            }
            const float re = frontend->fft_re[bin];
            const float im = frontend->fft_im[bin];
            energy += weight * (re * re + im * im);
        }
        frame[m] =
            (logf(energy + WW_FRONTEND_LOG_EPS) - kWwFrontendNormMean[m]) / kWwFrontendNormStd[m];
    }

    if (callback != NULL) {
        callback(ctx, frame);
    }
}

size_t ww_frontend_push(ww_frontend_t *frontend, const int16_t *pcm, size_t frames,
                        ww_frontend_frame_fn callback, void *ctx)
{
    if (frontend == NULL || pcm == NULL) {
        return 0;
    }

    size_t emitted = 0;
    size_t offset = 0;
    while (offset < frames) {
        const size_t wanted = WW_FRONTEND_N_FFT - frontend->fill;
        const size_t available = frames - offset;
        const size_t take = wanted < available ? wanted : available;
        for (size_t i = 0; i < take; i++) {
            frontend->samples[frontend->fill + i] = (float)pcm[offset + i] * (1.0f / 32768.0f);
        }
        frontend->fill += take;
        offset += take;

        if (frontend->fill == WW_FRONTEND_N_FFT) {
            ww_frontend_emit(frontend, callback, ctx);
            emitted++;
            memmove(frontend->samples, frontend->samples + WW_FRONTEND_HOP_SAMPLES,
                    WW_RETAINED_SAMPLES * sizeof(frontend->samples[0]));
            frontend->fill = WW_RETAINED_SAMPLES;
        }
    }
    return emitted;
}
