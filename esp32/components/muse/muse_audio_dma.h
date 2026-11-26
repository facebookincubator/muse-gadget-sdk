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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The internal DMA memory opening the codecs may take (muse_audio.c's guard,
 * before esp_codec_dev_open): opening reconfigures each I2S channel to the
 * format it opens with, and a new buffer size makes ESP-IDF free the
 * channel's DMA buffers and allocate new ones (i2s_std_set_slot). If that
 * allocation fails, esp_codec_dev ignores the error and enables the channel
 * anyway, which faults (LoadProhibited in i2s_rx_channel_start on the
 * Waveshare S3 1.75C). So the codecs open only when the new buffers fit.
 *
 * desc and frames: the channels' dma_desc_num and dma_frame_num (0: ESP-IDF's
 * defaults, 6 and 240); bytes_per_frame: every slot of one frame (16-bit
 * stereo: 4); dirs: the channels opened (speaker and mic: 2). Each buffer is
 * its own allocation (ESP-IDF caps one at 4092 bytes), with its descriptor
 * and the heap's overhead.
 */
typedef struct {
    size_t buf;     /* one DMA buffer, the largest single allocation */
    size_t need;    /* everything the channels allocate */
} muse_audio_dma_t;

#define MUSE_AUDIO_DMA_DEFAULT_DESC 6
#define MUSE_AUDIO_DMA_DEFAULT_FRAMES 240
#define MUSE_AUDIO_DMA_BUF_MAX 4092
/* Per buffer: its descriptor (an lldesc_t) and two heap block headers. */
#define MUSE_AUDIO_DMA_PER_BUF_OVERHEAD 64

muse_audio_dma_t muse_audio_dma_budget(unsigned desc, unsigned frames, unsigned bytes_per_frame, unsigned dirs);

/*
 * Live reply audio (total playout depth at most
 * 100 ms): the musegadgets boards' duplex I2S takes 3 buffers of 160 frames,
 * 30 ms each way at 16 kHz, instead of ESP-IDF's 6 x 240 (90 ms). The player
 * writes 10 ms chunks and the mic reads 20 ms, so each side keeps a buffer
 * of slack. A board sets both its channel config and muse_board_t's
 * i2s_dma_desc / i2s_dma_frames to these.
 */
#define MUSE_AUDIO_DMA_LIVE_DESC 3
#define MUSE_AUDIO_DMA_LIVE_FRAMES 160

/* The audio the speaker's DMA holds, in microseconds: desc x frames at rate
 * (0: ESP-IDF's defaults, as for the budget). */
uint32_t muse_audio_dma_depth_us(unsigned desc, unsigned frames, unsigned rate);

/* Whether the budget fits in internal DMA memory with free bytes free and
 * largest the largest free block. Pessimistic: the channels' old buffers,
 * which ESP-IDF frees first, aren't counted. */
bool muse_audio_dma_fits(const muse_audio_dma_t *b, size_t free, size_t largest);

#ifdef __cplusplus
}
#endif
