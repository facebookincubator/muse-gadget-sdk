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

#ifndef H264SWDEC_VLC_H
#define H264SWDEC_VLC_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_stream.h"
#include "h264bsd_transform.h"
#include "h264bsd_util.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/


/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdDecodeExpGolombUnsigned(strmData_t *pStrmData, u32 *value);

u32 h264bsdDecodeExpGolombSigned(strmData_t *pStrmData, i32 *value);

/* OPT: the codes of 1 .. 31 bits inline (one window, the leading zeros counted at once:
 * the same values and end-of-stream errors as the functions); longer ones through them */
#if H264BSD_CLZ
static inline u32 h264bsdExpGolombU(strmData_t *pStrmData, u32 *value)
{
    u32 bits = h264bsdShowBits32(pStrmData);

    if (bits >= 0x00010000)
    {
        u32 len = 2 * (u32)__builtin_clz(bits) + 1;
        if (h264bsdFlushBits(pStrmData, len) == END_OF_STREAM)
            return(HANTRO_NOK);
        *value = (bits >> (32 - len)) - 1;
        return(HANTRO_OK);
    }
    return h264bsdDecodeExpGolombUnsigned(pStrmData, value);
}

static inline u32 h264bsdExpGolombS(strmData_t *pStrmData, i32 *value)
{
    u32 bits = h264bsdShowBits32(pStrmData);

    if (bits >= 0x00010000)
    {
        u32 len = 2 * (u32)__builtin_clz(bits) + 1, codeNum;
        if (h264bsdFlushBits(pStrmData, len) == END_OF_STREAM)
            return(HANTRO_NOK);
        codeNum = (bits >> (32 - len)) - 1;
        *value = (codeNum & 0x1) ? (i32)((codeNum + 1) >> 1) : -(i32)((codeNum + 1) >> 1);
        return(HANTRO_OK);
    }
    return h264bsdDecodeExpGolombSigned(pStrmData, value);
}
#else
#define h264bsdExpGolombU h264bsdDecodeExpGolombUnsigned
#define h264bsdExpGolombS h264bsdDecodeExpGolombSigned
#endif

u32 h264bsdDecodeExpGolombMapped(strmData_t *pStrmData, u32 *value,
    u32 isIntra);

u32 h264bsdDecodeExpGolombTruncated(strmData_t *pStrmData, u32 *value,
    u32 greaterThanOne);

#endif /* #ifdef H264SWDEC_VLC_H */

