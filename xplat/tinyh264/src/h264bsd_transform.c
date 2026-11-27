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
     2. External compiler flags
     3. Module defines
     4. Local function prototypes
     5. Functions
          h264bsdProcessBlock
          h264bsdProcessLumaDc
          h264bsdProcessChromaDc

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_transform.h"
#include "h264bsd_util.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/* Switch off the following Lint messages for this file:
 * Info 701: Shift left of signed quantity (int)
 * Info 702: Shift right of signed quantity (int)
 */
/*lint -e701 -e702 */

/* LevelScale function */
static const i32 levelScale[6][3] = {
    {10,13,16}, {11,14,18}, {13,16,20}, {14,18,23}, {16,20,25}, {18,23,29}};

/* qp % 6 as a function of qp */
static const u8 qpMod6[52] = {0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,
    0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3};

/* qp / 6 as a function of qp */
static const u8 qpDiv6[52] = {0,0,0,0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,3,
    4,4,4,4,4,4,5,5,5,5,5,5,6,6,6,6,6,6,7,7,7,7,7,7,8,8,8,8};

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------

    Function: h264bsdProcessBlock

        Functional description:
            Function performs inverse zig-zag scan, inverse scaling and
            inverse transform for a luma or a chroma residual block

        Inputs:
            data            pointer to data to be processed
            qp              quantization parameter
            skip            skip processing of data[0], set to non-zero value
                            if dc coeff hanled separately
            coeffMap        16 lsb's indicate which coeffs are non-zero,
                            bit 0 (lsb) for coeff 0, bit 1 for coeff 1 etc.

        Outputs:
            data            processed data

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      processed data not in valid range [-512, 511]

------------------------------------------------------------------------------*/
#if defined(H264BSD_MVE)
#include <arm_mve.h>
/* OPT (Helium): the general case (coefficients past the first row) with the
 * same arithmetic as the C below: the inverse zig-zag as gathers of the
 * block's columns (lanes: rows), each scaled by its position's level scale;
 * the horizontal transform lane by lane on the columns; a transpose through
 * an interleaving store (vst4); the vertical transform lane by lane on the
 * rows, (x + 32) >> 6, the [-512, 511] check, raster order back into data. */
static u32 ProcessBlockMve(i32 *data, i32 s1, i32 s2, i32 s3, u32 skip)
{
    /* the scan index of each raster position, column by column (rows 0..3) */
    static const uint32_t colScan[4][4] = {{0, 2, 3, 9}, {1, 4, 8, 10}, {5, 7, 11, 14}, {6, 12, 13, 15}};
    int32_t *d32 = (int32_t *)data;   /* i32 is int, int32_t long on some targets */
    int32x4_t even = vdupq_n_s32(s2), odd = vdupq_n_s32(s3);
    even = vsetq_lane_s32(s1, even, 0);
    even = vsetq_lane_s32(s1, even, 2);     /* rows 0, 2: s1; rows 1, 3: s2 */
    odd = vsetq_lane_s32(s2, odd, 0);
    odd = vsetq_lane_s32(s2, odd, 2);       /* rows 0, 2: s2; rows 1, 3: s3 */
    int32x4_t first = skip ? vsetq_lane_s32(1, even, 0) : even;
    int32x4_t c0 = vmulq_s32(vldrwq_gather_shifted_offset_s32(d32, vld1q_u32(colScan[0])), first);
    int32x4_t c1 = vmulq_s32(vldrwq_gather_shifted_offset_s32(d32, vld1q_u32(colScan[1])), odd);
    int32x4_t c2 = vmulq_s32(vldrwq_gather_shifted_offset_s32(d32, vld1q_u32(colScan[2])), even);
    int32x4_t c3 = vmulq_s32(vldrwq_gather_shifted_offset_s32(d32, vld1q_u32(colScan[3])), odd);
    /* horizontal transform (each lane a row) */
    int32x4_t t0 = vaddq_s32(c0, c2), t1 = vsubq_s32(c0, c2);
    int32x4_t t2 = vsubq_s32(vshrq_n_s32(c1, 1), c3), t3 = vaddq_s32(c1, vshrq_n_s32(c3, 1));
    int32x4x4_t x;
    x.val[0] = vaddq_s32(t0, t3);
    x.val[1] = vaddq_s32(t1, t2);
    x.val[2] = vsubq_s32(t1, t2);
    x.val[3] = vsubq_s32(t0, t3);
    vst4q_s32(d32, x);   /* row-major: data[4 * row + column] */
    /* vertical transform (each lane a column) */
    int32x4_t a0 = vld1q_s32(d32), a1 = vld1q_s32(d32 + 4), a2 = vld1q_s32(d32 + 8), a3 = vld1q_s32(d32 + 12);
    t0 = vaddq_s32(a0, a2);
    t1 = vsubq_s32(a0, a2);
    t2 = vsubq_s32(vshrq_n_s32(a1, 1), a3);
    t3 = vaddq_s32(a1, vshrq_n_s32(a3, 1));
    int32x4_t o0 = vshrq_n_s32(vaddq_n_s32(vaddq_s32(t0, t3), 32), 6);
    int32x4_t o1 = vshrq_n_s32(vaddq_n_s32(vaddq_s32(t1, t2), 32), 6);
    int32x4_t o2 = vshrq_n_s32(vaddq_n_s32(vsubq_s32(t1, t2), 32), 6);
    int32x4_t o3 = vshrq_n_s32(vaddq_n_s32(vsubq_s32(t0, t3), 32), 6);
    /* each value in [-512, 511] */
    uint32x4_t lim = vdupq_n_u32(1023);
    mve_pred16_t bad = vcmphiq_u32(vreinterpretq_u32_s32(vaddq_n_s32(o0, 512)), lim) |
                       vcmphiq_u32(vreinterpretq_u32_s32(vaddq_n_s32(o1, 512)), lim) |
                       vcmphiq_u32(vreinterpretq_u32_s32(vaddq_n_s32(o2, 512)), lim) |
                       vcmphiq_u32(vreinterpretq_u32_s32(vaddq_n_s32(o3, 512)), lim);
    if (bad)
        return(HANTRO_NOK);
    vst1q_s32(d32, o0);
    vst1q_s32(d32 + 4, o1);
    vst1q_s32(d32 + 8, o2);
    vst1q_s32(d32 + 12, o3);
    return(HANTRO_OK);
}
#endif

u32 h264bsdProcessBlock(i32 *data, u32 qp, u32 skip, u32 coeffMap)
{

/* Variables */

    i32 tmp0, tmp1, tmp2, tmp3;
    i32 d1, d2, d3;
    u32 row,col;
    u32 qpDiv;
    i32 *ptr;

/* Code */

    qpDiv = qpDiv6[qp];
    tmp1 = levelScale[qpMod6[qp]][0] << qpDiv;
    tmp2 = levelScale[qpMod6[qp]][1] << qpDiv;
    tmp3 = levelScale[qpMod6[qp]][2] << qpDiv;

#if defined(H264BSD_MVE)
    if (coeffMap & 0xFF9C)
        return ProcessBlockMve(data, tmp1, tmp2, tmp3, skip);
#endif
    if (!skip)
        data[0] = (data[0] * tmp1);

    /* at least one of the rows 1, 2 or 3 contain non-zero coeffs, mask takes
     * the scanning order into account */
    if (coeffMap & 0xFF9C)
    {
        /* do the zig-zag scan and inverse quantization */
        d1 = data[1];
        d2 = data[14];
        d3 = data[15];
        data[1] = (d1 * tmp2);
        data[14] = (d2 * tmp2);
        data[15] = (d3 * tmp3);

        d1 = data[2];
        d2 = data[5];
        d3 = data[4];
        data[4] = (d1 * tmp2);
        data[2]  = (d2 * tmp1);
        data[5] = (d3 * tmp3);

        d1 = data[8];
        d2 = data[3];
        d3 = data[6];
        tmp0 = (d1 * tmp2);
        data[8] = (d2 * tmp1);
        data[3]  = (d3 * tmp2);
        d1 = data[7];
        d2 = data[12];
        d3 = data[9];
        data[6]  = (d1 * tmp2);
        data[7]  = (d2 * tmp3);
        data[12] = (d3 * tmp2);
        data[9]  = tmp0;

        d1 = data[10];
        d2 = data[11];
        d3 = data[13];
        data[13] = (d1 * tmp3);
        data[10] = (d2 * tmp1);
        data[11] = (d3 * tmp2);

        /* horizontal transform */
        for (row = 4, ptr = data; row--; ptr += 4)
        {
            tmp0 = ptr[0] + ptr[2];
            tmp1 = ptr[0] - ptr[2];
            tmp2 = (ptr[1] >> 1) - ptr[3];
            tmp3 = ptr[1] + (ptr[3] >> 1);
            ptr[0] = tmp0 + tmp3;
            ptr[1] = tmp1 + tmp2;
            ptr[2] = tmp1 - tmp2;
            ptr[3] = tmp0 - tmp3;
        }

        /*lint +e661 +e662*/
        /* then vertical transform */
        for (col = 4; col--; data++)
        {
            tmp0 = data[0] + data[8];
            tmp1 = data[0] - data[8];
            tmp2 = (data[4] >> 1) - data[12];
            tmp3 = data[4] + (data[12] >> 1);
            data[0 ] = (tmp0 + tmp3 + 32)>>6;
            data[4 ] = (tmp1 + tmp2 + 32)>>6;
            data[8 ] = (tmp1 - tmp2 + 32)>>6;
            data[12] = (tmp0 - tmp3 + 32)>>6;
            /* check that each value is in the range [-512,511] */
            if (((u32)(data[0] + 512) > 1023) ||
                ((u32)(data[4] + 512) > 1023) ||
                ((u32)(data[8] + 512) > 1023) ||
                ((u32)(data[12] + 512) > 1023) )
                return(HANTRO_NOK);
        }
    }
    else /* rows 1, 2 and 3 are zero */
    {
        /* only dc-coeff is non-zero, i.e. coeffs at original positions
         * 1, 5 and 6 are zero */
        if ((coeffMap & 0x62) == 0)
        {
            tmp0 = (data[0] + 32) >> 6;
            /* check that value is in the range [-512,511] */
            if ((u32)(tmp0 + 512) > 1023)
                return(HANTRO_NOK);
            data[0] = data[1]  = data[2]  = data[3]  = data[4]  = data[5]  =
                      data[6]  = data[7]  = data[8]  = data[9]  = data[10] =
                      data[11] = data[12] = data[13] = data[14] = data[15] =
                      tmp0;
        }
        else /* at least one of the coeffs 1, 5 or 6 is non-zero */
        {
            data[1] = (data[1] * tmp2);
            data[2] = (data[5] * tmp1);
            data[3] = (data[6] * tmp2);
            tmp0 = data[0] + data[2];
            tmp1 = data[0] - data[2];
            tmp2 = (data[1] >> 1) - data[3];
            tmp3 = data[1] + (data[3] >> 1);
            data[0] = (tmp0 + tmp3 + 32)>>6;
            data[1] = (tmp1 + tmp2 + 32)>>6;
            data[2] = (tmp1 - tmp2 + 32)>>6;
            data[3] = (tmp0 - tmp3 + 32)>>6;
            data[4] = data[8] = data[12] = data[0];
            data[5] = data[9] = data[13] = data[1];
            data[6] = data[10] = data[14] = data[2];
            data[7] = data[11] = data[15] = data[3];
            /* check that each value is in the range [-512,511] */
            if (((u32)(data[0] + 512) > 1023) ||
                ((u32)(data[1] + 512) > 1023) ||
                ((u32)(data[2] + 512) > 1023) ||
                ((u32)(data[3] + 512) > 1023) )
                return(HANTRO_NOK);
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdProcessLumaDc

        Functional description:
            Function performs inverse zig-zag scan, inverse transform and
            inverse scaling for a luma DC coefficients block

        Inputs:
            data            pointer to data to be processed
            qp              quantization parameter

        Outputs:
            data            processed data

        Returns:
            none

------------------------------------------------------------------------------*/
void h264bsdProcessLumaDc(i32 *data, u32 qp)
{

/* Variables */

    i32 tmp0, tmp1, tmp2, tmp3;
    u32 row,col;
    u32 qpMod, qpDiv;
    i32 levScale;
    i32 *ptr;

/* Code */

    qpMod = qpMod6[qp];
    qpDiv = qpDiv6[qp];

    /* zig-zag scan */
    tmp0 = data[2];
    data[2]  = data[5];
    data[5] = data[4];
    data[4] = tmp0;

    tmp0 = data[8];
    data[8] = data[3];
    data[3]  = data[6];
    data[6]  = data[7];
    data[7]  = data[12];
    data[12] = data[9];
    data[9]  = tmp0;

    tmp0 = data[10];
    data[10] = data[11];
    data[11] = data[13];
    data[13] = tmp0;

    /* horizontal transform */
    for (row = 4, ptr = data; row--; ptr += 4)
    {
        tmp0 = ptr[0] + ptr[2];
        tmp1 = ptr[0] - ptr[2];
        tmp2 = ptr[1] - ptr[3];
        tmp3 = ptr[1] + ptr[3];
        ptr[0] = tmp0 + tmp3;
        ptr[1] = tmp1 + tmp2;
        ptr[2] = tmp1 - tmp2;
        ptr[3] = tmp0 - tmp3;
    }

    /*lint +e661 +e662*/
    /* then vertical transform and inverse scaling */
    levScale = levelScale[ qpMod ][0];
    if (qp >= 12)
    {
        levScale <<= (qpDiv-2);
        for (col = 4; col--; data++)
        {
            tmp0 = data[0] + data[8 ];
            tmp1 = data[0] - data[8 ];
            tmp2 = data[4] - data[12];
            tmp3 = data[4] + data[12];
            data[0 ] = ((tmp0 + tmp3)*levScale);
            data[4 ] = ((tmp1 + tmp2)*levScale);
            data[8 ] = ((tmp1 - tmp2)*levScale);
            data[12] = ((tmp0 - tmp3)*levScale);
        }
    }
    else
    {
        i32 tmp;
        tmp = ((1 - qpDiv) == 0) ? 1 : 2;
        for (col = 4; col--; data++)
        {
            tmp0 = data[0] + data[8 ];
            tmp1 = data[0] - data[8 ];
            tmp2 = data[4] - data[12];
            tmp3 = data[4] + data[12];
            data[0 ] = ((tmp0 + tmp3)*levScale+tmp) >> (2-qpDiv);
            data[4 ] = ((tmp1 + tmp2)*levScale+tmp) >> (2-qpDiv);
            data[8 ] = ((tmp1 - tmp2)*levScale+tmp) >> (2-qpDiv);
            data[12] = ((tmp0 - tmp3)*levScale+tmp) >> (2-qpDiv);
        }
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdProcessChromaDc

        Functional description:
            Function performs inverse transform and inverse scaling for a
            chroma DC coefficients block

        Inputs:
            data            pointer to data to be processed
            qp              quantization parameter

        Outputs:
            data            processed data

        Returns:
            none

------------------------------------------------------------------------------*/
void h264bsdProcessChromaDc(i32 *data, u32 qp)
{

/* Variables */

    i32 tmp0, tmp1, tmp2, tmp3;
    u32 qpDiv;
    i32 levScale;
    u32 levShift;

/* Code */

    qpDiv = qpDiv6[qp];
    levScale = levelScale[ qpMod6[qp] ][0];

    if (qp >= 6)
    {
        levScale <<= (qpDiv-1);
        levShift = 0;
    }
    else
    {
        levShift = 1;
    }

    tmp0 = data[0] + data[2];
    tmp1 = data[0] - data[2];
    tmp2 = data[1] - data[3];
    tmp3 = data[1] + data[3];
    data[0] = ((tmp0 + tmp3) * levScale) >> levShift;
    data[1] = ((tmp0 - tmp3) * levScale) >> levShift;
    data[2] = ((tmp1 + tmp2) * levScale) >> levShift;
    data[3] = ((tmp1 - tmp2) * levScale) >> levShift;
    tmp0 = data[4] + data[6];
    tmp1 = data[4] - data[6];
    tmp2 = data[5] - data[7];
    tmp3 = data[5] + data[7];
    data[4] = ((tmp0 + tmp3) * levScale) >> levShift;
    data[5] = ((tmp0 - tmp3) * levScale) >> levShift;
    data[6] = ((tmp1 + tmp2) * levScale) >> levShift;
    data[7] = ((tmp1 - tmp2) * levScale) >> levShift;

}

/*lint +e701 +e702 */


