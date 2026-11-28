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

#ifndef H264SWDEC_CAVLC_H
#define H264SWDEC_CAVLC_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_stream.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/


/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdDecodeResidualBlockCavlc(
  strmData_t *pStrmData,
  i32 *coeffLevel,
  i32 nc,
  u32 maxNumCoeff);

/* OPT: the 32-bit stream cache kept across a macroblock's residual blocks, so a block
 * neither re-reads the stream position at its start nor writes it back at its end (DecodeResidual:
 * h264bsdCavlcBitsInit once, h264bsdDecodeResidualBlockCavlcC per block, h264bsdCavlcBitsDone at
 * the end). The same bits are read in the same order: bit-exact. */
typedef struct
{
    u32 value; /* the next bits, MSB first */
    u32 bits;  /* how many of them are valid */
} cavlcBits_t;

static inline void h264bsdCavlcBitsInit(strmData_t *pStrmData, cavlcBits_t *c)
{
    c->value = h264bsdShowBits32(pStrmData);
    c->bits = 32;
}

static inline u32 h264bsdCavlcBitsDone(strmData_t *pStrmData, cavlcBits_t *c)
{
    return h264bsdFlushBits(pStrmData, 32 - c->bits);
}

u32 h264bsdDecodeResidualBlockCavlcC(
  strmData_t *pStrmData,
  cavlcBits_t *c,
  i32 *coeffLevel,
  i32 nc,
  u32 maxNumCoeff);

#endif /* #ifdef H264SWDEC_CAVLC_H */

