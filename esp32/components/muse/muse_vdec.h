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
 * The video decoders behind muse_video.c, one interface: H.264 (tinyh264)
 * and, with CONFIG_MUSE_VIDEO_VP8, VP8 (libvpx_vp8). A decoder
 * reports the rows of its picture that are final as it decodes them, so the
 * panel can start before the frame is done.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A decoded 4:2:0 picture: three planes with their own strides. w and h are
 * the decoder's (macroblock-aligned) size. */
typedef struct {
    const uint8_t *y, *u, *v;
    int y_stride, c_stride;
    int w, h;
} muse_pic_t;

typedef struct muse_vdec muse_vdec_t;

/* pic: the picture so far; rows: its luma rows that are final. */
typedef void (*muse_vdec_progress_t)(void *ctx, const muse_pic_t *pic, int rows);

/* codec: MUSE_VIDEO_CODEC_H264 or MUSE_VIDEO_CODEC_VP8 (muse_vstream.h). NULL: no memory
 * or a codec this build doesn't have. */
muse_vdec_t *muse_vdec_open(uint8_t codec);
void muse_vdec_close(muse_vdec_t *d);
bool muse_vdec_has(uint8_t codec);
void muse_vdec_set_progress(muse_vdec_t *d, muse_vdec_progress_t cb, void *ctx);

typedef enum {
    MUSE_VDEC_PICTURE = 1,     /* *pic holds a picture to show */
    MUSE_VDEC_NO_PICTURE = 0,  /* decoded, nothing to show (parameter sets, a hidden frame) */
    MUSE_VDEC_ERROR = -1,      /* not decodable: wait for a keyframe */
} muse_vdec_result_t;

/* One frame: an H.264 access unit or a VP8 frame. The picture stays valid
 * until the decode after next (the decoder predicts the next frame from it). */
muse_vdec_result_t muse_vdec_decode(muse_vdec_t *d, const uint8_t *buf, uint32_t len, muse_pic_t *pic);

#ifdef __cplusplus
}
#endif
