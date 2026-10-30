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

/* Host preview: renders Muse states to PPM so the art can be checked without hardware.
 *   cc -O2 -I components/muse tools/muse/preview.c avatar/muse_pixel.c -lm -o /tmp/muse_preview && /tmp/muse_preview /tmp
 * For your own avatar, build with components/muse/avatar/muse_pixel.c instead.
 */
#include <stdio.h>
#include <stdint.h>
#include "muse_pixel.h"

#define S 5
#define N (MUSE_PX_W * S)

static uint16_t buf[N * N];

static void save(const char *dir, const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/muse_%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t c = buf[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void shot(const char *dir, const char *name, muse_pose_t p)
{
    /* Run a second of frames so palette blending and eyes settle. */
    float end = p.t;
    for (float t = end - 1.5f; t <= end; t += 0.04f) {
        muse_pose_t q = p;
        q.t = t;
        q.mode_t = p.mode_t - (end - t);
        if (q.mode_t < 0) q.mode_t = 0;
        muse_pixel_render(&q);
    }
    muse_pixel_set_size(N);
    muse_pixel_scale(buf, N, 0, N - 1, 0, N - 1);
    save(dir, name);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    shot(dir, "boot", (muse_pose_t){ .mode = MUSE_MODE_BOOT, .t = 10.7f, .mode_t = 0.7f });
    shot(dir, "idle", (muse_pose_t){ .mode = MUSE_MODE_IDLE, .t = 20.3f, .mode_t = 5 });
    shot(dir, "listening", (muse_pose_t){ .mode = MUSE_MODE_LISTENING, .t = 30.1f, .mode_t = 2, .level = 0.7f });
    shot(dir, "thinking", (muse_pose_t){ .mode = MUSE_MODE_THINKING, .t = 40.2f, .mode_t = 1 });
    shot(dir, "working", (muse_pose_t){ .mode = MUSE_MODE_THINKING, .t = 45.1f, .mode_t = 1, .working = 1 });
    shot(dir, "speaking", (muse_pose_t){ .mode = MUSE_MODE_SPEAKING, .t = 50.25f, .mode_t = 1, .level = 0.6f });
    shot(dir, "happy", (muse_pose_t){ .mode = MUSE_MODE_IDLE, .t = 60.1f, .mode_t = 9, .happy = 1 });
    shot(dir, "off", (muse_pose_t){ .mode = MUSE_MODE_OFF, .t = 90.0f, .mode_t = 0.6f });
    shot(dir, "error", (muse_pose_t){ .mode = MUSE_MODE_ERROR, .t = 80.0f, .mode_t = 2 });
    return 0;
}
