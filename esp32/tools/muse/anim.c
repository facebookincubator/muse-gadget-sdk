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

/* Host animation dump: plays each Muse animation through the real renderer at the
 * device frame rate and writes the frames as PPMs (<dir>/<name>/NNN.ppm).
 * tools/muse/make_gifs.py builds this, against your own avatar's renderer if
 * you have one (components/muse/avatar/muse_pixel.c), and turns the frames into GIFs.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include "muse_pixel.h"

#ifndef MUSE_ANIM_SCALE
#define MUSE_ANIM_SCALE 5
#endif
#define S MUSE_ANIM_SCALE
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */
#define WARMUP 1.5f     /* let palette blending and eyes settle before recording */

static uint16_t buf[N * N];

typedef struct {
    const char *name;
    muse_mode_t mode;
    float secs;
    float start_mode_t;     /* mode_t at the first recorded frame */
    bool talky;             /* drive `level` like live speech */
    bool pet;               /* play a happy reaction partway through */
} anim_t;

static const anim_t ANIMS[] = {
    { "boot", MUSE_MODE_BOOT, 3.0f, 0 },
    { "idle", MUSE_MODE_IDLE, 6.0f, 5 },
    { "listening", MUSE_MODE_LISTENING, 4.0f, 0, .talky = true },
    { "thinking", MUSE_MODE_THINKING, 4.0f, 0 },
    { "speaking", MUSE_MODE_SPEAKING, 4.0f, 0, .talky = true },
    { "happy", MUSE_MODE_IDLE, 3.0f, 9, .pet = true },
    { "off", MUSE_MODE_OFF, 2.0f, 0 },
    { "error", MUSE_MODE_ERROR, 3.0f, 0 },
};

/* Bursty syllable-like envelope in 0..1. */
static float speech_level(float t)
{
    float syl = fabsf(sinf(t * 6.3f)) * (0.55f + 0.45f * sinf(t * 1.7f + 1.0f));
    float gap = sinf(t * 0.9f) > -0.6f ? 1.0f : 0.15f;
    float v = syl * gap;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* Matches muse_state_happiness(): full for HAPPY_SECS - 0.4, then eases out. */
static float happiness(float since_pet)
{
    float left = 1.6f - since_pet;
    if (since_pet < 0 || left <= 0) {
        return 0;
    }
    return left > 0.4f ? 1.0f : left / 0.4f;
}

static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t c = buf[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    float base = 10.0f;
    muse_pixel_set_size(N);

    for (size_t a = 0; a < sizeof(ANIMS) / sizeof(ANIMS[0]); a++) {
        const anim_t *an = &ANIMS[a];
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, an->name);
        mkdir(path, 0755);

        int frames = (int)(an->secs / DT + 0.5f);
        int warm = (int)(WARMUP / DT + 0.5f);
        for (int i = -warm; i < frames; i++) {
            float rt = i * DT;      /* seconds into the recording */
            muse_pose_t p = {
                .mode = an->mode,
                .t = base + rt,
                .mode_t = an->start_mode_t + rt,
                .level = an->talky ? speech_level(rt) : 0,
                .happy = an->pet ? happiness(rt - 0.5f) : 0,
            };
            /* Modes that start at 0 hold their first frame during warm-up. */
            if (p.mode_t < 0) {
                p.mode_t = 0;
            }
            muse_pixel_render(&p);
            if (i >= 0) {
                muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
                snprintf(path, sizeof(path), "%s/%s/%03d.ppm", dir, an->name, i);
                save(path);
            }
        }
        base += 100.0f;
    }
    return 0;
}
