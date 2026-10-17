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

storage_t* h264bsdAlloc();
void h264bsdFree(storage_t *pStorage);

#endif /* #ifdef H264SWDEC_DECODER_H */

