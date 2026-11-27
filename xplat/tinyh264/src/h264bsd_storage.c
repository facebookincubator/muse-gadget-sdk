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
          h264bsdInitStorage
          h264bsdStoreSeqParamSet
          h264bsdStorePicParamSet
          h264bsdActivateParamSets
          h264bsdResetStorage
          h264bsdIsStartOfPicture
          h264bsdIsEndOfPicture
          h264bsdComputeSliceGroupMap
          h264bsdCheckAccessUnitBoundary
          CheckPps
          h264bsdValidParamSets

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_storage.h"
#include "h264bsd_util.h"
#include "h264bsd_neighbour.h"
#include "h264bsd_slice_group_map.h"
#include "h264bsd_dpb.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_seq_param_set.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

static u32 CheckPps(picParamSet_t *pps, seqParamSet_t *sps);

/*------------------------------------------------------------------------------

    Function name: h264bsdInitStorage

        Functional description:
            Initialize storage structure. Sets contents of the storage to '0'
            except for the active parameter set ids, which are initialized
            to invalid values.

        Inputs:

        Outputs:
            pStorage    initialized data stored here

        Returns:
            none

------------------------------------------------------------------------------*/

void h264bsdInitStorage(storage_t *pStorage)
{

/* Variables */

/* Code */

    ASSERT(pStorage);

    memset(pStorage, 0, sizeof(storage_t));

    pStorage->activeSpsId = MAX_NUM_SEQ_PARAM_SETS;
    pStorage->activePpsId = MAX_NUM_PIC_PARAM_SETS;

    pStorage->aub->firstCallFlag = HANTRO_TRUE;
}

/*------------------------------------------------------------------------------

    Function: h264bsdStoreSeqParamSet

        Functional description:
            Store sequence parameter set into the storage. If active SPS is
            overwritten -> check if contents changes and if it does, set
            parameters to force reactivation of parameter sets

        Inputs:
            pStorage        pointer to storage structure
            pSeqParamSet    pointer to param set to be stored

        Outputs:
            none

        Returns:
            HANTRO_OK                success
            MEMORY_ALLOCATION_ERROR  failure in memory allocation


------------------------------------------------------------------------------*/

/* OPT: once the stream has shown a picture (h264bsdKeepParamSets), a set of
 * another size or profile than its own, or another one with the kept one's
 * id outside a keyframe's access unit: damaged or stray bytes, never let
 * them replace the stream's parameter sets */
static u32 KeptSetsRefuse(const storage_t *pStorage, seqParamSet_t *sps)
{
    return (pStorage->lockWidthMbs &&
            (sps->picWidthInMbs != pStorage->lockWidthMbs ||
             sps->picHeightInMbs != pStorage->lockHeightMbs ||
             sps->profileIdc != pStorage->lockProfileIdc)) ||
           (pStorage->lockWidthMbs && pStorage->goodSps &&
            !pStorage->ppsChangeAllowed &&
            sps->seqParameterSetId == pStorage->goodSps->seqParameterSetId &&
            h264bsdCompareSeqParamSets(sps, pStorage->goodSps) != 0);
}

#if H264BSD_LIMITS
/* OPT: the DPB won't reorder output for this sequence parameter set (as
 * h264bsdActivateParamSets decides): it then holds the reference frames
 * alone */
static u32 NoReordering(const storage_t *pStorage, const seqParamSet_t *sps)
{
    return pStorage->noReordering ||
           sps->picOrderCntType == 2 ||
           (sps->vuiParametersPresentFlag &&
            sps->vuiParameters->bitstreamRestrictionFlag &&
            !sps->vuiParameters->numReorderFrames);
}
#endif

u32 h264bsdStoreSeqParamSet(storage_t *pStorage, seqParamSet_t *pSeqParamSet)
{

/* Variables */

    u32 id;

/* Code */

    ASSERT(pStorage);
    ASSERT(pSeqParamSet);
    ASSERT(pSeqParamSet->seqParameterSetId < MAX_NUM_SEQ_PARAM_SETS);

    id = pSeqParamSet->seqParameterSetId;

#if H264BSD_LIMITS
    /* OPT: h264bsdSetLimits (its reference frames, and the frames the DPB
     * holds for output reordering; without it, the reference frames alone).
     * The stream now means a sequence past the limits by this id: the one
     * stored with it is dropped too (deactivated if active), so no slice
     * decodes with it as if it still held, and the caller gets an error.
     * One the kept sets refuse anyway is only refused, as any other. */
    if ( !KeptSetsRefuse(pStorage, pSeqParamSet) &&
         ((pStorage->maxMbs &&
           pSeqParamSet->picWidthInMbs * pSeqParamSet->picHeightInMbs >
               pStorage->maxMbs) ||
          (pStorage->maxDpbFrames &&
           pSeqParamSet->numRefFrames > pStorage->maxDpbFrames) ||
          (pStorage->maxDpbFrames && !NoReordering(pStorage, pSeqParamSet) &&
           pSeqParamSet->maxDpbSize > pStorage->maxDpbFrames)) )
    {
        FREE(pSeqParamSet->offsetForRefFrame);
        FREE(pSeqParamSet->vuiParameters);
        if (pStorage->sps[id])
        {
            if (id == pStorage->activeSpsId)
            {
                pStorage->activeSpsId = MAX_NUM_SEQ_PARAM_SETS + 1;
                pStorage->activePpsId = MAX_NUM_PIC_PARAM_SETS + 1;
                pStorage->activeSps = NULL;
                pStorage->activePps = NULL;
            }
            FREE(pStorage->sps[id]->offsetForRefFrame);
            FREE(pStorage->sps[id]->vuiParameters);
            FREE(pStorage->sps[id]);
        }
        return(H264BSD_SPS_PAST_LIMITS);
    }
#endif

    /* OPT: a picture larger than the caller allows (h264bsdSetMaxSize), or
     * with more reference frames (h264bsdSetMaxReferences), or
     * one the kept sets refuse (KeptSetsRefuse): damaged or stray bytes,
     * never let them replace the stream's parameter sets. The picture after
     * frame cropping (crop units of 2 x 2: 4:2:0 frames only, as the parse
     * takes; the parse checked the offsets leave one) is held to
     * h264bsdSetMaxCropped's size, the coded one to h264bsdSetMaxSize's */
    if ( (pStorage->maxWidthMbs &&
          pSeqParamSet->picWidthInMbs > pStorage->maxWidthMbs) ||
         (pStorage->maxHeightMbs &&
          pSeqParamSet->picHeightInMbs > pStorage->maxHeightMbs) ||
         (pStorage->maxRefFrames &&
          pSeqParamSet->numRefFrames > pStorage->maxRefFrames) ||
         (pStorage->maxCroppedWidth &&
          16 * (uint64_t)pSeqParamSet->picWidthInMbs -
          (pSeqParamSet->frameCroppingFlag ?
           2 * ((uint64_t)pSeqParamSet->frameCropLeftOffset +
                pSeqParamSet->frameCropRightOffset) : 0) >
          pStorage->maxCroppedWidth) ||
         (pStorage->maxCroppedHeight &&
          16 * (uint64_t)pSeqParamSet->picHeightInMbs -
          (pSeqParamSet->frameCroppingFlag ?
           2 * ((uint64_t)pSeqParamSet->frameCropTopOffset +
                pSeqParamSet->frameCropBottomOffset) : 0) >
          pStorage->maxCroppedHeight) ||
         KeptSetsRefuse(pStorage, pSeqParamSet) )
    {
        FREE(pSeqParamSet->offsetForRefFrame);
        FREE(pSeqParamSet->vuiParameters);
        return(HANTRO_NOK);
    }

    /* seq parameter set with id not used before -> allocate memory */
    if (pStorage->sps[id] == NULL)
    {
        ALLOCATE(pStorage->sps[id], 1, seqParamSet_t);
        if (pStorage->sps[id] == NULL)
            return(MEMORY_ALLOCATION_ERROR);
    }
    /* sequence parameter set with id equal to id of active sps */
    else if (id == pStorage->activeSpsId)
    {
        /* if seq parameter set contents changes
         *    -> overwrite and re-activate when next IDR picture decoded
         *    ids of active param sets set to invalid values to force
         *    re-activation. Memories allocated for old sps freed
         * otherwise free memeries allocated for just decoded sps and
         * continue */
        if (h264bsdCompareSeqParamSets(pSeqParamSet, pStorage->activeSps) != 0)
        {
            FREE(pStorage->sps[id]->offsetForRefFrame);
            FREE(pStorage->sps[id]->vuiParameters);
            pStorage->activeSpsId = MAX_NUM_SEQ_PARAM_SETS + 1;
            pStorage->activePpsId = MAX_NUM_PIC_PARAM_SETS + 1;
            pStorage->activeSps = NULL;
            pStorage->activePps = NULL;
        }
        else
        {
            FREE(pSeqParamSet->offsetForRefFrame);
            FREE(pSeqParamSet->vuiParameters);
            return(HANTRO_OK);
        }
    }
    /* overwrite seq param set other than active one -> free memories
     * allocated for old param set */
    else
    {
        FREE(pStorage->sps[id]->offsetForRefFrame);
        FREE(pStorage->sps[id]->vuiParameters);
    }

    *pStorage->sps[id] = *pSeqParamSet;

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdStorePicParamSet

        Functional description:
            Store picture parameter set into the storage. If active PPS is
            overwritten -> check if active SPS changes and if it does -> set
            parameters to force reactivation of parameter sets

        Inputs:
            pStorage        pointer to storage structure
            pPicParamSet    pointer to param set to be stored

        Outputs:
            none

        Returns:
            HANTRO_OK                success
            MEMORY_ALLOCATION_ERROR  failure in memory allocation

------------------------------------------------------------------------------*/

/* OPT: the same picture parameter set (one slice group: no arrays to compare) */
static u32 SamePicParamSet(const picParamSet_t *a, const picParamSet_t *b)
{
    return a->numSliceGroups == 1 && b->numSliceGroups == 1 &&
        a->picParameterSetId == b->picParameterSetId &&
        a->seqParameterSetId == b->seqParameterSetId &&
        a->picOrderPresentFlag == b->picOrderPresentFlag &&
        a->numRefIdxL0Active == b->numRefIdxL0Active &&
        a->picInitQp == b->picInitQp &&
        a->chromaQpIndexOffset == b->chromaQpIndexOffset &&
        a->deblockingFilterControlPresentFlag ==
            b->deblockingFilterControlPresentFlag &&
        a->constrainedIntraPredFlag == b->constrainedIntraPredFlag &&
        a->redundantPicCntPresentFlag == b->redundantPicCntPresentFlag;
}

u32 h264bsdStorePicParamSet(storage_t *pStorage, picParamSet_t *pPicParamSet)
{

/* Variables */

    u32 id;

/* Code */

    ASSERT(pStorage);
    ASSERT(pPicParamSet);
    ASSERT(pPicParamSet->picParameterSetId < MAX_NUM_PIC_PARAM_SETS);
    ASSERT(pPicParamSet->seqParameterSetId < MAX_NUM_SEQ_PARAM_SETS);

    id = pPicParamSet->picParameterSetId;

    /* OPT: once the stream has shown a picture, its kept picture parameter
     * set is replaced only from a keyframe's access unit: damaged or stray
     * bytes that parse as one never change how its frames decode */
    if (pStorage->lockWidthMbs && pStorage->goodPps &&
        id == pStorage->goodPps->picParameterSetId &&
        !pStorage->ppsChangeAllowed &&
        !SamePicParamSet(pPicParamSet, pStorage->goodPps))
    {
        FREE(pPicParamSet->runLength);
        FREE(pPicParamSet->topLeft);
        FREE(pPicParamSet->bottomRight);
        FREE(pPicParamSet->sliceGroupId);
        return(HANTRO_NOK);
    }

    /* pic parameter set with id not used before -> allocate memory */
    if (pStorage->pps[id] == NULL)
    {
        ALLOCATE(pStorage->pps[id], 1, picParamSet_t);
        if (pStorage->pps[id] == NULL)
            return(MEMORY_ALLOCATION_ERROR);
    }
    /* picture parameter set with id equal to id of active pps */
    else if (id == pStorage->activePpsId)
    {
        /* check whether seq param set changes, force re-activation of
         * param set if it does. Set activeSpsId to invalid value to
         * accomplish this */
        if (pPicParamSet->seqParameterSetId != pStorage->activeSpsId)
        {
            pStorage->activePpsId = MAX_NUM_PIC_PARAM_SETS + 1;
        }
        /* free memories allocated for old param set */
        FREE(pStorage->pps[id]->runLength);
        FREE(pStorage->pps[id]->topLeft);
        FREE(pStorage->pps[id]->bottomRight);
        FREE(pStorage->pps[id]->sliceGroupId);
    }
    /* overwrite pic param set other than active one -> free memories
     * allocated for old param set */
    else
    {
        FREE(pStorage->pps[id]->runLength);
        FREE(pStorage->pps[id]->topLeft);
        FREE(pStorage->pps[id]->bottomRight);
        FREE(pStorage->pps[id]->sliceGroupId);
    }

    *pStorage->pps[id] = *pPicParamSet;

    return(HANTRO_OK);

}

#if H264BSD_BUFFER_GATE
/* OPT: before the DPB's pictures are freed (another sequence parameter set
 * activated, h264bsdShutdown) the buffer gate lets each one go, so a display
 * still reading one in place is done with it first. In place the shared
 * buffer is named by its start, as when a picture is decoded into it. */
void h264bsdGateDpb(storage_t *pStorage)
{
    dpbStorage_t *dpb = pStorage->dpb;
    u32 i;

    if (!pStorage->bufferGateFn || !dpb->buffer)
        return;
#if H264BSD_LOWMEM
    if (dpb->lmRows)
    {
        /* NULL when its allocation failed: nothing to let go */
        if (dpb->lmY)
            pStorage->bufferGateFn(pStorage->bufferGateCtx, dpb->lmY);
        return;
    }
#endif
    for (i = 0; i < dpb->dpbSize + 1; i++)
        if (dpb->buffer[i].data)
            pStorage->bufferGateFn(pStorage->bufferGateCtx, dpb->buffer[i].data);
}
#endif

/*------------------------------------------------------------------------------

    Function: h264bsdActivateParamSets

        Functional description:
            Activate certain SPS/PPS combination. This function shall be
            called in the beginning of each picture. Picture parameter set
            can be changed as wanted, but sequence parameter set may only be
            changed when the starting picture is an IDR picture.

            When new SPS is activated the function allocates memory for
            macroblock storages and slice group map and (re-)initializes the
            decoded picture buffer. If this is not the first activation the old
            allocations are freed and FreeDpb called before new allocations.

        Inputs:
            pStorage        pointer to storage data structure
            ppsId           identifies the PPS to be activated, SPS id obtained
                            from the PPS
            isIdr           flag to indicate if the picture is an IDR picture

        Outputs:
            none

        Returns:
            HANTRO_OK       success
            HANTRO_NOK      non-existing or invalid param set combination,
                            trying to change SPS with non-IDR picture
            MEMORY_ALLOCATION_ERROR     failure in memory allocation

------------------------------------------------------------------------------*/

u32 h264bsdActivateParamSets(storage_t *pStorage, u32 ppsId, u32 isIdr)
{

/* Variables */

    u32 tmp;
    u32 flag;

/* Code */

    ASSERT(pStorage);
    ASSERT(ppsId < MAX_NUM_PIC_PARAM_SETS);

    /* check that pps and corresponding sps exist */
    if ( (pStorage->pps[ppsId] == NULL) ||
         (pStorage->sps[pStorage->pps[ppsId]->seqParameterSetId] == NULL) )
    {
        return(HANTRO_NOK);
    }

    /* check that pps parameters do not violate picture size constraints */
    tmp = CheckPps(pStorage->pps[ppsId],
                   pStorage->sps[pStorage->pps[ppsId]->seqParameterSetId]);
    if (tmp != HANTRO_OK)
        return(tmp);

    /* OPT: the stream's format, once locked (h264bsdKeepParamSets), holds at
     * activation too: a sequence parameter set stored before the lock, of
     * another size or profile, is never activated afterwards. */
    {
        const seqParamSet_t *sps =
            pStorage->sps[pStorage->pps[ppsId]->seqParameterSetId];
        if (pStorage->lockWidthMbs &&
            (sps->picWidthInMbs != pStorage->lockWidthMbs ||
             sps->picHeightInMbs != pStorage->lockHeightMbs ||
             sps->profileIdc != pStorage->lockProfileIdc))
            return(HANTRO_NOK);
    }

    /* first activation part1 */
    if (pStorage->activePpsId == MAX_NUM_PIC_PARAM_SETS)
    {
        pStorage->activePpsId = ppsId;
        pStorage->activePps = pStorage->pps[ppsId];
        pStorage->activeSpsId = pStorage->activePps->seqParameterSetId;
        pStorage->activeSps = pStorage->sps[pStorage->activeSpsId];
        pStorage->picSizeInMbs =
            pStorage->activeSps->picWidthInMbs *
            pStorage->activeSps->picHeightInMbs;

        pStorage->currImage->width = pStorage->activeSps->picWidthInMbs;
        pStorage->currImage->height = pStorage->activeSps->picHeightInMbs;

        pStorage->pendingActivation = HANTRO_TRUE;
    }
    /* first activation part2 */
    else if (pStorage->pendingActivation)
    {
        pStorage->pendingActivation = HANTRO_FALSE;
#if H264BSD_BUFFER_GATE
        h264bsdGateDpb(pStorage);   /* the old pictures are freed below */
#endif

#if H264BSD_LOWMEM
        if (pStorage->lmRingRows)
            h264bsdLowMemFreeMb(pStorage);
        else
#endif
#if H264BSD_STRIP
        if (pStorage->stripMode)
            h264bsdStripFree(pStorage);
        else
#endif
        FREE(pStorage->mb);
        FREE(pStorage->sliceGroupMap);

#if H264BSD_STRIP
        /* OPT: strip mode: a ring of macroblock rows, pixels and entries
         * (h264bsd_strip.c) */
        pStorage->dpb->stripMode = pStorage->stripMode;
        if (pStorage->stripMode)
        {
            pStorage->sliceGroupMap = (u32 *)h264bsdMallocHot(pStorage->picSizeInMbs * sizeof(u32));
            if (pStorage->sliceGroupMap == NULL ||
                h264bsdStripAlloc(pStorage) != HANTRO_OK)
                return(MEMORY_ALLOCATION_ERROR);
        }
        else
#endif
#if H264BSD_LOWMEM
        /* OPT: low-memory mode: a ring of macroblock rows */
        if (pStorage->lmFlags & H264BSD_LOWMEM_MBRING)
        {
            pStorage->sliceGroupMap = (u32 *)h264bsdMallocHot(pStorage->picSizeInMbs * sizeof(u32));
            if (pStorage->sliceGroupMap == NULL ||
                h264bsdLowMemAllocMb(pStorage) != HANTRO_OK)
                return(MEMORY_ALLOCATION_ERROR);
        }
        else
#endif
#if H264BSD_LOWMEM || H264BSD_STRIP
        {
#endif
        ALLOCATE(pStorage->mb, pStorage->picSizeInMbs, mbStorage_t);
        pStorage->sliceGroupMap = (u32 *)h264bsdMallocHot(pStorage->picSizeInMbs * sizeof(u32));
        if (pStorage->mb == NULL || pStorage->sliceGroupMap == NULL)
            return(MEMORY_ALLOCATION_ERROR);

        memset(pStorage->mb, 0,
            pStorage->picSizeInMbs * sizeof(mbStorage_t));

        h264bsdInitMbNeighbours(pStorage->mb,
            pStorage->activeSps->picWidthInMbs,
            pStorage->picSizeInMbs);
#if H264BSD_LOWMEM || H264BSD_STRIP
        }
#endif

        /* dpb output reordering disabled if
         * 1) application set noReordering flag
         * 2) POC type equal to 2
         * 3) num_reorder_frames in vui equal to 0 */
        if ( pStorage->noReordering ||
             pStorage->activeSps->picOrderCntType == 2 ||
             (pStorage->activeSps->vuiParametersPresentFlag &&
              pStorage->activeSps->vuiParameters->bitstreamRestrictionFlag &&
              !pStorage->activeSps->vuiParameters->numReorderFrames) )
            flag = HANTRO_TRUE;
        else
            flag = HANTRO_FALSE;

#if H264BSD_LOWMEM
        /* OPT: low-memory mode: in-place pictures when the DPB will hold
         * one reference (and nothing waits for reordering) */
        pStorage->dpb->lmRows = 0;
        pStorage->dpb->lmWidthMbs = pStorage->activeSps->picWidthInMbs;
#if H264BSD_ONEREF
        {
        /* OPT: an SPS allowing several references decodes in place too,
         * keeping one; a slice that needs more is refused
         * (h264bsd_decoder.c) */
        u32 refs = pStorage->activeSps->numRefFrames;
        u32 dpbSize = pStorage->activeSps->maxDpbSize;
        pStorage->lmOneRef = (pStorage->lmFlags & H264BSD_LOWMEM_INPLACE) &&
            flag && refs > 1;
        if (pStorage->lmOneRef)
            refs = dpbSize = 1;
        if ((pStorage->lmFlags & H264BSD_LOWMEM_INPLACE) && flag && refs <= 1)
            pStorage->dpb->lmRows = h264bsdLowMemRows(pStorage);
        tmp = h264bsdResetDpb(pStorage->dpb,
            pStorage->activeSps->picWidthInMbs *
            pStorage->activeSps->picHeightInMbs,
            dpbSize,
            refs,
            pStorage->activeSps->maxFrameNum,
            flag);
        }
#else
        if ((pStorage->lmFlags & H264BSD_LOWMEM_INPLACE) && flag &&
            pStorage->activeSps->numRefFrames <= 1)
            pStorage->dpb->lmRows = h264bsdLowMemRows(pStorage);
#endif
#endif
#if !H264BSD_ONEREF
        tmp = h264bsdResetDpb(pStorage->dpb,
            pStorage->activeSps->picWidthInMbs *
            pStorage->activeSps->picHeightInMbs,
            pStorage->activeSps->maxDpbSize,
            pStorage->activeSps->numRefFrames,
            pStorage->activeSps->maxFrameNum,
            flag);
#endif
        if (tmp != HANTRO_OK)
            return(tmp);
#if H264BSD_LOWMEM
        h264bsdLowMemLayout(pStorage);
#endif
    }
    else if (ppsId != pStorage->activePpsId)
    {
        /* sequence parameter set shall not change but before an IDR picture */
        if (pStorage->pps[ppsId]->seqParameterSetId != pStorage->activeSpsId)
        {
            DEBUG(("SEQ PARAM SET CHANGING...\n"));
            if (isIdr)
            {
                pStorage->activePpsId = ppsId;
                pStorage->activePps = pStorage->pps[ppsId];
                pStorage->activeSpsId = pStorage->activePps->seqParameterSetId;
                pStorage->activeSps = pStorage->sps[pStorage->activeSpsId];
                pStorage->picSizeInMbs =
                    pStorage->activeSps->picWidthInMbs *
                    pStorage->activeSps->picHeightInMbs;

                pStorage->currImage->width = pStorage->activeSps->picWidthInMbs;
                pStorage->currImage->height =
                    pStorage->activeSps->picHeightInMbs;

                pStorage->pendingActivation = HANTRO_TRUE;
            }
            else
            {
                DEBUG(("TRYING TO CHANGE SPS IN NON-IDR SLICE\n"));
                return(HANTRO_NOK);
            }
        }
        else
        {
            pStorage->activePpsId = ppsId;
            pStorage->activePps = pStorage->pps[ppsId];
        }
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdResetStorage

        Functional description:
            Reset contents of the storage. This should be called before
            processing of new image is started.

        Inputs:
            pStorage    pointer to storage structure

        Outputs:
            none

        Returns:
            none


------------------------------------------------------------------------------*/

H264BSD_HOT void h264bsdResetStorage(storage_t *pStorage)
{

/* Variables */

    u32 i;

/* Code */

    ASSERT(pStorage);

    pStorage->slice->numDecodedMbs = 0;
    pStorage->slice->sliceId = 0;

#if H264BSD_LOWMEM && H264BSD_STRIP
    /* OPT: in low-memory and strip modes the ring is reset when a picture
     * starts */
    for (i = 0; !pStorage->lmRingRows && !pStorage->stripMode &&
         i < pStorage->picSizeInMbs; i++)
#elif H264BSD_LOWMEM
    /* OPT: in low-memory mode the ring is reset when a picture starts */
    for (i = 0; !pStorage->lmRingRows && i < pStorage->picSizeInMbs; i++)
#elif H264BSD_STRIP
    /* OPT: in strip mode the ring is reset when a picture starts */
    for (i = 0; !pStorage->stripMode && i < pStorage->picSizeInMbs; i++)
#else
    for (i = 0; i < pStorage->picSizeInMbs; i++)
#endif
    {
        pStorage->mb[i].sliceId = 0;
        pStorage->mb[i].decoded = 0;
    }

}

/*------------------------------------------------------------------------------

    Function: h264bsdIsStartOfPicture

        Functional description:
            Determine if the decoder is in the start of a picture. This
            information is needed to decide if h264bsdActivateParamSets and
            h264bsdCheckGapsInFrameNum functions should be called. Function
            considers that new picture is starting if no slice headers
            have been successfully decoded for the current access unit.

        Inputs:
            pStorage    pointer to storage structure

        Outputs:
            none

        Returns:
            HANTRO_TRUE        new picture is starting
            HANTRO_FALSE       not starting

------------------------------------------------------------------------------*/

u32 h264bsdIsStartOfPicture(storage_t *pStorage)
{

/* Variables */


/* Code */

    if (pStorage->validSliceInAccessUnit == HANTRO_FALSE)
        return(HANTRO_TRUE);
    else
        return(HANTRO_FALSE);

}

/*------------------------------------------------------------------------------

    Function: h264bsdIsEndOfPicture

        Functional description:
            Determine if the decoder is in the end of a picture. This
            information is needed to determine when deblocking filtering
            and reference picture marking processes should be performed.

            If the decoder is processing primary slices the return value
            is determined by checking the value of numDecodedMbs in the
            storage. On the other hand, if the decoder is processing
            redundant slices the numDecodedMbs may not contain valid
            informationa and each macroblock has to be checked separately.

        Inputs:
            pStorage    pointer to storage structure

        Outputs:
            none

        Returns:
            HANTRO_TRUE        end of picture
            HANTRO_FALSE       noup

------------------------------------------------------------------------------*/

u32 h264bsdIsEndOfPicture(storage_t *pStorage)
{

/* Variables */

    u32 i, tmp;

/* Code */

    /* primary picture */
    if (!pStorage->sliceHeader[0].redundantPicCnt)
    {
        if (pStorage->slice->numDecodedMbs == pStorage->picSizeInMbs)
            return(HANTRO_TRUE);
    }
    else
    {
#if H264BSD_LOWMEM || H264BSD_STRIP
        /* OPT: low-memory and strip modes refuse redundant slices
         * (h264bsd_decoder.c) */
        if (H264BSD_DROPS(pStorage))
            return(HANTRO_FALSE);
#endif
        for (i = 0, tmp = 0; i < pStorage->picSizeInMbs; i++)
            tmp += pStorage->mb[i].decoded ? 1 : 0;

        if (tmp == pStorage->picSizeInMbs)
            return(HANTRO_TRUE);
    }

    return(HANTRO_FALSE);

}

/*------------------------------------------------------------------------------

    Function: h264bsdComputeSliceGroupMap

        Functional description:
            Compute slice group map. Just call h264bsdDecodeSliceGroupMap with
            appropriate parameters.

        Inputs:
            pStorage                pointer to storage structure
            sliceGroupChangeCycle

        Outputs:
            none

        Returns:
            none

------------------------------------------------------------------------------*/

void h264bsdComputeSliceGroupMap(storage_t *pStorage, u32 sliceGroupChangeCycle)
{

/* Variables */


/* Code */

    h264bsdDecodeSliceGroupMap(pStorage->sliceGroupMap,
                        pStorage->activePps, sliceGroupChangeCycle,
                        pStorage->activeSps->picWidthInMbs,
                        pStorage->activeSps->picHeightInMbs);

}

/*------------------------------------------------------------------------------

    Function: h264bsdCheckAccessUnitBoundary

        Functional description:
            Check if next NAL unit starts a new access unit. Following
            conditions specify start of a new access unit:

                -NAL unit types 6-11, 13-18 (e.g. SPS, PPS)

           following conditions checked only for slice NAL units, values
           compared to ones obtained from previous slice:

                -NAL unit type differs (slice / IDR slice)
                -frame_num differs
                -nal_ref_idc differs and one of the values is 0
                -POC information differs
                -both are IDR slices and idr_pic_id differs

        Inputs:
            strm        pointer to stream data structure
            nuNext      pointer to NAL unit structure
            storage     pointer to storage structure

        Outputs:
            accessUnitBoundaryFlag  the result is stored here, TRUE for
                                    access unit boundary, FALSE otherwise

        Returns:
            HANTRO_OK           success
            HANTRO_NOK          failure, invalid stream data
            PARAM_SET_ERROR     invalid param set usage

------------------------------------------------------------------------------*/

u32 h264bsdCheckAccessUnitBoundary(
  strmData_t *strm,
  nalUnit_t *nuNext,
  storage_t *storage,
  u32 *accessUnitBoundaryFlag)
{

/* Variables */

    u32 tmp, ppsId, frameNum, idrPicId, picOrderCntLsb;
    i32 deltaPicOrderCntBottom, deltaPicOrderCnt[2];
    seqParamSet_t *sps;
    picParamSet_t *pps;

/* Code */

    ASSERT(strm);
    ASSERT(nuNext);
    ASSERT(storage);
    ASSERT(storage->sps);
    ASSERT(storage->pps);

    /* initialize default output to FALSE */
    *accessUnitBoundaryFlag = HANTRO_FALSE;

    if ( ( (nuNext->nalUnitType > 5) && (nuNext->nalUnitType < 12) ) ||
         ( (nuNext->nalUnitType > 12) && (nuNext->nalUnitType <= 18) ) )
    {
        *accessUnitBoundaryFlag = HANTRO_TRUE;
        return(HANTRO_OK);
    }
    else if ( nuNext->nalUnitType != NAL_CODED_SLICE &&
              nuNext->nalUnitType != NAL_CODED_SLICE_IDR )
    {
        return(HANTRO_OK);
    }

    /* check if this is the very first call to this function */
    if (storage->aub->firstCallFlag)
    {
        *accessUnitBoundaryFlag = HANTRO_TRUE;
        storage->aub->firstCallFlag = HANTRO_FALSE;
    }

    /* get picture parameter set id */
    tmp = h264bsdCheckPpsId(strm, &ppsId);
    if (tmp != HANTRO_OK)
        return(tmp);

    /* store sps and pps in separate pointers just to make names shorter */
    pps = storage->pps[ppsId];
    if ( pps == NULL || storage->sps[pps->seqParameterSetId] == NULL  ||
         (storage->activeSpsId != MAX_NUM_SEQ_PARAM_SETS &&
          pps->seqParameterSetId != storage->activeSpsId &&
          nuNext->nalUnitType != NAL_CODED_SLICE_IDR) )
        return(PARAM_SET_ERROR);
    sps = storage->sps[pps->seqParameterSetId];

    if (storage->aub->nuPrev->nalRefIdc != nuNext->nalRefIdc &&
      (storage->aub->nuPrev->nalRefIdc == 0 || nuNext->nalRefIdc == 0))
        *accessUnitBoundaryFlag = HANTRO_TRUE;

    if ((storage->aub->nuPrev->nalUnitType == NAL_CODED_SLICE_IDR &&
          nuNext->nalUnitType != NAL_CODED_SLICE_IDR) ||
      (storage->aub->nuPrev->nalUnitType != NAL_CODED_SLICE_IDR &&
       nuNext->nalUnitType == NAL_CODED_SLICE_IDR))
        *accessUnitBoundaryFlag = HANTRO_TRUE;

    tmp = h264bsdCheckFrameNum(strm, sps->maxFrameNum, &frameNum);
    if (tmp != HANTRO_OK)
        return(HANTRO_NOK);

    if (storage->aub->prevFrameNum != frameNum)
    {
        storage->aub->prevFrameNum = frameNum;
        *accessUnitBoundaryFlag = HANTRO_TRUE;
    }

    if (nuNext->nalUnitType == NAL_CODED_SLICE_IDR)
    {
        tmp = h264bsdCheckIdrPicId(strm, sps->maxFrameNum, nuNext->nalUnitType,
          &idrPicId);
        if (tmp != HANTRO_OK)
            return(HANTRO_NOK);

        if (storage->aub->nuPrev->nalUnitType == NAL_CODED_SLICE_IDR &&
          storage->aub->prevIdrPicId != idrPicId)
            *accessUnitBoundaryFlag = HANTRO_TRUE;

        storage->aub->prevIdrPicId = idrPicId;
    }

    if (sps->picOrderCntType == 0)
    {
        tmp = h264bsdCheckPicOrderCntLsb(strm, sps, nuNext->nalUnitType,
          &picOrderCntLsb);
        if (tmp != HANTRO_OK)
            return(HANTRO_NOK);

        if (storage->aub->prevPicOrderCntLsb != picOrderCntLsb)
        {
            storage->aub->prevPicOrderCntLsb = picOrderCntLsb;
            *accessUnitBoundaryFlag = HANTRO_TRUE;
        }

        if (pps->picOrderPresentFlag)
        {
            tmp = h264bsdCheckDeltaPicOrderCntBottom(strm, sps,
                nuNext->nalUnitType, &deltaPicOrderCntBottom);
            if (tmp != HANTRO_OK)
                return(tmp);

            if (storage->aub->prevDeltaPicOrderCntBottom !=
                deltaPicOrderCntBottom)
            {
                storage->aub->prevDeltaPicOrderCntBottom =
                    deltaPicOrderCntBottom;
                *accessUnitBoundaryFlag = HANTRO_TRUE;
            }
        }
    }
    else if (sps->picOrderCntType == 1 && !sps->deltaPicOrderAlwaysZeroFlag)
    {
        tmp = h264bsdCheckDeltaPicOrderCnt(strm, sps, nuNext->nalUnitType,
          pps->picOrderPresentFlag, deltaPicOrderCnt);
        if (tmp != HANTRO_OK)
            return(tmp);

        if (storage->aub->prevDeltaPicOrderCnt[0] != deltaPicOrderCnt[0])
        {
            storage->aub->prevDeltaPicOrderCnt[0] = deltaPicOrderCnt[0];
            *accessUnitBoundaryFlag = HANTRO_TRUE;
        }

        if (pps->picOrderPresentFlag)
            if (storage->aub->prevDeltaPicOrderCnt[1] != deltaPicOrderCnt[1])
            {
                storage->aub->prevDeltaPicOrderCnt[1] = deltaPicOrderCnt[1];
                *accessUnitBoundaryFlag = HANTRO_TRUE;
            }
    }

    *storage->aub->nuPrev = *nuNext;

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: CheckPps

        Functional description:
            Check picture parameter set. Contents of the picture parameter
            set information that depends on the image dimensions is checked
            against the dimensions in the sps.

        Inputs:
            pps     pointer to picture paramter set
            sps     pointer to sequence parameter set

        Outputs:
            none

        Returns:
            HANTRO_OK      everything ok
            HANTRO_NOK     invalid data in picture parameter set

------------------------------------------------------------------------------*/
u32 CheckPps(picParamSet_t *pps, seqParamSet_t *sps)
{

    u32 i;
    u32 picSize;

    picSize = sps->picWidthInMbs * sps->picHeightInMbs;

    /* check slice group params */
    if (pps->numSliceGroups > 1)
    {
        if (pps->sliceGroupMapType == 0)
        {
            ASSERT(pps->runLength);
            for (i = 0; i < pps->numSliceGroups; i++)
            {
                if (pps->runLength[i] > picSize)
                    return(HANTRO_NOK);
            }
        }
        else if (pps->sliceGroupMapType == 2)
        {
            ASSERT(pps->topLeft);
            ASSERT(pps->bottomRight);
            for (i = 0; i < pps->numSliceGroups-1; i++)
            {
                if (pps->topLeft[i] > pps->bottomRight[i] ||
                    pps->bottomRight[i] >= picSize)
                    return(HANTRO_NOK);

                if ( (pps->topLeft[i] % sps->picWidthInMbs) >
                     (pps->bottomRight[i] % sps->picWidthInMbs) )
                    return(HANTRO_NOK);
            }
        }
        else if (pps->sliceGroupMapType > 2 && pps->sliceGroupMapType < 6)
        {
            if (pps->sliceGroupChangeRate > picSize)
                return(HANTRO_NOK);
        }
        else if (pps->sliceGroupMapType == 6 &&
                 pps->picSizeInMapUnits < picSize)
            return(HANTRO_NOK);
    }

    return(HANTRO_OK);
}

/*------------------------------------------------------------------------------

    Function: h264bsdValidParamSets

        Functional description:
            Check if any valid SPS/PPS combination exists in the storage.
            Function tries each PPS in the buffer and checks if corresponding
            SPS exists and calls CheckPps to determine if the PPS conforms
            to image dimensions of the SPS.

        Inputs:
            pStorage    pointer to storage structure

        Outputs:
            HANTRO_OK   there is at least one valid combination
            HANTRO_NOK  no valid combinations found


------------------------------------------------------------------------------*/

u32 h264bsdValidParamSets(storage_t *pStorage)
{

/* Variables */

    u32 i;

/* Code */

    ASSERT(pStorage);

    for (i = 0; i < MAX_NUM_PIC_PARAM_SETS; i++)
    {
        if ( pStorage->pps[i] &&
             pStorage->sps[pStorage->pps[i]->seqParameterSetId] &&
             CheckPps(pStorage->pps[i],
                      pStorage->sps[pStorage->pps[i]->seqParameterSetId]) ==
                 HANTRO_OK)
        {
            return(HANTRO_OK);
        }
    }

    return(HANTRO_NOK);

}

