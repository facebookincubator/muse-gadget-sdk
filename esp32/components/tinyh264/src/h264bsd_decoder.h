/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*------------------------------------------------------------------------------

    Table of contents

    1. Include headers
    2. Module defines
    3. Data types
    4. Function prototypes

------------------------------------------------------------------------------*/

#ifndef H264SWDEC_DECODER_H
#define H264SWDEC_DECODER_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"

#include "h264bsd_storage.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/* enumerated return values of the functions */
enum {
    H264BSD_RDY,
    H264BSD_PIC_RDY,
    H264BSD_HDRS_RDY,
    H264BSD_ERROR,
    H264BSD_PARAM_SET_ERROR,
    H264BSD_MEMALLOC_ERROR
};

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/



/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdInit(storage_t *pStorage, u32 noOutputReordering);
u32 h264bsdDecode(storage_t *pStorage, u8 *byteStrm, u32 len, u8 **picture, u32 *width, u32 *height);
void h264bsdShutdown(storage_t *pStorage);
/* OPT: fn(ctx, picture, widthMbs, heightMbs, rows) whenever more of the
 * picture being decoded is final (decoded and deblocked), from inside
 * h264bsdDecode; rows counts luma rows from the top, reaching the full height
 * when the picture is done. The picture pointer is the one h264bsdDecode
 * returns for it (no output reordering). NULL: off. */
void h264bsdSetProgress(storage_t *pStorage,
    void (*fn)(void *ctx, const u8 *pic, u32 widthMbs, u32 heightMbs, u32 rowsFinal), void *ctx);

/* OPT: refuse sequence parameter sets for pictures wider or taller than
 * this (macroblocks), before anything is allocated for them: a stream can't
 * ask for more memory than its caller planned. Call after h264bsdInit. */
/* Zero keeps the default. Set before the first parameter set. */
void h264bsdSetMaxReferences(storage_t *pStorage, u32 maxRefs);
void h264bsdSetMaxSize(storage_t *pStorage, u32 maxWidthMbs, u32 maxHeightMbs);
/* OPT: and refuse them when their picture after frame cropping (the
 * sequence parameter set's frame_crop offsets) is wider or taller than this
 * (pixels; 0: no limit). The coded size may be larger than the cropped one
 * (an encoder's padding: iPhone streams code at least 192 x 96), within
 * h264bsdSetMaxSize. Call after h264bsdInit. */
void h264bsdSetMaxCropped(storage_t *pStorage, u32 maxWidth, u32 maxHeight);
/* OPT: the active sequence parameter set's frame_crop_left, right, top and
 * bottom offsets, as coded: in crop units (2 x 2 luma pixels for the 4:2:0
 * frames this decoder takes); 0 without frame_cropping_flag. HANTRO_OK, or
 * HANTRO_NOK with no active sequence parameter set (all 0). */
u32 h264bsdCroppingOffsets(const storage_t *pStorage, u32 *left, u32 *right, u32 *top, u32 *bottom);

/* OPT: error recovery (../README.md). h264bsdDecode leaves a picture it
 * couldn't finish (an error, or slices missing) started; the next call would
 * conceal it at its first NAL unit and return that picture in place of the
 * one it was given, which then goes undecoded. A caller that hands over whole
 * access units abandons it instead: after a call that didn't return a
 * picture, h264bsdPictureStarted says one is left, and h264bsdAbandonPicture
 * drops it (nothing of it is shown or kept as a reference), so the next
 * access unit decodes on its own. */
u32 h264bsdPictureStarted(const storage_t *pStorage);
void h264bsdAbandonPicture(storage_t *pStorage);
/* OPT: after an IDR picture decoded, keep the active parameter sets and lock
 * the stream's size and profile (sequence parameter sets for another are
 * refused from then on); after an error, put the kept ones back, in case
 * damaged bytes replaced them. HANTRO_OK, or MEMORY_ALLOCATION_ERROR (nothing
 * changed). */
u32 h264bsdKeepParamSets(storage_t *pStorage);
u32 h264bsdRestoreParamSets(storage_t *pStorage);
/* OPT: once parameter sets are kept, one that differs from the kept one with
 * its id is taken only while allowed (another size or profile never is): set
 * it for a keyframe's access unit (where the stream's own come), clear it for
 * the rest. */
void h264bsdAllowParamSetChange(storage_t *pStorage, u32 allow);

storage_t* h264bsdAlloc();
void h264bsdFree(storage_t *pStorage);

#endif /* #ifdef H264SWDEC_DECODER_H */

