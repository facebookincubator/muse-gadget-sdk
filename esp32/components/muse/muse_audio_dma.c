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

#include "muse_audio_dma.h"

muse_audio_dma_t muse_audio_dma_budget(unsigned desc, unsigned frames, unsigned bytes_per_frame, unsigned dirs)
{
    if (!desc) desc = MUSE_AUDIO_DMA_DEFAULT_DESC;
    if (!frames) frames = MUSE_AUDIO_DMA_DEFAULT_FRAMES;
    size_t buf = (size_t)frames * bytes_per_frame;
    if (buf > MUSE_AUDIO_DMA_BUF_MAX) buf = MUSE_AUDIO_DMA_BUF_MAX;
    buf = (buf + 3) & ~(size_t)3;   /* 4-byte aligned allocations */
    muse_audio_dma_t b = {
        .buf = buf,
        /* Each channel: desc buffers, each with its descriptor, plus the two
         * pointer arrays (4-byte pointers on the ESP32 family, one header each). */
        .need = (size_t)dirs * ((size_t)desc * (buf + MUSE_AUDIO_DMA_PER_BUF_OVERHEAD) + 2 * ((size_t)desc * 4 + 32)),
    };
    return b;
}

uint32_t muse_audio_dma_depth_us(unsigned desc, unsigned frames, unsigned rate)
{
    if (!desc) desc = MUSE_AUDIO_DMA_DEFAULT_DESC;
    if (!frames) frames = MUSE_AUDIO_DMA_DEFAULT_FRAMES;
    return rate ? (uint32_t)((uint64_t)desc * frames * 1000000u / rate) : 0;
}

bool muse_audio_dma_fits(const muse_audio_dma_t *b, size_t free, size_t largest)
{
    return b->buf > 0 && largest >= b->buf && free >= b->need;
}
