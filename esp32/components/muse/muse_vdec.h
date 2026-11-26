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
    uint32_t seq;   /* this picture's number, unique across decoders (0: none) */
    /* H.264 pictures only, else NULL: per macroblock (w / 16 a row), 1 where it
     * is picture same_as (the same decoder's previous one) exactly
     * (muse_vdec_same.h). */
    const uint8_t *same;
    uint32_t same_as;
    int same_cols;   /* same's macroblocks a row (the coded width / 16) */
} muse_pic_t;

/* Does picture p's memory cover a w x h window from its top left (its planes, strides and same-map)? A
 * reader that addresses one picture by another's size checks this first (two bands). */
static inline bool muse_pic_covers(const muse_pic_t *p, int w, int h)
{
    if (!p || w < 0 || h < 0 || w > p->w || h > p->h) {
        return false;
    }
    return p->y && p->u && p->v && p->y_stride >= p->w && p->c_stride >= (p->w + 1) / 2 &&
           (!p->same || p->same_cols >= (p->w + 15) / 16);
}

/*
 * H.264 frame cropping (7.4.2.1.1): the window of the coded picture that is
 * the picture, from a sequence parameter set's frame_crop_left, right, top
 * and bottom offsets, which count crop units: 1 x (2 - frame_mbs_only) luma
 * pixels for monochrome (chroma_format_idc 0), else SubWidthC x SubHeightC *
 * (2 - frame_mbs_only) (4:2:0: 2 x 2, 4:2:2: 2 x 1, 4:4:4: 1 x 1, for
 * frames). coded_h is the frame's height in luma rows. false, with the whole
 * coded picture as the window, when the offsets leave no picture (or the
 * format is unknown). An H.264 picture muse_vdec_decode returns is already
 * this window (y, u and v point at its top left; w and h are its size).
 */
typedef struct {
    int x, y, w, h;
} muse_vcrop_t;
bool muse_h264_crop_window(int coded_w, int coded_h, unsigned chroma_format_idc, bool frame_mbs_only,
                           uint32_t left, uint32_t right, uint32_t top, uint32_t bottom, muse_vcrop_t *win);

typedef struct muse_vdec muse_vdec_t;

/* pic: the picture so far; rows: its luma rows that are final. */
typedef void (*muse_vdec_progress_t)(void *ctx, const muse_pic_t *pic, int rows);

/* codec: MUSE_VIDEO_CODEC_H264 or _VP8 (muse_vstream.h). NULL: no memory
 * or a codec this build doesn't have. */
/* A decoder for codec whose pictures are at most max_w x max_h (the
 * stream's negotiated size): a stream that describes a larger one is an
 * error, before anything is allocated for it. For H.264 that's the picture
 * after frame cropping (start_streaming's width and height are, mgcommands.h),
 * up to max rounded up to whole macroblocks (an uncropped stream padded to
 * them, as before); the coded picture may be larger by up to 32 px of
 * cropping a side, and at least 192 x 96 (the iPhone encoder's floor, which
 * it crops): a cropped sequence parameter set is never refused for its
 * padding. */
muse_vdec_t *muse_vdec_open(uint8_t codec, int max_w, int max_h);
/* True if the access unit may make the decoder reallocate its pictures (an
 * H.264 parameter set or IDR slice, with or without a start code before it;
 * a VP8 keyframe): pictures it handed out before may be freed by
 * muse_vdec_decode, so wait for their readers first. */
bool muse_vdec_may_realloc(const muse_vdec_t *d, const uint8_t *buf, uint32_t len);
/* Optional H.264 reference-frame limit; no effect on VP8. Set before decode. */
void muse_vdec_limit_refs(muse_vdec_t *d, unsigned max_refs);
void muse_vdec_close(muse_vdec_t *d);
bool muse_vdec_has(uint8_t codec);
/* Whether a w x h stream of codec has room to decode now (true on every board with PSRAM): a stream start
 * that doesn't fit is refused busy. */
bool muse_vdec_fits(uint8_t codec, int w, int h);
/* The stream asked for more memory than there is (its SPS, mid-stream): the caller ends it. */
bool muse_vdec_no_memory(const muse_vdec_t *d);
/* Takes the decoder's memory now for a cw x ch coded picture (the H.264 low-memory copy opens at the
 * stream's first SPS otherwise; a different SPS reopens it). False if there's no room. */
bool muse_vdec_prepare(muse_vdec_t *d, int cw, int ch);
/* The coded size the decoder's memory was taken for (the H.264 low-memory copy) and its reach: the rows a
 * motion vector may point above the row being written (0: the whole picture is kept); false if none yet. */
bool muse_vdec_coded(const muse_vdec_t *d, int *cw, int *ch, int *reach);
void muse_vdec_set_progress(muse_vdec_t *d, muse_vdec_progress_t cb, void *ctx);

/* Each access unit decodes on its own (H.264: a picture an earlier one left
 * unfinished is dropped first, never returned for this one). After an error
 * the parameter sets of the last keyframe that decoded come back; once one
 * has, parameter sets for another size or profile are refused, and changed
 * ones only come beside a keyframe (../../../protocols/VIDEO.md, Trust). */
typedef enum {
    MUSE_VDEC_PICTURE = 1,     /* *pic holds this access unit's picture, to show */
    MUSE_VDEC_NO_PICTURE = 0,  /* decoded, nothing to show (parameter sets alone, a hidden frame) */
    MUSE_VDEC_ERROR = -1,      /* not decodable, or its picture incomplete: wait for a keyframe */
} muse_vdec_result_t;

/* One frame: an H.264 access unit or a VP8 frame. The picture stays valid
 * until the decode after next (the decoder predicts the next frame from it). */
muse_vdec_result_t muse_vdec_decode(muse_vdec_t *d, const uint8_t *buf, uint32_t len, muse_pic_t *pic);

#ifdef __cplusplus
}
#endif
