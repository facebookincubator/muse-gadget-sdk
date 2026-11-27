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
          h264bsdDecodeSliceData
          SetMbParams
          h264bsdMarkSliceCorrupted

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_slice_data.h"
#include "h264bsd_deblocking.h"
#include "h264bsd_util.h"
#include "h264bsd_vlc.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static void SetMbParams(mbStorage_t *pMb, sliceHeader_t *pSlice, u32 sliceId,
    i32 chromaQpIndexOffset);

/*------------------------------------------------------------------------------

   5.1  Function name: h264bsdDecodeSliceData

        Functional description:
            Decode one slice. Function decodes stream data, i.e. macroblocks
            and possible skip_run fields. h264bsdDecodeMacroblock function is
            called to handle all other macroblock related processing.
            Macroblock to slice group mapping is considered when next
            macroblock to process is determined (h264bsdNextMbAddress function)
            map

        Inputs:
            pStrmData       pointer to stream data structure
            pStorage        pointer to storage structure
            currImage       pointer to current processed picture, needed for
                            intra prediction of the macroblocks
            pSliceHeader    pointer to slice header of the current slice

        Outputs:
            currImage       processed macroblocks are written to current image
            pStorage        mbStorage structure of each processed macroblock
                            is updated here

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      invalid stream data

------------------------------------------------------------------------------*/

/* OPT: the current macroblock's entry and the entries' base: in low-memory
 * mode a ring (h264bsd_lowmem.c), not indexed by address */
#if H264BSD_LOWMEM
#define CURR_MB pMb
#define MB_BASE (pStorage->lmRingRows ? pStorage->lmMb : pStorage->mb)
#else
#define CURR_MB (pStorage->mb + currMbAddr)
#define MB_BASE pStorage->mb
#endif

#if H264BSD_CHANGED_MAP
/* OPT: h264bsdChangedMap: a macroblock is clean, its pixels the previous
 * output picture's before the loop filter, when it is inter predicted from
 * reference index 0 (the previous picture: h264bsd_decoder.c ChmStart) with
 * every motion vector zero (full-pel: the prediction is the same 16x16 luma
 * and 8x8 chroma samples) and adds no residual: a skipped macroblock, or
 * every residual block h264bsdWriteOutputBlocks adds is empty or all zero
 * after the inverse transform (an encoder may code coefficients that
 * reconstruct to nothing: a coefficient count would call those changed).
 * When its slice deblocks, a macroblock with luma coefficients is changed even
 * so: the loop filter's boundary strength counts them and filters the
 * macroblock's inner edges. Anything else (intra, I_PCM, a vector, a
 * residual sample, another reference) leaves its bit 0: changed. Called
 * after h264bsdDecodeMacroblock (the residual still in pStorage->mbLayer). */
H264BSD_HOT static void ChmMb(storage_t *s, const mbStorage_t *mb, u32 addr)
{
    u32 i, j;
    if (mb->mbType > P_8x8ref0)
        return;
    for (i = 0; i < 4; i++)
        if (mb->refPic[i])
            return;
    for (i = 0; i < 16; i++)
        if (mb->mv[i].hor | mb->mv[i].ver)
            return;
    if (mb->mbType != P_Skip)
    {
        if (mb->disableDeblockingFilterIdc != 1 && mb->nzMask)
            return;
#ifdef H264DEC_OMXDL
        for (i = 0; i < 27; i++)
            if (mb->totalCoeff[i])
                return;
#else
        for (i = 0; i < 24; i++)
        {
            const i32 *r = s->mbLayer->residual.level[i];
#if defined(H264BSD_MVE)
            /* Helium's inter residual marks no empty blocks: resMask holds
             * the ones it added (ProcessResidualInter) */
            if (!(s->mbLayer->residual.resMask >> i & 1))
                continue;
#else
            if (IS_RESIDUAL_EMPTY(r))
                continue;
#endif
            for (j = 0; j < 16; j++)
                if (r[j])
                    return;
        }
#endif
    }
    s->chmBits[addr >> 3] |= (u8)(1u << (addr & 7));
}
#endif

H264BSD_HOT u32 h264bsdDecodeSliceData(strmData_t *pStrmData, storage_t *pStorage,
    image_t *currImage, sliceHeader_t *pSliceHeader)
{

/* Variables */

    u8 mbData[384 + 15 + 32];
    u8 *data;
    u32 tmp;
    u32 skipRun;
    u32 prevSkipped;
    u32 currMbAddr;
    u32 moreMbs;
    u32 mbCount;
    i32 qpY;
    macroblockLayer_t *mbLayer;
#if H264BSD_LOWMEM
    mbStorage_t *pMb;
#endif

/* Code */

    ASSERT(pStrmData);
    ASSERT(pSliceHeader);
    ASSERT(pStorage);
    ASSERT(pSliceHeader->firstMbInSlice < pStorage->picSizeInMbs);

    /* ensure 16-byte alignment */
    data = (u8*)ALIGN(mbData, 16);

    mbLayer = pStorage->mbLayer;

    currMbAddr = pSliceHeader->firstMbInSlice;
    skipRun = 0;
    prevSkipped = HANTRO_FALSE;

    /* increment slice index, will be one for decoding of the first slice of
     * the picture */
    pStorage->slice->sliceId++;

    /* lastMbAddr stores address of the macroblock that was last successfully
     * decoded, needed for error handling */
    pStorage->slice->lastMbAddr = 0;

    mbCount = 0;
    /* initial quantization parameter for the slice is obtained as the sum of
     * initial QP for the picture and sliceQpDelta for the current slice */
    qpY = (i32)pStorage->activePps->picInitQp + pSliceHeader->sliceQpDelta;
    do
    {
#if H264BSD_STRIP
        /* OPT: strip mode: make room in the row ring for a new row. Only
         * the next macroblock in raster order can come in (rows that left
         * the ring, or skipping rows, would address outside it, and a row
         * is handed out only once all of it is decoded), checked before
         * anything reads the entry or a row is handed out. */
        if (pStorage->stripMode)
        {
            u32 row = currMbAddr / currImage->width;
            if (currMbAddr != pStorage->dbNextMb || pStorage->dbBad ||
                row < pStorage->stripBase ||
                row > pStorage->stripBase + pStorage->stripMode + 2)
                return(HANTRO_NOK);
            if (row == pStorage->stripBase + pStorage->stripMode + 2)
                h264bsdStripRow(pStorage, row);
        }
#endif
#if H264BSD_LOWMEM
        /* OPT: low-memory mode: the macroblock ring and in-place pictures
         * (h264bsd_lowmem.c) */
        if (pStorage->lmRingRows)
        {
            u32 w = currImage->width;
            pMb = h264bsdLowMemMb(pStorage, currMbAddr);
            if (!pMb || pStorage->dbBad || pStorage->lmIpBad)
                return(HANTRO_NOK);
            if (pStorage->dpb->lmRows)
                currImage->ipWater = (i32)((currMbAddr / w) * 16 + 16) -
                    (i32)pStorage->dpb->lmRows;
        }
        else
            pMb = pStorage->mb + currMbAddr;
#endif

        /* primary picture and already decoded macroblock -> error */
        if (!pSliceHeader->redundantPicCnt && CURR_MB->decoded)
        {
            EPRINT("Primary and already decoded");
            return(HANTRO_NOK);
        }

        SetMbParams(CURR_MB, pSliceHeader,
            pStorage->slice->sliceId, pStorage->activePps->chromaQpIndexOffset);

        if (!IS_I_SLICE(pSliceHeader->sliceType))
        {
            if (!prevSkipped)
            {
                tmp = h264bsdExpGolombU(pStrmData, &skipRun);
                if (tmp != HANTRO_OK)
                    return(tmp);
                /* skip_run shall be less than or equal to number of
                 * macroblocks left */
                if (skipRun > (pStorage->picSizeInMbs - currMbAddr))
                {
                    EPRINT("skip_run");
                    return(HANTRO_NOK);
                }
                if (skipRun)
                {
                    prevSkipped = HANTRO_TRUE;
                    memset(&mbLayer->mbPred, 0, sizeof(mbPred_t));
                    /* mark current macroblock skipped */
                    mbLayer->mbType = P_Skip;
                }
            }
        }

        if (skipRun)
        {
            DEBUG(("Skipping macroblock %d\n", currMbAddr));
            skipRun--;
        }
        else
        {
            prevSkipped = HANTRO_FALSE;
            tmp = h264bsdDecodeMacroblockLayer(pStrmData, mbLayer,
                CURR_MB, pSliceHeader->sliceType,
                pSliceHeader->numRefIdxL0Active);
            if (tmp != HANTRO_OK)
            {
                EPRINT("macroblock_layer");
                return(tmp);
            }
        }

        tmp = h264bsdDecodeMacroblock(CURR_MB, mbLayer,
            currImage, pStorage->dpb, &qpY, currMbAddr,
            pStorage->activePps->constrainedIntraPredFlag, data);
        if (tmp != HANTRO_OK)
        {
            EPRINT("MACRO_BLOCK");
            return(tmp);
        }
#if H264BSD_CHANGED_MAP
        if (pStorage->chmOk && currMbAddr < H264BSD_CHANGED_MAP_MBS)
            ChmMb(pStorage, CURR_MB, currMbAddr);
#endif

        /* increment macroblock count only for macroblocks that were decoded
         * for the first time (redundant slices) */
        if (CURR_MB->decoded == 1)
        {
            mbCount++;
            /* OPT: deblock macroblock (r-1, c-1) right after (r, c) is decoded
             * (the last unfiltered uses of it are as top-right / top / top-left
             * intra neighbours). Same raster filtering order as the
             * whole-picture pass -> identical output, but the pixels are still
             * in the data cache. Disabled for the rest of the picture on any
             * out-of-order macroblock (ASO / errors). */
            if (!pStorage->dbBad)
            {
                if (currMbAddr != pStorage->dbNextMb)
                    pStorage->dbBad = 1;
                else
                {
                    u32 w = currImage->width;
                    u32 c = currMbAddr % w;
                    i32 lim = (i32)currMbAddr - (i32)w - (c == w - 1 ? 0 : 1);
                    pStorage->dbNextMb++;
                    if (lim >= (i32)pStorage->dbRowsDone)
                    {
                        h264bsdFilterMbRange(currImage, MB_BASE,
                            pStorage->dbRowsDone, (u32)lim + 1);
                        pStorage->dbRowsDone = (u32)lim + 1;
                        /* OPT: macroblock row r is final once row r + 1 has
                         * been filtered (its top edge reaches 3 rows up) */
#if H264BSD_STRIP
                        if (pStorage->progressFn && !pStorage->stripMode)
#else
                        if (pStorage->progressFn)
#endif
                        {
                            u32 full = pStorage->dbRowsDone / w;
                            u32 rows = full >= 2 ? (full - 1) * 16 : 0;
                            if (rows > pStorage->progressRows)
                            {
                                pStorage->progressRows = rows;
                                pStorage->progressFn(pStorage->progressCtx,
                                    currImage->data, w, currImage->height, rows);
                            }
                        }
                    }
                }
            }
        }

        /* keep on processing as long as there is stream data left or
         * processing of macroblocks to be skipped based on the last skipRun is
         * not finished */
        moreMbs = (h264bsdMoreRbspData(pStrmData) || skipRun) ?
                                        HANTRO_TRUE : HANTRO_FALSE;

        /* lastMbAddr is only updated for intra slices (all macroblocks of
         * inter slices will be lost in case of an error) */
        if (IS_I_SLICE(pSliceHeader->sliceType))
            pStorage->slice->lastMbAddr = currMbAddr;

        currMbAddr = h264bsdNextMbAddress(pStorage->sliceGroupMap,
            pStorage->picSizeInMbs, currMbAddr);
        /* data left in the buffer but no more macroblocks for current slice
         * group -> error */
        if (moreMbs && !currMbAddr)
        {
            EPRINT("Next mb address");
            return(HANTRO_NOK);
        }

    } while (moreMbs);

#if H264BSD_LOWMEM
    /* OPT: low-memory mode: the last macroblocks' checks */
    if (pStorage->lmRingRows && (pStorage->dbBad || pStorage->lmIpBad))
        return(HANTRO_NOK);
#endif
#if H264BSD_STRIP
    /* OPT: strip mode: macroblocks out of order can't be filtered in time */
    if (pStorage->stripMode && pStorage->dbBad)
        return(HANTRO_NOK);
#endif

    if ((pStorage->slice->numDecodedMbs + mbCount) > pStorage->picSizeInMbs)
    {
        EPRINT("Num decoded mbs");
        return(HANTRO_NOK);
    }

    pStorage->slice->numDecodedMbs += mbCount;

    return(HANTRO_OK);

}
#undef CURR_MB
#undef MB_BASE

/*------------------------------------------------------------------------------

   5.2  Function: SetMbParams

        Functional description:
            Set macroblock parameters that remain constant for this slice

        Inputs:
            pSlice      pointer to current slice header
            sliceId     id of the current slice
            chromaQpIndexOffset

        Outputs:
            pMb         pointer to macroblock structure which is updated

        Returns:
            none

------------------------------------------------------------------------------*/

H264BSD_HOT void SetMbParams(mbStorage_t *pMb, sliceHeader_t *pSlice, u32 sliceId,
    i32 chromaQpIndexOffset)
{

/* Variables */
    u32 tmp1;
    i32 tmp2, tmp3;

/* Code */

    tmp1 = pSlice->disableDeblockingFilterIdc;
    tmp2 = pSlice->sliceAlphaC0Offset;
    tmp3 = pSlice->sliceBetaOffset;
    pMb->sliceId = sliceId;
    pMb->disableDeblockingFilterIdc = tmp1;
    pMb->filterOffsetA = tmp2;
    pMb->filterOffsetB = tmp3;
    pMb->chromaQpIndexOffset = chromaQpIndexOffset;

}

/*------------------------------------------------------------------------------

   5.3  Function name: h264bsdMarkSliceCorrupted

        Functional description:
            Mark macroblocks of the slice corrupted. If lastMbAddr in the slice
            storage is set -> picWidhtInMbs (or at least 10) macroblocks back
            from  the lastMbAddr are marked corrupted. However, if lastMbAddr
            is not set -> all macroblocks of the slice are marked.

        Inputs:
            pStorage        pointer to storage structure
            firstMbInSlice  address of the first macroblock in the slice, this
                            identifies the slice to be marked corrupted

        Outputs:
            pStorage        mbStorage for the corrupted macroblocks updated

        Returns:
            none

------------------------------------------------------------------------------*/

H264BSD_COLD_FN void h264bsdMarkSliceCorrupted(storage_t *pStorage, u32 firstMbInSlice)
{

/* Variables */

    u32 tmp, i;
    u32 sliceId;
    u32 currMbAddr;

/* Code */

    ASSERT(pStorage);
    ASSERT(firstMbInSlice < pStorage->picSizeInMbs);

    currMbAddr = firstMbInSlice;

    sliceId = pStorage->slice->sliceId;

    /* DecodeSliceData sets lastMbAddr for I slices -> if it was set, go back
     * MAX(picWidthInMbs, 10) macroblocks and start marking from there */
    if (pStorage->slice->lastMbAddr)
    {
        ASSERT(pStorage->mb[pStorage->slice->lastMbAddr].sliceId == sliceId);
        i = pStorage->slice->lastMbAddr - 1;
        tmp = 0;
        while (i > currMbAddr)
        {
            if (pStorage->mb[i].sliceId == sliceId)
            {
                tmp++;
                if (tmp >= MAX(pStorage->activeSps->picWidthInMbs, 10))
                    break;
            }
            i--;
        }
        currMbAddr = i;
    }

    do
    {

        if ( (pStorage->mb[currMbAddr].sliceId == sliceId) &&
             (pStorage->mb[currMbAddr].decoded) )
        {
            pStorage->mb[currMbAddr].decoded--;
        }
        else
        {
            break;
        }

        currMbAddr = h264bsdNextMbAddress(pStorage->sliceGroupMap,
            pStorage->picSizeInMbs, currMbAddr);

    } while (currMbAddr);

}

