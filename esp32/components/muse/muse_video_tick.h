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

/*
 * The video's once-a-second counters (for A/V bench runs): while a video stream runs, the decode task logs one line a second
 * on muse_video (on the Nordic UART mirror):
 *
 *   av video: t=12034 span=1001 rx=30 shown=30 drop=0 skip=0 decerr=0 fps=30 int=23456 low=21000
 *
 *   t       ms since the stream's decoder opened (the end of this window)
 *   span    ms this window covers (about 1000)
 *   rx      frames received (taken from the ring) in the window
 *   shown   pictures that reached the panel
 *   drop    frames dropped (waiting for a keyframe, lost, not decoded)
 *   skip    decoded but not drawn (a newer picture was ready)
 *   decerr  frames the decoder refused (part of drop)
 *   fps     the stream's target
 *   int     internal RAM free now; low: the lowest it has been since boot
 *
 * No allocation and no IDF here, so the host tests run it.
 */

typedef struct {
    uint32_t rx;
    uint16_t shown, dropped;   /* the video core's 16-bit counters: windows take them mod 2^16 */
    uint32_t skipped;
    uint32_t decode_errors;
} muse_vcounts_t;

typedef struct {
    int64_t start_us, last_us;
    muse_vcounts_t last;
    bool on;
} muse_vtick_t;

/* A stream's decoder opened: windows count from here. */
void muse_vtick_start(muse_vtick_t *t, int64_t now_us, const muse_vcounts_t *c);
/* The stream ended: no more lines until the next start. */
void muse_vtick_stop(muse_vtick_t *t);
/* A second or more since the last line (or the start). */
bool muse_vtick_due(const muse_vtick_t *t, int64_t now_us);
/* Formats the window that ends now into out and starts the next; returns the
 * line's length (snprintf's), 0 if not running. */
int muse_vtick_line(muse_vtick_t *t, int64_t now_us, const muse_vcounts_t *c, unsigned fps,
                         uint32_t internal_free, uint32_t internal_low, char *out, size_t cap);
