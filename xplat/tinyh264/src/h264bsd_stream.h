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

#ifndef H264SWDEC_STREAM_H
#define H264SWDEC_STREAM_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include <string.h>

#include "basetype.h"

/* OPT: H264BSD_WORD_LOADS reads the stream a 32-bit word at a time (byte-swapped)
 * where unaligned loads are cheap and the core is little-endian; elsewhere four
 * byte loads, as upstream. Either way the same bits. A port can set it to 0, or
 * to 1 on a little-endian core (the swap assumes one). */
#ifndef H264BSD_WORD_LOADS
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && \
    (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || \
     defined(__ARM_FEATURE_UNALIGNED) || defined(__riscv_misaligned_fast))
#define H264BSD_WORD_LOADS 1
#else
#define H264BSD_WORD_LOADS 0
#endif
#endif
#if H264BSD_WORD_LOADS && defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "H264BSD_WORD_LOADS needs a little-endian target"
#endif

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

typedef struct
{
    u8  *pStrmBuffStart;    /* pointer to start of stream buffer */
    u8  *pStrmCurrPos;      /* current read address in stream buffer */
    u32  bitPosInWord;      /* bit position in stream buffer byte */
    u32  strmBuffSize;      /* size of stream buffer (bytes) */
    u32  strmBuffReadBits;  /* number of bits read from stream buffer */
} strmData_t;

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

/* OPT: hot bit-reader primitives inlined into every caller (identical semantics
 * to the original out-of-line versions). */
static inline u32 h264bsdShowBits32(strmData_t *pStrmData)
{
    u8 *pStrm = pStrmData->pStrmCurrPos;
    i32 bits = (i32)pStrmData->strmBuffSize*8 - (i32)pStrmData->strmBuffReadBits;
    if (bits >= 32)
    {
        u32 bitPosInWord = pStrmData->bitPosInWord;
#if H264BSD_WORD_LOADS
        u32 out;
        /* OPT: one (unaligned) word, byte-swapped: the big-endian 32 bits */
        memcpy(&out, pStrm, 4);
        out = __builtin_bswap32(out);
#else
        u32 out = ((u32)pStrm[0] << 24) | ((u32)pStrm[1] << 16) |
                  ((u32)pStrm[2] <<  8) | ((u32)pStrm[3]);
#endif
        if (bitPosInWord)
        {
            out <<= bitPosInWord;
            out |= (u32)pStrm[4] >> (8 - bitPosInWord);
        }
        return out;
    }
    else if (bits > 0)
    {
        i32 shift = (i32)(24 + pStrmData->bitPosInWord);
        u32 out = (u32)(*pStrm++) << shift;
        bits -= (i32)(8 - pStrmData->bitPosInWord);
        while (bits > 0)
        {
            shift -= 8;
            out |= (u32)(*pStrm++) << shift;
            bits -= 8;
        }
        return out;
    }
    return 0;
}

static inline u32 h264bsdFlushBits(strmData_t *pStrmData, u32 numBits)
{
    u32 rb = pStrmData->strmBuffReadBits + numBits;
    pStrmData->strmBuffReadBits = rb;
    pStrmData->bitPosInWord = rb & 0x7;
    if (rb <= 8*pStrmData->strmBuffSize)
    {
        pStrmData->pStrmCurrPos = pStrmData->pStrmBuffStart + (rb >> 3);
        return 0; /* HANTRO_OK */
    }
    return 0xFFFFFFFFU; /* END_OF_STREAM */
}

static inline u32 h264bsdGetBits(strmData_t *pStrmData, u32 numBits)
{
    u32 out = h264bsdShowBits32(pStrmData) >> (32 - numBits);
    if (h264bsdFlushBits(pStrmData, numBits) == 0)
        return out;
    return 0xFFFFFFFFU; /* END_OF_STREAM */
}

u32 h264bsdIsByteAligned(strmData_t *);

#endif /* #ifdef H264SWDEC_STREAM_H */

