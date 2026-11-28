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

#ifndef H264SWDEC_IMAGE_H
#define H264SWDEC_IMAGE_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_cfg.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

typedef struct
{
    u8 *data;
    u32 width;
    u32 height;
    /* current MB's components */
    u8 *luma;
    u8 *cb;
    u8 *cr;
#if H264BSD_SPLIT_PLANES
    /* OPT: the chroma planes need not follow the luma plane (low-memory
     * mode, h264bsdSetLowMem). cbData: this picture's Cb plane; Cr is
     * crDelta bytes from it. yBase / cbBase: in low-memory mode the luma
     * and chroma regions every picture lives in, which maps a reference
     * picture's luma pointer to its Cb plane (h264bsdCbOf); NULL otherwise
     * (Cb follows luma). */
    u8 *cbData;
    long crDelta;           /* bytes from Cb to Cr (may be negative) */
    u8 *yBase;
    u8 *cbBase;
    /* OPT: in-place decoding (low-memory mode): reference luma rows above
     * ipWater have been overwritten by this picture; a prediction that would
     * read one sets *ipBad (the picture is then reported as an error). */
    i32 ipWater;
    u32 *ipBad;
#endif
#if H264BSD_LOWMEM
    /* OPT: the macroblock entries are a ring of this many rows (0: one per
     * macroblock; h264bsd_lowmem.c) */
    u32 mbRingRows;
#endif
} image_t;

#if H264BSD_SPLIT_PLANES
/* OPT: the Cb plane of the picture whose luma starts at luma (the current
 * picture or a reference), laid out as im says */
static inline u8 *h264bsdCbOf(const image_t *im, const u8 *luma)
{
    if (im->yBase)
        return im->cbBase + ((u32)(luma - im->yBase) >> 2);
    return (u8 *)luma + 256 * im->width * im->height;
}

/* OPT: a reference picture's image laid out like the current one (cur);
 * its data is set per partition */
static inline void h264bsdRefImageInit(image_t *ref, const image_t *cur)
{
    ref->width = cur->width;
    ref->height = cur->height;
    ref->crDelta = cur->crDelta;
    ref->yBase = cur->yBase;
    ref->cbBase = cur->cbBase;
    ref->ipWater = cur->ipWater;
    ref->ipBad = cur->ipBad;
}

/* OPT: point image at a picture whose luma starts at data */
static inline void h264bsdImageSetData(image_t *im, u8 *data)
{
    im->data = data;
    im->cbData = h264bsdCbOf(im, data);
    if (!im->yBase)
    {
        im->crDelta = 64 * im->width * im->height;
    }
}
#endif

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

void h264bsdWriteMacroblock(image_t *image, u8 *data);

#ifndef H264DEC_OMXDL
void h264bsdWriteOutputBlocks(image_t *image, u32 mbNum, u8 *data,
    i32 residual[][16]);
#endif

#endif /* #ifdef H264SWDEC_IMAGE_H */

