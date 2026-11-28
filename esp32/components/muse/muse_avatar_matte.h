/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool applied;
    unsigned removed, softened;
} muse_avatar_matte_stats_t;

/* Conservative, opt-in uniform-white matting. Never a global white color key.
 * RGB565 is immutable while the A8 mask is computed. Work is bounded to 466x466.
 * Callers own an N-byte mask and N-entry uint32 queue, normally in PSRAM.
 * Rejected/blank backgrounds produce an all-opaque mask. No allocation or I/O. */
bool muse_avatar_white_candidate(const uint16_t *pixels,int width,int height);
bool muse_avatar_white_mask(const uint16_t *pixels,int width,int height,
                            uint8_t *alpha,uint32_t *queue,size_t queue_entries,
                            muse_avatar_matte_stats_t *stats);
/* Remove white edge contamination after the complete mask is known. RGB stays
 * unpremultiplied; the renderer composites it with A8 over its actual canvas. */
void muse_avatar_matte_decontaminate(uint16_t *pixels,const uint8_t *alpha,size_t count);
