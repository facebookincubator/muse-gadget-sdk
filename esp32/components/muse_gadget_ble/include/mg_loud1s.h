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
 * A capture's loudest 1 s window (the level check of spoken-prompt
 * tests: "loud1s -38.2 dBFS" beside rms / peak / floor in the utterance
 * summary). The capture path already sums each sample's square; it hands
 * this the mean square of each 10 ms frame (mg_loud1s_push), and a running
 * sum over the last 100 frames gives each 1 s window's level in O(1) per
 * frame: nothing per sample. The only memory is the ring of the last 100
 * frames' energies (MG_LOUD1S_RING_BYTES), which the owner allocates (PSRAM
 * where it can) and hands over once; without one nothing is measured.
 *
 * A capture shorter than 1 s reads as its whole level. Once it reaches a
 * full second only whole 1 s windows count. Portable: no IDF, no allocation.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MG_LOUD1S_FRAMES 100   /* 10 ms frames: one second */
#define MG_LOUD1S_RING_BYTES (MG_LOUD1S_FRAMES * sizeof(uint32_t))

typedef struct {
    uint32_t *e;        /* the last frames' mean squares, MG_LOUD1S_FRAMES of them; NULL: not measured */
    uint64_t sum;       /* the frames in the ring, summed */
    uint64_t best;      /* the loudest window's sum, scaled to MG_LOUD1S_FRAMES frames */
    uint16_t at, n;     /* the next slot; frames held (up to MG_LOUD1S_FRAMES) */
} mg_loud1s_t;

/* A new capture; ring (MG_LOUD1S_RING_BYTES, or NULL) is kept across them. */
static inline void mg_loud1s_reset(mg_loud1s_t *l, uint32_t *ring)
{
    l->e = ring;
    l->sum = l->best = 0;
    l->at = l->n = 0;
}

/* One 10 ms frame's mean square (sum of squares / samples; at most 2^30). */
static inline void mg_loud1s_push(mg_loud1s_t *l, uint32_t mean_sq)
{
    if (!l->e) {
        return;
    }
    bool full = l->n == MG_LOUD1S_FRAMES;
    if (full) {
        l->sum -= l->e[l->at];
    } else {
        l->n++;
    }
    l->e[l->at] = mean_sq;
    l->sum += mean_sq;
    l->at = (uint16_t)((l->at + 1) % MG_LOUD1S_FRAMES);
    if (!full) {
        /* Under a second, the whole so far; the first full second replaces it. */
        l->best = l->sum * MG_LOUD1S_FRAMES / l->n;
    } else if (l->sum > l->best) {
        l->best = l->sum;
    }
}

/* The loudest window in dBFS (-100 at the least) into *db; false with no frame or no ring. */
static inline bool mg_loud1s_db(const mg_loud1s_t *l, float *db)
{
    if (!l->e || !l->n) {
        return false;
    }
    double ms = (double)l->best / MG_LOUD1S_FRAMES / (32768.0 * 32768.0);
    float v = (float)(10.0 * log10(ms + 1e-10));
    *db = v < -100.0f ? -100.0f : v;
    return true;
}
