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
          h264bsdDecodeVuiParameters
          DecodeHrdParameters

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_vui.h"
#include "basetype.h"
#include "h264bsd_vlc.h"
#include "h264bsd_stream.h"
#include "h264bsd_cfg.h"
#include "h264bsd_util.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

#define MAX_DPB_SIZE 16
#define MAX_BR       240000 /* for level 5.1 */
#define MAX_CPB      240000 /* for level 5.1 */

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

#if !H264BSD_LEAN_VUI
static u32 DecodeHrdParameters(
  strmData_t *pStrmData,
  hrdParameters_t *pHrdParameters);
#endif

#if H264BSD_LEAN_VUI
/*------------------------------------------------------------------------------

    OPT: H264BSD_LEAN_VUI (off by default), for ports short of code space:
    the same walk through every VUI field in the standard's order, with the
    full parser's end-of-stream checks and validations unchanged (so a VUI is
    accepted or refused exactly as before and the walk ends on the same bit,
    where h264bsd_seq_param_set.c checks the RBSP trailing bits), but only
    what the decoder reads is stored: bitstream_restriction_flag,
    num_reorder_frames and max_dec_frame_buffering (h264bsd_seq_param_set.c's
    checks and maxDpbSize, h264bsd_storage.c's output bumping) and the two
    log2_max_mv_length fields, with the full parser's defaults when
    bitstream_restriction is absent. Everything else (aspect ratio, overscan,
    video signal type, chroma location, timing, both hrd_parameters with
    their cpb loops, low_delay, pic_struct, the other restriction fields) is
    read and dropped; the rest of vuiParameters_t stays zero. The HRD fields'
    only reader is SEI parsing, which nothing calls (h264bsd_decoder.c skips
    SEI NAL units) and which this compiles out (h264bsd_sei.c). Code: the
    full parser is about 5 KB on a 32-bit Arm core at -O2 (the bit readers
    inlined at every field); this is under 1 KB, its readers out of line.

------------------------------------------------------------------------------*/

/* The VUI is read once per SPS: each reader below is one out-of-line function,
 * so the inlined bit-reader primitives (h264bsd_stream.h) are expanded once
 * here instead of at every field (that, not the stores, is the full parser's
 * size). */
#if defined(__GNUC__)
#define VUI_READER __attribute__((noinline, cold))
#else
#define VUI_READER
#endif

/* n bits into *v (n < 32); HANTRO_NOK at the end of the stream */
static VUI_READER u32 VuiBits(strmData_t *pStrmData, u32 n, u32 *v)
{
    u32 tmp = h264bsdGetBits(pStrmData, n);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    *v = tmp;
    return(HANTRO_OK);
}

/* one ue(v) into *v, refused above max (as the full parser refuses it) */
static VUI_READER u32 VuiUe(strmData_t *pStrmData, u32 max, u32 *v)
{
    u32 tmp = h264bsdDecodeExpGolombUnsigned(pStrmData, v);
    if (tmp != HANTRO_OK)
        return(tmp);
    return(*v > max ? HANTRO_NOK : HANTRO_OK);
}

/* 32 bits that must not be 0 (num_units_in_tick, time_scale) */
static VUI_READER u32 VuiNonZero32(strmData_t *pStrmData)
{
    u32 tmp = h264bsdShowBits32(pStrmData);
    if (h264bsdFlushBits(pStrmData, 32) == END_OF_STREAM)
        return(HANTRO_NOK);
    return(tmp ? HANTRO_OK : HANTRO_NOK);
}

/* hrd_parameters(), walked: cpb_cnt_minus1 + 1 (u32, as the full parser
 * counts it) <= MAX_CPB_CNT, the two scales, each cpb's bit rate and size
 * (each <= 2^32 - 2) and cbr flag, the four lengths */
static VUI_READER u32 VuiHrd(strmData_t *pStrmData)
{
    u32 cnt, v, i;
    if (VuiUe(pStrmData, 0xFFFFFFFFU, &cnt) != HANTRO_OK)
        return(HANTRO_NOK);
    cnt++;
    if (cnt > MAX_CPB_CNT || VuiBits(pStrmData, 8, &v) != HANTRO_OK)
        return(HANTRO_NOK);
    for (i = 0; i < cnt; i++)
        if (VuiUe(pStrmData, 4294967294U, &v) != HANTRO_OK ||
            VuiUe(pStrmData, 4294967294U, &v) != HANTRO_OK ||
            VuiBits(pStrmData, 1, &v) != HANTRO_OK)
            return(HANTRO_NOK);
    return(VuiBits(pStrmData, 20, &v));
}

u32 h264bsdDecodeVuiParameters(strmData_t *pStrmData,
    vuiParameters_t *pVuiParameters)
{
    u32 f, v, hrd;

    ASSERT(pStrmData);
    ASSERT(pVuiParameters);

    memset(pVuiParameters, 0, sizeof(vuiParameters_t));

    /* aspect_ratio_info_present_flag: aspect_ratio_idc; Extended_SAR's
     * sar_width, sar_height */
    if (VuiBits(pStrmData, 1, &f) != HANTRO_OK)
        return(HANTRO_NOK);
    if (f)
    {
        if (VuiBits(pStrmData, 8, &v) != HANTRO_OK)
            return(HANTRO_NOK);
        if (v == ASPECT_RATIO_EXTENDED_SAR &&
            (VuiBits(pStrmData, 16, &v) != HANTRO_OK ||
             VuiBits(pStrmData, 16, &v) != HANTRO_OK))
            return(HANTRO_NOK);
    }

    /* overscan_info_present_flag: overscan_appropriate_flag */
    if (VuiBits(pStrmData, 1, &f) != HANTRO_OK ||
        (f && VuiBits(pStrmData, 1, &v) != HANTRO_OK))
        return(HANTRO_NOK);

    /* video_signal_type_present_flag: video_format, video_full_range_flag,
     * colour_description_present_flag: colour_primaries,
     * transfer_characteristics, matrix_coefficients */
    if (VuiBits(pStrmData, 1, &f) != HANTRO_OK)
        return(HANTRO_NOK);
    if (f)
    {
        if (VuiBits(pStrmData, 3, &v) != HANTRO_OK ||
            VuiBits(pStrmData, 1, &v) != HANTRO_OK ||
            VuiBits(pStrmData, 1, &f) != HANTRO_OK)
            return(HANTRO_NOK);
        if (f && (VuiBits(pStrmData, 8, &v) != HANTRO_OK ||
                  VuiBits(pStrmData, 8, &v) != HANTRO_OK ||
                  VuiBits(pStrmData, 8, &v) != HANTRO_OK))
            return(HANTRO_NOK);
    }

    /* chroma_loc_info_present_flag: the two chroma_sample_loc_types, <= 5 */
    if (VuiBits(pStrmData, 1, &f) != HANTRO_OK)
        return(HANTRO_NOK);
    if (f && (VuiUe(pStrmData, 5, &v) != HANTRO_OK ||
              VuiUe(pStrmData, 5, &v) != HANTRO_OK))
        return(HANTRO_NOK);

    /* timing_info_present_flag: num_units_in_tick, time_scale (both
     * non-zero), fixed_frame_rate_flag */
    if (VuiBits(pStrmData, 1, &f) != HANTRO_OK)
        return(HANTRO_NOK);
    if (f && (VuiNonZero32(pStrmData) != HANTRO_OK ||
              VuiNonZero32(pStrmData) != HANTRO_OK ||
              VuiBits(pStrmData, 1, &v) != HANTRO_OK))
        return(HANTRO_NOK);

    /* nal_ and vcl_hrd_parameters_present_flag: hrd_parameters() each;
     * low_delay_hrd_flag after either */
    if (VuiBits(pStrmData, 1, &hrd) != HANTRO_OK ||
        (hrd && VuiHrd(pStrmData) != HANTRO_OK) ||
        VuiBits(pStrmData, 1, &f) != HANTRO_OK ||
        (f && VuiHrd(pStrmData) != HANTRO_OK) ||
        ((hrd || f) && VuiBits(pStrmData, 1, &v) != HANTRO_OK))
        return(HANTRO_NOK);

    /* pic_struct_present_flag; bitstream_restriction_flag:
     * motion_vectors_over_pic_boundaries_flag, max_bytes_per_pic_denom,
     * max_bits_per_mb_denom, log2_max_mv_length_horizontal / _vertical (each
     * <= 16, the last two kept), then the two the DPB is sized by */
    if (VuiBits(pStrmData, 1, &v) != HANTRO_OK ||
        VuiBits(pStrmData, 1, &f) != HANTRO_OK)
        return(HANTRO_NOK);
    pVuiParameters->bitstreamRestrictionFlag = f ? HANTRO_TRUE : HANTRO_FALSE;
    if (f)
    {
        if (VuiBits(pStrmData, 1, &v) != HANTRO_OK ||
            VuiUe(pStrmData, 16, &v) != HANTRO_OK ||
            VuiUe(pStrmData, 16, &v) != HANTRO_OK ||
            VuiUe(pStrmData, 16,
                  &pVuiParameters->log2MaxMvLengthHorizontal) != HANTRO_OK ||
            VuiUe(pStrmData, 16,
                  &pVuiParameters->log2MaxMvLengthVertical) != HANTRO_OK ||
            VuiUe(pStrmData, 0xFFFFFFFFU,
                  &pVuiParameters->numReorderFrames) != HANTRO_OK ||
            VuiUe(pStrmData, 0xFFFFFFFFU,
                  &pVuiParameters->maxDecFrameBuffering) != HANTRO_OK)
            return(HANTRO_NOK);
    }
    else
    {
        pVuiParameters->log2MaxMvLengthHorizontal = 16;
        pVuiParameters->log2MaxMvLengthVertical   = 16;
        pVuiParameters->numReorderFrames          = MAX_DPB_SIZE;
        pVuiParameters->maxDecFrameBuffering      = MAX_DPB_SIZE;
    }

    return(HANTRO_OK);
}
#else

/*------------------------------------------------------------------------------

    Function: h264bsdDecodeVuiParameters

        Functional description:
            Decode VUI parameters from the stream. See standard for details.

        Inputs:
            pStrmData       pointer to stream data structure

        Outputs:
            pVuiParameters  decoded information is stored here

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      invalid stream data or end of stream

------------------------------------------------------------------------------*/

u32 h264bsdDecodeVuiParameters(strmData_t *pStrmData,
    vuiParameters_t *pVuiParameters)
{

/* Variables */

    u32 tmp;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pVuiParameters);

    memset(pVuiParameters, 0, sizeof(vuiParameters_t));

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->aspectRatioPresentFlag = (tmp == 1) ?
                                HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->aspectRatioPresentFlag)
    {
        tmp = h264bsdGetBits(pStrmData, 8);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->aspectRatioIdc = tmp;

        if (pVuiParameters->aspectRatioIdc == ASPECT_RATIO_EXTENDED_SAR)
        {
            tmp = h264bsdGetBits(pStrmData, 16);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
            pVuiParameters->sarWidth = tmp;

            tmp = h264bsdGetBits(pStrmData, 16);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
            pVuiParameters->sarHeight = tmp;
        }
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->overscanInfoPresentFlag = (tmp == 1) ?
                                HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->overscanInfoPresentFlag)
    {
        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->overscanAppropriateFlag = (tmp == 1) ?
                                HANTRO_TRUE : HANTRO_FALSE;
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->videoSignalTypePresentFlag = (tmp == 1) ?
                                HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->videoSignalTypePresentFlag)
    {
        tmp = h264bsdGetBits(pStrmData, 3);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->videoFormat = tmp;

        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->videoFullRangeFlag = (tmp == 1) ?
                                HANTRO_TRUE : HANTRO_FALSE;

        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->colourDescriptionPresentFlag =
            (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

        if (pVuiParameters->colourDescriptionPresentFlag)
        {
            tmp = h264bsdGetBits(pStrmData, 8);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
            pVuiParameters->colourPrimaries = tmp;

            tmp = h264bsdGetBits(pStrmData, 8);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
            pVuiParameters->transferCharacteristics = tmp;

            tmp = h264bsdGetBits(pStrmData, 8);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
            pVuiParameters->matrixCoefficients = tmp;
        }
        else
        {
            pVuiParameters->colourPrimaries         = 2;
            pVuiParameters->transferCharacteristics = 2;
            pVuiParameters->matrixCoefficients      = 2;
        }
    }
    else
    {
        pVuiParameters->videoFormat             = 5;
        pVuiParameters->colourPrimaries         = 2;
        pVuiParameters->transferCharacteristics = 2;
        pVuiParameters->matrixCoefficients      = 2;
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->chromaLocInfoPresentFlag =
        (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->chromaLocInfoPresentFlag)
    {
        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->chromaSampleLocTypeTopField);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pVuiParameters->chromaSampleLocTypeTopField > 5)
            return(HANTRO_NOK);

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->chromaSampleLocTypeBottomField);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pVuiParameters->chromaSampleLocTypeBottomField > 5)
            return(HANTRO_NOK);
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->timingInfoPresentFlag =
        (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->timingInfoPresentFlag)
    {
        tmp = h264bsdShowBits32(pStrmData);
        if (h264bsdFlushBits(pStrmData, 32) == END_OF_STREAM)
            return(HANTRO_NOK);
        if (tmp == 0)
            return(HANTRO_NOK);
        pVuiParameters->numUnitsInTick = tmp;

        tmp = h264bsdShowBits32(pStrmData);
        if (h264bsdFlushBits(pStrmData, 32) == END_OF_STREAM)
            return(HANTRO_NOK);
        if (tmp == 0)
            return(HANTRO_NOK);
        pVuiParameters->timeScale = tmp;

        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->fixedFrameRateFlag =
            (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->nalHrdParametersPresentFlag =
        (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->nalHrdParametersPresentFlag)
    {
        tmp = DecodeHrdParameters(pStrmData, &pVuiParameters->nalHrdParameters);
        if (tmp != HANTRO_OK)
            return(tmp);
    }
    else
    {
        pVuiParameters->nalHrdParameters.cpbCnt          = 1;
        /* MaxBR and MaxCPB should be the values correspondig to the levelIdc
         * in the SPS containing these VUI parameters. However, these values
         * are not used anywhere and maximum for any level will be used here */
        pVuiParameters->nalHrdParameters.bitRateValue[0] = 1200 * MAX_BR + 1;
        pVuiParameters->nalHrdParameters.cpbSizeValue[0] = 1200 * MAX_CPB + 1;
        pVuiParameters->nalHrdParameters.initialCpbRemovalDelayLength = 24;
        pVuiParameters->nalHrdParameters.cpbRemovalDelayLength        = 24;
        pVuiParameters->nalHrdParameters.dpbOutputDelayLength         = 24;
        pVuiParameters->nalHrdParameters.timeOffsetLength             = 24;
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->vclHrdParametersPresentFlag =
        (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->vclHrdParametersPresentFlag)
    {
        tmp = DecodeHrdParameters(pStrmData, &pVuiParameters->vclHrdParameters);
        if (tmp != HANTRO_OK)
            return(tmp);
    }
    else
    {
        pVuiParameters->vclHrdParameters.cpbCnt          = 1;
        /* MaxBR and MaxCPB should be the values correspondig to the levelIdc
         * in the SPS containing these VUI parameters. However, these values
         * are not used anywhere and maximum for any level will be used here */
        pVuiParameters->vclHrdParameters.bitRateValue[0] = 1000 * MAX_BR + 1;
        pVuiParameters->vclHrdParameters.cpbSizeValue[0] = 1000 * MAX_CPB + 1;
        pVuiParameters->vclHrdParameters.initialCpbRemovalDelayLength = 24;
        pVuiParameters->vclHrdParameters.cpbRemovalDelayLength        = 24;
        pVuiParameters->vclHrdParameters.dpbOutputDelayLength         = 24;
        pVuiParameters->vclHrdParameters.timeOffsetLength             = 24;
    }

    if (pVuiParameters->nalHrdParametersPresentFlag ||
      pVuiParameters->vclHrdParametersPresentFlag)
    {
        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->lowDelayHrdFlag =
            (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;
    }

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->picStructPresentFlag =
        (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

    tmp = h264bsdGetBits(pStrmData, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pVuiParameters->bitstreamRestrictionFlag =
        (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

    if (pVuiParameters->bitstreamRestrictionFlag)
    {
        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pVuiParameters->motionVectorsOverPicBoundariesFlag =
            (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->maxBytesPerPicDenom);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pVuiParameters->maxBytesPerPicDenom > 16)
            return(HANTRO_NOK);

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->maxBitsPerMbDenom);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pVuiParameters->maxBitsPerMbDenom > 16)
            return(HANTRO_NOK);

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->log2MaxMvLengthHorizontal);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pVuiParameters->log2MaxMvLengthHorizontal > 16)
            return(HANTRO_NOK);

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->log2MaxMvLengthVertical);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pVuiParameters->log2MaxMvLengthVertical > 16)
            return(HANTRO_NOK);

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->numReorderFrames);
        if (tmp != HANTRO_OK)
            return(tmp);

        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pVuiParameters->maxDecFrameBuffering);
        if (tmp != HANTRO_OK)
            return(tmp);
    }
    else
    {
        pVuiParameters->motionVectorsOverPicBoundariesFlag = HANTRO_TRUE;
        pVuiParameters->maxBytesPerPicDenom       = 2;
        pVuiParameters->maxBitsPerMbDenom         = 1;
        pVuiParameters->log2MaxMvLengthHorizontal = 16;
        pVuiParameters->log2MaxMvLengthVertical   = 16;
        pVuiParameters->numReorderFrames          = MAX_DPB_SIZE;
        pVuiParameters->maxDecFrameBuffering      = MAX_DPB_SIZE;
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: DecodeHrdParameters

        Functional description:
            Decode HRD parameters from the stream. See standard for details.

        Inputs:
            pStrmData       pointer to stream data structure

        Outputs:
            pHrdParameters  decoded information is stored here

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      invalid stream data

------------------------------------------------------------------------------*/

static u32 DecodeHrdParameters(
  strmData_t *pStrmData,
  hrdParameters_t *pHrdParameters)
{

/* Variables */

    u32 tmp, i;

/* Code */

    ASSERT(pStrmData);
    ASSERT(pHrdParameters);


    tmp = h264bsdDecodeExpGolombUnsigned(pStrmData, &pHrdParameters->cpbCnt);
    if (tmp != HANTRO_OK)
        return(tmp);
    /* cpbCount = cpb_cnt_minus1 + 1 */
    pHrdParameters->cpbCnt++;
    if (pHrdParameters->cpbCnt > MAX_CPB_CNT)
        return(HANTRO_NOK);

    tmp = h264bsdGetBits(pStrmData, 4);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pHrdParameters->bitRateScale = tmp;

    tmp = h264bsdGetBits(pStrmData, 4);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pHrdParameters->cpbSizeScale = tmp;

    for (i = 0; i < pHrdParameters->cpbCnt; i++)
    {
        /* bit_rate_value_minus1 in the range [0, 2^32 - 2] */
        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pHrdParameters->bitRateValue[i]);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pHrdParameters->bitRateValue[i] > 4294967294U)
            return(HANTRO_NOK);
        pHrdParameters->bitRateValue[i]++;
        /* this may result in overflow, but this value is not used for
         * anything */
        pHrdParameters->bitRateValue[i] *=
            1 << (6 + pHrdParameters->bitRateScale);

        /* cpb_size_value_minus1 in the range [0, 2^32 - 2] */
        tmp = h264bsdDecodeExpGolombUnsigned(pStrmData,
          &pHrdParameters->cpbSizeValue[i]);
        if (tmp != HANTRO_OK)
            return(tmp);
        if (pHrdParameters->cpbSizeValue[i] > 4294967294U)
            return(HANTRO_NOK);
        pHrdParameters->cpbSizeValue[i]++;
        /* this may result in overflow, but this value is not used for
         * anything */
        pHrdParameters->cpbSizeValue[i] *=
            1 << (4 + pHrdParameters->cpbSizeScale);

        tmp = h264bsdGetBits(pStrmData, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        pHrdParameters->cbrFlag[i] = (tmp == 1) ? HANTRO_TRUE : HANTRO_FALSE;
    }

    tmp = h264bsdGetBits(pStrmData, 5);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pHrdParameters->initialCpbRemovalDelayLength = tmp + 1;

    tmp = h264bsdGetBits(pStrmData, 5);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pHrdParameters->cpbRemovalDelayLength = tmp + 1;

    tmp = h264bsdGetBits(pStrmData, 5);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pHrdParameters->dpbOutputDelayLength = tmp + 1;

    tmp = h264bsdGetBits(pStrmData, 5);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    pHrdParameters->timeOffsetLength = tmp;

    return(HANTRO_OK);

}

#endif /* H264BSD_LEAN_VUI */
