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
          h264bsdInit
          h264bsdDecode
          h264bsdShutdown




------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/
#include "h264bsd_decoder.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_byte_stream.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_pic_param_set.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_slice_data.h"
#include "h264bsd_neighbour.h"
#include "h264bsd_util.h"
#include "h264bsd_dpb.h"
#include "h264bsd_deblocking.h"
#include "h264bsd_conceal.h"
#include "h264bsd_storage.h"

/*------------------------------------------------------------------------------
    2. External compiler flags
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------

    Function name: h264bsdInit

        Functional description:
            Initialize the decoder.

        Inputs:
            noOutputReordering  flag to indicate the decoder that it does not
                                have to perform reordering of display images.

        Outputs:
            pStorage            pointer to initialized storage structure

        Returns:
            none

------------------------------------------------------------------------------*/

u32 h264bsdInit(storage_t *pStorage, u32 noOutputReordering)
{

/* Variables */
    u32 size;
/* Code */

    h264bsdInitStorage(pStorage);

    /* allocate mbLayer to be next multiple of 64 to enable use of
     * specific NEON optimized "memset" for clearing the structure */
    size = (sizeof(macroblockLayer_t) + 63) & ~0x3F;

    pStorage->mbLayer = (macroblockLayer_t*)h264bsdMallocHot(size);
    if (!pStorage->mbLayer)
        return HANTRO_NOK;

    if (noOutputReordering)
        pStorage->noReordering = HANTRO_TRUE;

    return HANTRO_OK;
}

/*------------------------------------------------------------------------------

    Function: h264bsdDecodeInternal

        Functional description:
            Decode a NAL unit. This function calls other modules to perform
            tasks like
                * extract and decode NAL unit from the byte stream
                * decode parameter sets
                * decode slice header and slice data
                * conceal errors in the picture
                * perform deblocking filtering

            This function contains top level control logic of the decoder.

        Inputs:
            pStorage        pointer to storage data structure
            byteStrm        pointer to stream buffer given by application
            len             length of the buffer in bytes

        Outputs:
            readBytes       number of bytes read from the stream is stored
                            here

        Returns:
            H264BSD_RDY             decoding finished, nothing special
            H264BSD_PIC_RDY         decoding of a picture finished
            H264BSD_HDRS_RDY        param sets activated, information like
                                    picture dimensions etc can be read
            H264BSD_ERROR           error in decoding
            H264BSD_PARAM_SET_ERROR serious error in decoding, failed to
                                    activate param sets

------------------------------------------------------------------------------*/

#if H264BSD_BUFFER_GATE
/* OPT: the DPB buffer the next picture is decoded into, once the caller's
 * buffer gate (h264bsdSetBufferGate) has let it go: a picture still being
 * read elsewhere is never written. */
static u8 *AllocateImage(storage_t *pStorage)
{
    u8 *data = h264bsdAllocateDpbImage(pStorage->dpb);
    if (pStorage->bufferGateFn)
#if H264BSD_LOWMEM
        /* in place, the new picture always starts at lmY, where the last
         * one is, and moving that one down writes the whole buffer */
        pStorage->bufferGateFn(pStorage->bufferGateCtx,
            pStorage->dpb->lmRows ? pStorage->dpb->lmY : data);
#else
        pStorage->bufferGateFn(pStorage->bufferGateCtx, data);
#endif
    return data;
}
#else
#define AllocateImage(pStorage) h264bsdAllocateDpbImage((pStorage)->dpb)
#endif

#if H264BSD_CHANGED_MAP
/* OPT: h264bsdChangedMap. A picture starts: its map is trusted only if it
 * predicts from the picture last output, which was a reference picture
 * whose frame_num is one less (no gap: no non-existing frame sits ahead of
 * it in the reference list), and the bit map holds it. Only without output
 * reordering: with it, the call that decodes this picture may return
 * another one. */
static void ChmStart(storage_t *s, const nalUnit_t *nal)
{
    memset(s->chmBits, 0, sizeof(s->chmBits));
    s->chmDb = 0;
    s->chmOk = !s->chmSkip && s->chmPrev && !IS_IDR_NAL_UNIT(nal) &&
               s->dpb->noReordering &&
               s->picSizeInMbs <= H264BSD_CHANGED_MAP_MBS && s->dpb->maxFrameNum &&
               s->sliceHeader[1].frameNum == (s->chmFn + 1) % s->dpb->maxFrameNum;
}

/* A slice (its reference list built): reordered references, a reference
 * index 0 that isn't the last output picture or a redundant slice end the
 * map's trust. In place (one buffer) the reference is the last picture,
 * moved down. */
static void ChmSlice(storage_t *s, const sliceHeader_t *sh)
{
    if (sh->redundantPicCnt ||
        (!IS_I_SLICE(sh->sliceType) && sh->refPicListReordering.refPicListReorderingFlagL0))
        s->chmOk = 0;
    if (!IS_I_SLICE(sh->sliceType) &&
#if H264BSD_LOWMEM
        !s->dpb->lmRows &&
#endif
        (!s->dpb->list[0] || s->dpb->list[0]->data != s->chmPrevData))
        s->chmOk = 0;
    if (sh->disableDeblockingFilterIdc != 1)
        s->chmDb = 1;
}

/* The picture is finished: its map, and whether the next picture may trust
 * one */
static void ChmDone(storage_t *s)
{
    const sliceHeader_t *sh = s->sliceHeader;
    const nalUnit_t *nal = s->prevNalUnit;
    u32 whole = s->validSliceInAccessUnit && !s->numConcealedMbs &&
                s->slice->numDecodedMbs == s->picSizeInMbs;
    /* bit 2 (IDR) comes from the picture returned (DecodeAu) */
    s->chmOut = (u8)(whole && s->chmOk ? (s->chmDb ? 3 : 1) : 0);
    s->chmPrev = (u8)(whole && nal->nalRefIdc &&
                      !(IS_IDR_NAL_UNIT(nal) ? sh->decRefPicMarking.longTermReferenceFlag :
                        sh->decRefPicMarking.adaptiveRefPicMarkingModeFlag));
    s->chmFn = sh->frameNum;
}
#endif

u32 h264bsdDecodeInternal(storage_t *pStorage, u8 *byteStrm, u32 len,
    u32 *readBytes)
{

/* Variables */

    u32 tmp, ppsId, spsId;
    i32 picOrderCnt;
    nalUnit_t nalUnit;
    seqParamSet_t seqParamSet;
    picParamSet_t picParamSet;
    strmData_t strm;
    u32 accessUnitBoundaryFlag = HANTRO_FALSE;
    u32 picReady = HANTRO_FALSE;
#if H264BSD_LOWMEM
    u32 lmRecover;
#endif

    /* if previous buffer was not finished and same pointer given -> skip NAL
     * unit extraction */
    /* OPT: and only while that NAL unit fits in this buffer */
    if (pStorage->prevBufNotFinished && byteStrm == pStorage->prevBufPointer &&
        pStorage->prevBytesConsumed <= len)
    {
        strm = pStorage->strm[0];
        strm.pStrmCurrPos = strm.pStrmBuffStart;
        strm.strmBuffReadBits = strm.bitPosInWord = 0;
        *readBytes = pStorage->prevBytesConsumed;
    }
    else
    {
        tmp = h264bsdExtractNalUnit(byteStrm, len, &strm, readBytes);
        if (tmp != HANTRO_OK)
        {
            EPRINT("BYTE_STREAM");
            return(H264BSD_ERROR);
        }
        /* store stream */
        pStorage->strm[0] = strm;
        pStorage->prevBytesConsumed = *readBytes;
        pStorage->prevBufPointer = byteStrm;
    }
    pStorage->prevBufNotFinished = HANTRO_FALSE;

    tmp = h264bsdDecodeNalUnit(&strm, &nalUnit);
    if (tmp != HANTRO_OK)
    {
        EPRINT("NAL_UNIT");
        return(H264BSD_ERROR);
    }

    /* Discard unspecified, reserved, SPS extension and auxiliary picture slices */
    if(nalUnit.nalUnitType == 0 || nalUnit.nalUnitType >= 13)
    {
        DEBUG(("DISCARDED NAL (UNSPECIFIED, REGISTERED, SPS ext or AUX slice)\n"));
        return(H264BSD_RDY);
    }

    tmp = h264bsdCheckAccessUnitBoundary(
      &strm,
      &nalUnit,
      pStorage,
      &accessUnitBoundaryFlag);
    if (tmp != HANTRO_OK)
    {
        EPRINT("ACCESS UNIT BOUNDARY CHECK");
        if (tmp == PARAM_SET_ERROR)
            return(H264BSD_PARAM_SET_ERROR);
        else
            return(H264BSD_ERROR);
    }

    if ( accessUnitBoundaryFlag )
    {
        DEBUG(("Access unit boundary\n"));
#if H264BSD_LOWMEM || H264BSD_STRIP
        /* OPT: low-memory and strip modes: no concealment (it needs the
         * whole picture): drop the unfinished picture */
        if (pStorage->picStarted && pStorage->activeSps != NULL &&
            H264BSD_DROPS(pStorage))
        {
#if H264BSD_LOWMEM
            if (pStorage->lmFlags)
                h264bsdLowMemDrop(pStorage);
#endif
            pStorage->picStarted = HANTRO_FALSE;
            pStorage->validSliceInAccessUnit = HANTRO_FALSE;
            pStorage->slice->numDecodedMbs = 0;
            pStorage->slice->sliceId = 0;
            pStorage->dbNextMb = pStorage->dbRowsDone = pStorage->dbBad = 0;
            pStorage->progressRows = 0;
        }
        else
#endif
        /* conceal if picture started and param sets activated */
        if (pStorage->picStarted && pStorage->activeSps != NULL)
        {
            DEBUG(("CONCEALING..."));

            /* return error if second phase of
             * initialization is not completed */
            if (pStorage->pendingActivation)
            {
                EPRINT("Pending activation not completed");
                return (H264BSD_ERROR);
            }

            if (!pStorage->validSliceInAccessUnit)
            {
#if H264BSD_SPLIT_PLANES
                h264bsdImageSetData(pStorage->currImage,
                    AllocateImage(pStorage));
#else
                pStorage->currImage->data =
                    AllocateImage(pStorage);
#endif
                h264bsdInitRefPicList(pStorage->dpb);
                tmp = h264bsdConceal(pStorage, pStorage->currImage, P_SLICE);
            }
            else
                tmp = h264bsdConceal(pStorage, pStorage->currImage,
                    pStorage->sliceHeader->sliceType);

            picReady = HANTRO_TRUE;

            /* current NAL unit should be decoded on next activation -> set
             * readBytes to 0 */
            *readBytes = 0;
            pStorage->prevBufNotFinished = HANTRO_TRUE;
            DEBUG(("...DONE\n"));
        }
        else
        {
            pStorage->validSliceInAccessUnit = HANTRO_FALSE;
        }
        pStorage->skipRedundantSlices = HANTRO_FALSE;
    }

    if (!picReady)
    {
        switch (nalUnit.nalUnitType)
        {
            case NAL_SEQ_PARAM_SET:
                DEBUG(("SEQ PARAM SET\n"));
                tmp = h264bsdDecodeSeqParamSet(&strm, &seqParamSet);
                if (tmp != HANTRO_OK)
                {
                    EPRINT("SEQ_PARAM_SET");
                    FREE(seqParamSet.offsetForRefFrame);
                    FREE(seqParamSet.vuiParameters);
                    return(H264BSD_ERROR);
                }
                tmp = h264bsdStoreSeqParamSet(pStorage, &seqParamSet);
#if H264BSD_LIMITS
                if (tmp == H264BSD_SPS_PAST_LIMITS)
                {
                    EPRINT("SEQ_PARAM_SET past the limits");
                    return(H264BSD_ERROR);
                }
#endif
                break;

            case NAL_PIC_PARAM_SET:
                DEBUG(("PIC PARAM SET\n"));
                tmp = h264bsdDecodePicParamSet(&strm, &picParamSet);
                if (tmp != HANTRO_OK)
                {
                    EPRINT("PIC_PARAM_SET");
                    FREE(picParamSet.runLength);
                    FREE(picParamSet.topLeft);
                    FREE(picParamSet.bottomRight);
                    FREE(picParamSet.sliceGroupId);
                    return(H264BSD_ERROR);
                }
                tmp = h264bsdStorePicParamSet(pStorage, &picParamSet);
                break;

            case NAL_CODED_SLICE_IDR:
                DEBUG(("IDR "));
                /* fall through */
            case NAL_CODED_SLICE:
                DEBUG(("SLICE HEADER\n"));

                /* picture successfully finished and still decoding same old
                 * access unit -> no need to decode redundant slices */
                if (pStorage->skipRedundantSlices)
                    return(H264BSD_RDY);

                pStorage->picStarted = HANTRO_TRUE;

                if (h264bsdIsStartOfPicture(pStorage))
                {
                    pStorage->numConcealedMbs = 0;
                    pStorage->currentPicId    = 0;

                    tmp = h264bsdCheckPpsId(&strm, &ppsId);
                    ASSERT(tmp == HANTRO_OK);
                    /* store old activeSpsId and return headers ready
                     * indication if activeSps changes */
                    spsId = pStorage->activeSpsId;
                    tmp = h264bsdActivateParamSets(pStorage, ppsId,
                            IS_IDR_NAL_UNIT(&nalUnit) ?
                            HANTRO_TRUE : HANTRO_FALSE);
                    if (tmp != HANTRO_OK)
                    {
                        EPRINT("Param set activation");
                        pStorage->activePpsId = MAX_NUM_PIC_PARAM_SETS;
                        pStorage->activePps = NULL;
                        pStorage->activeSpsId = MAX_NUM_SEQ_PARAM_SETS;
                        pStorage->activeSps = NULL;
                        pStorage->pendingActivation = HANTRO_FALSE;

                        if(tmp == MEMORY_ALLOCATION_ERROR)
                        {
                            return H264BSD_MEMALLOC_ERROR;
                        }
                        else
                            return(H264BSD_PARAM_SET_ERROR);
                    }

                    if (spsId != pStorage->activeSpsId)
                    {
                        seqParamSet_t *oldSPS = NULL;
                        seqParamSet_t *newSPS = pStorage->activeSps;
                        u32 noOutputOfPriorPicsFlag = 1;

                        if(pStorage->oldSpsId < MAX_NUM_SEQ_PARAM_SETS)
                        {
                            oldSPS = pStorage->sps[pStorage->oldSpsId];
                        }

                        *readBytes = 0;
                        pStorage->prevBufNotFinished = HANTRO_TRUE;


                        if(nalUnit.nalUnitType == NAL_CODED_SLICE_IDR)
                        {
                            tmp =
                            h264bsdCheckPriorPicsFlag(&noOutputOfPriorPicsFlag,
                                                          &strm, newSPS,
                                                          pStorage->activePps,
                                                          nalUnit.nalUnitType);
                        }
                        else
                        {
                            tmp = HANTRO_NOK;
                        }

                        if((tmp != HANTRO_OK) ||
                           (noOutputOfPriorPicsFlag != 0) ||
                           (pStorage->dpb->noReordering) ||
                           (oldSPS == NULL) ||
                           (oldSPS->picWidthInMbs != newSPS->picWidthInMbs) ||
                           (oldSPS->picHeightInMbs != newSPS->picHeightInMbs) ||
                           (oldSPS->maxDpbSize != newSPS->maxDpbSize))
                        {
                            pStorage->dpb->flushed = 0;
                        }
                        else
                        {
                            h264bsdFlushDpb(pStorage->dpb);
                        }

                        pStorage->oldSpsId = pStorage->activeSpsId;

                        return(H264BSD_HDRS_RDY);
                    }
                }

                /* return error if second phase of
                 * initialization is not completed */
                if (pStorage->pendingActivation)
                {
                    EPRINT("Pending activation not completed");
                    return (H264BSD_ERROR);
                }
                tmp = h264bsdDecodeSliceHeader(&strm, pStorage->sliceHeader + 1,
                    pStorage->activeSps, pStorage->activePps, &nalUnit);
                if (tmp != HANTRO_OK)
                {
                    EPRINT("SLICE_HEADER");
                    return(H264BSD_ERROR);
                }
#if H264BSD_LOWMEM
                /* OPT: low-memory mode: what it can't decode exactly */
                if (pStorage->lmFlags)
                {
                    sliceHeader_t *sh = pStorage->sliceHeader + 1;
                    if (h264bsdIsStartOfPicture(pStorage))
                    {
                        u32 prev = pStorage->dpb->prevRefFrameNum;
                        /* an IDR picture ends the wait once it has decoded
                         * whole (picReady below), not when it starts: it
                         * may still be refused or fail */
                        u32 needIdr = pStorage->lmNeedIdr && !IS_IDR_NAL_UNIT(&nalUnit);
                        pStorage->lmFail = pStorage->lmSkip = HANTRO_FALSE;
                        pStorage->lmRecover = pStorage->lmNeedIdr && IS_IDR_NAL_UNIT(&nalUnit);
#if H264BSD_ONEREF
                        pStorage->lmNonRef = HANTRO_FALSE;
#endif
                        /* an intra picture can restart too, if all its
                         * slices are intra (checked slice by slice below)
                         * and it is a reference picture (at its end) */
                        if (needIdr && IS_I_SLICE(sh->sliceType))
                        {
                            /* the pictures skipped while waiting never
                             * reached the frame_num gap check: the restart
                             * continues frame_num from its own */
                            pStorage->lmRecover = HANTRO_TRUE;
                            pStorage->dpb->prevRefFrameNum =
                                (sh->frameNum + pStorage->dpb->maxFrameNum - 1) %
                                pStorage->dpb->maxFrameNum;
                        }
                        else if (needIdr)
                            pStorage->lmSkip = HANTRO_TRUE;
                        else if (pStorage->activePps->numSliceGroups > 1 ||
                            (!IS_IDR_NAL_UNIT(&nalUnit) &&
                             pStorage->activeSps->gapsInFrameNumValueAllowedFlag &&
                             sh->frameNum != prev &&
                             sh->frameNum != (prev + 1) % pStorage->dpb->maxFrameNum))
                        {
                            pStorage->lmFail = HANTRO_TRUE;   /* nothing written yet */
                            pStorage->lmRefused++;
                        }
                        else if (LM_OVERWRITES_REF(pStorage) && !nalUnit.nalRefIdc)
                        {
                            pStorage->lmSkip = HANTRO_TRUE;  /* would overwrite the reference */
#if H264BSD_ONEREF
                            pStorage->lmNonRef = HANTRO_TRUE;
                            pStorage->lmNonRefs++;
#endif
                        }
                    }
#if H264BSD_ONEREF
                    /* OPT: one reference kept (an SPS that allows more): a
                     * slice that would use another is refused */
                    if (pStorage->lmOneRef && !pStorage->lmSkip && !pStorage->lmFail &&
                        ((IS_P_SLICE(sh->sliceType) && sh->numRefIdxL0Active > 1) ||
                         sh->refPicListReordering.refPicListReorderingFlagL0 ||
                         (IS_IDR_NAL_UNIT(&nalUnit) ?
                          sh->decRefPicMarking.longTermReferenceFlag :
                          nalUnit.nalRefIdc && sh->decRefPicMarking.adaptiveRefPicMarkingModeFlag)))
                    {
                        pStorage->lmFail = HANTRO_TRUE;
                        pStorage->lmNeedIdr = HANTRO_TRUE;
                        pStorage->lmRefused++;
                    }
#endif
                    if (pStorage->lmRecover && !IS_I_SLICE(sh->sliceType))
                    {
                        /* a P slice in the picture meant to restart */
                        pStorage->lmFail = HANTRO_TRUE;
                        pStorage->lmRefused++;
                    }
                    if (pStorage->lmSkip || pStorage->lmFail || sh->redundantPicCnt)
                        return(H264BSD_ERROR);
                }
#endif
#if H264BSD_STRIP
                /* OPT: strip mode: intra slices only, no redundant ones */
                if (pStorage->stripMode &&
                    (!IS_I_SLICE(pStorage->sliceHeader[1].sliceType) ||
                     pStorage->sliceHeader[1].redundantPicCnt))
                    return(H264BSD_ERROR);
#endif
                if (h264bsdIsStartOfPicture(pStorage))
                {
                    if (!IS_IDR_NAL_UNIT(&nalUnit))
                    {
                        tmp = h264bsdCheckGapsInFrameNum(pStorage->dpb,
                            pStorage->sliceHeader[1].frameNum,
                            nalUnit.nalRefIdc != 0 ?
                            HANTRO_TRUE : HANTRO_FALSE,
                            pStorage->activeSps->
                            gapsInFrameNumValueAllowedFlag);
                        if (tmp != HANTRO_OK)
                        {
                            EPRINT("Gaps in frame num");
                            return(H264BSD_ERROR);
                        }
                    }
#if H264BSD_SPLIT_PLANES
                    h264bsdImageSetData(pStorage->currImage,
                        AllocateImage(pStorage));
#else
                    pStorage->currImage->data =
                        AllocateImage(pStorage);
#endif
#if H264BSD_LOWMEM
                    /* OPT: low-memory mode */
                    if (pStorage->dpb->lmRows)
                        h264bsdLowMemPlace(pStorage);
                    if (pStorage->lmRingRows)
                        h264bsdLowMemBeginPicture(pStorage);
#endif
#if H264BSD_STRIP
                    if (pStorage->stripMode)
                        h264bsdStripBegin(pStorage);
#endif
#if H264BSD_CHANGED_MAP
                    ChmStart(pStorage, &nalUnit);
#endif
                }

                /* store slice header to storage if successfully decoded */
                pStorage->sliceHeader[0] = pStorage->sliceHeader[1];
                pStorage->validSliceInAccessUnit = HANTRO_TRUE;
                pStorage->prevNalUnit[0] = nalUnit;

                h264bsdComputeSliceGroupMap(pStorage,
                    pStorage->sliceHeader->sliceGroupChangeCycle);

                h264bsdInitRefPicList(pStorage->dpb);
                tmp = h264bsdReorderRefPicList(pStorage->dpb,
                    &pStorage->sliceHeader->refPicListReordering,
                    pStorage->sliceHeader->frameNum,
                    pStorage->sliceHeader->numRefIdxL0Active);
                if (tmp != HANTRO_OK)
                {
                    EPRINT("Reordering");
                    return(H264BSD_ERROR);
                }

                DEBUG(("SLICE DATA, FIRST %d\n",
                        pStorage->sliceHeader->firstMbInSlice));
#if H264BSD_CHANGED_MAP
                ChmSlice(pStorage, pStorage->sliceHeader);
#endif
                tmp = h264bsdDecodeSliceData(&strm, pStorage,
                    pStorage->currImage, pStorage->sliceHeader);
                if (tmp != HANTRO_OK)
                {
                    EPRINT("SLICE_DATA");
#if H264BSD_LOWMEM
                    if (pStorage->lmFlags)
                    {
                        /* OPT: low-memory mode: the picture failed */
                        pStorage->lmFail = HANTRO_TRUE;
                        if (LM_OVERWRITES_REF(pStorage))
                            pStorage->lmNeedIdr = HANTRO_TRUE;
                        pStorage->lmRefused++;
                        return(H264BSD_ERROR);
                    }
#endif
#if H264BSD_STRIP
                    if (pStorage->stripMode)
                        return(H264BSD_ERROR);   /* OPT: dropped, never concealed */
#endif
                    h264bsdMarkSliceCorrupted(pStorage,
                        pStorage->sliceHeader->firstMbInSlice);
                    return(H264BSD_ERROR);
                }

                if (h264bsdIsEndOfPicture(pStorage))
                {
                    picReady = HANTRO_TRUE;
                    pStorage->skipRedundantSlices = HANTRO_TRUE;
                }
                break;

            case NAL_SEI:
                DEBUG(("SEI MESSAGE, NOT DECODED"));
                break;

            default:
                DEBUG(("NOT IMPLEMENTED YET %d\n",nalUnit.nalUnitType));
        }
    }

    if (picReady)
    {
#if H264BSD_CHANGED_MAP
        ChmDone(pStorage);  /* before the storage reset clears the decoded count */
#endif
#if H264BSD_LOWMEM
        /* OPT: low-memory mode: a complete intra picture restarts once it
         * is the reference (below: a picture that isn't one, or that
         * reference marking couldn't store, leaves the damaged reference
         * in the DPB) */
        lmRecover = pStorage->lmRecover && pStorage->prevNalUnit->nalRefIdc;
        pStorage->lmRecover = HANTRO_FALSE;
        h264bsdFilterMbRange(pStorage->currImage,
            pStorage->lmRingRows ? pStorage->lmMb : pStorage->mb,
            pStorage->dbRowsDone,
            pStorage->currImage->width * pStorage->currImage->height);
#else
        h264bsdFilterMbRange(pStorage->currImage, pStorage->mb,
            pStorage->dbRowsDone,
            pStorage->currImage->width * pStorage->currImage->height);
#endif
        pStorage->dbNextMb = pStorage->dbRowsDone = pStorage->dbBad = 0;
#if H264BSD_STRIP
        if (pStorage->stripMode)
        {
            /* OPT: strip mode: the rows still held, then the DPB's stand-in
             * picture back (its bookkeeping checks the picture it gave) */
            h264bsdStripFinish(pStorage);
            h264bsdImageSetData(pStorage->currImage,
                pStorage->dpb->currentOut->data);
        }
        else
#endif
        /* OPT: a picture concealed without a single valid slice isn't
         * reported: it isn't stored, and the rest of the call may reuse or
         * free its buffer */
        if (pStorage->progressFn && pStorage->validSliceInAccessUnit)
            pStorage->progressFn(pStorage->progressCtx, pStorage->currImage->data,
                pStorage->currImage->width, pStorage->currImage->height,
                pStorage->currImage->height * 16);
        pStorage->progressRows = 0;

        h264bsdResetStorage(pStorage);

         picOrderCnt = h264bsdDecodePicOrderCnt(pStorage->poc,
            pStorage->activeSps, pStorage->sliceHeader, pStorage->prevNalUnit);

        if (pStorage->validSliceInAccessUnit)
        {
            if (pStorage->prevNalUnit->nalRefIdc)
            {
                tmp = h264bsdMarkDecRefPic(pStorage->dpb,
                    &pStorage->sliceHeader->decRefPicMarking,
                    pStorage->currImage, pStorage->sliceHeader->frameNum,
                    picOrderCnt,
                    IS_IDR_NAL_UNIT(pStorage->prevNalUnit) ?
                    HANTRO_TRUE : HANTRO_FALSE,
                    pStorage->currentPicId, pStorage->numConcealedMbs);
#if H264BSD_LOWMEM
                /* in place, a picture reference marking can't store has
                 * overwritten the reference left in the DPB */
                if (tmp != HANTRO_OK && LM_OVERWRITES_REF(pStorage))
                    pStorage->lmNeedIdr = HANTRO_TRUE;
                else if (lmRecover && tmp == HANTRO_OK)
                    pStorage->lmNeedIdr = HANTRO_FALSE;
#endif
#if H264BSD_CHANGED_MAP
                /* a picture not stored as the reference can't be the next
                 * map's predecessor */
                if (tmp != HANTRO_OK)
                    pStorage->chmPrev = 0;
#endif
            }
            /* non-reference picture, just store for possible display
             * reordering */
            else
            {
                tmp = h264bsdMarkDecRefPic(pStorage->dpb, NULL,
                    pStorage->currImage, pStorage->sliceHeader->frameNum,
                    picOrderCnt,
                    IS_IDR_NAL_UNIT(pStorage->prevNalUnit) ?
                    HANTRO_TRUE : HANTRO_FALSE,
                    pStorage->currentPicId, pStorage->numConcealedMbs);
            }
        }

        pStorage->picStarted = HANTRO_FALSE;
        pStorage->validSliceInAccessUnit = HANTRO_FALSE;

        return(H264BSD_PIC_RDY);
    }
    else
        return(H264BSD_RDY);

}

#if H264BSD_CHANGED_MAP
/* OPT: h264bsdChangedMap: the decode below is DecodeAu, wrapped by the
 * h264bsdDecode after it */
static u32 DecodeAu(storage_t *pStorage, u8 *byteStrm, u32 len, u8 **picture, u32 *width, u32 *height);
#define h264bsdDecode DecodeAu
#endif

u32 h264bsdDecode(storage_t *pStorage, u8 *byteStrm, u32 len, u8 **picture, u32 *width, u32 *height) {
  u32 bytesRead = 0;
  u32 retCode = 3;
  u32 readBytes = 0;

    /* OPT: each call is a whole access unit: a NAL unit an earlier call left
     * unfinished is never resumed (the caller reuses one buffer, so the same
     * address holds other bytes now) */
    pStorage->prevBufNotFinished = HANTRO_FALSE;
    pStorage->prevBufPointer = NULL;

    do {
      /* OPT: never past the buffer (a resumed NAL unit's count from an
       * earlier buffer wrapped len and the start code search read on) */
      if (readBytes > len)
        return(H264BSD_ERROR);
      len -= readBytes;
      byteStrm += readBytes;
      retCode = h264bsdDecodeInternal(pStorage, byteStrm, len, &readBytes);
      /* OPT: stop at the buffer's end with H264BSD_RDY (upstream went on
       * and failed on an empty buffer): parameter sets alone aren't an
       * error, and the caller tells an unfinished picture by
       * h264bsdPictureStarted. A picture concealed without a single valid
       * slice is not stored, so it has nothing to output: go on with the
       * rest of the buffer (the NAL unit that ended it is decoded again) */
    } while (retCode == H264BSD_HDRS_RDY ||
             (retCode == H264BSD_RDY && readBytes < len) ||
             (retCode == H264BSD_PIC_RDY &&
              pStorage->dpb->outIndex >= pStorage->dpb->numOut));

    if (retCode == H264BSD_PIC_RDY) {
      /* OPT: a picture that reached no output (concealed with no valid
       * slice, or not stored) is an error, not a NULL dereference: damaged
       * parameter sets got here (esp32/tests/test_video_route.py, sps) */
      dpbOutPicture_t *out = h264bsdDpbOutputPicture(pStorage->dpb);
      if (out == NULL || pStorage->activeSps == NULL)
        return H264BSD_ERROR;
      *width = (pStorage->activeSps->picWidthInMbs)*16;
      *height = (pStorage->activeSps->picHeightInMbs)*16;
#if H264BSD_STRIP
      /* OPT: strip mode: the rows went to the row callback; the DPB's
       * picture is a stand-in, nothing to read */
      *picture = pStorage->stripMode ? NULL : out->data;
#else
      *picture = out->data;
#endif
#if H264BSD_CHANGED_MAP
      if (out->isIdr)
        pStorage->chmOut |= 4;
#endif
    }

    return retCode;
}
#if H264BSD_CHANGED_MAP
#undef h264bsdDecode

/* OPT: h264bsdChangedMap: a map only with the picture it describes; any
 * failure but a skipped non-reference picture (which leaves the reference
 * and the shown picture as they were) means the next picture can't trust
 * one */
u32 h264bsdDecode(storage_t *pStorage, u8 *byteStrm, u32 len, u8 **picture, u32 *width, u32 *height)
{
    u32 rc = DecodeAu(pStorage, byteStrm, len, picture, width, height);
    if (rc == H264BSD_PIC_RDY)
        pStorage->chmPrevData = *picture;
    else
    {
        pStorage->chmOut = 0;
#if H264BSD_ONEREF
        if (rc >= H264BSD_ERROR && !pStorage->lmNonRef)
#else
        if (rc >= H264BSD_ERROR)
#endif
            pStorage->chmPrev = 0;
    }
    return rc;
}

/* OPT: see h264bsd_decoder.h */
u32 h264bsdChangedMap(storage_t *pStorage, const u8 **clean)
{
    *clean = pStorage->chmBits;
    return pStorage->chmOut;
}

/* OPT: see h264bsd_decoder.h */
void h264bsdChangedMapSkip(storage_t *pStorage, u32 skip)
{
    pStorage->chmSkip = (u8)(skip != 0);
}
#endif

/*------------------------------------------------------------------------------

    Function: h264bsdShutdown

        Functional description:
            Shutdown a decoder instance. Function frees all the memories
            allocated for the decoder instance.

        Inputs:
            pStorage    pointer to storage data structure

        Returns:
            none


------------------------------------------------------------------------------*/

void h264bsdShutdown(storage_t *pStorage)
{

/* Variables */

    u32 i;

/* Code */

    ASSERT(pStorage);

#if H264BSD_BUFFER_GATE
    h264bsdGateDpb(pStorage);   /* the pictures are freed below */
#endif
    for (i = 0; i < MAX_NUM_SEQ_PARAM_SETS; i++)
    {
        if (pStorage->sps[i])
        {
            FREE(pStorage->sps[i]->offsetForRefFrame);
            FREE(pStorage->sps[i]->vuiParameters);
            FREE(pStorage->sps[i]);
        }
    }

    for (i = 0; i < MAX_NUM_PIC_PARAM_SETS; i++)
    {
        if (pStorage->pps[i])
        {
            FREE(pStorage->pps[i]->runLength);
            FREE(pStorage->pps[i]->topLeft);
            FREE(pStorage->pps[i]->bottomRight);
            FREE(pStorage->pps[i]->sliceGroupId);
            FREE(pStorage->pps[i]);
        }
    }

    if (pStorage->goodSps)
    {
        FREE(pStorage->goodSps->offsetForRefFrame);
        FREE(pStorage->goodSps->vuiParameters);
        FREE(pStorage->goodSps);
    }
    if (pStorage->goodPps)
    {
        FREE(pStorage->goodPps->runLength);
        FREE(pStorage->goodPps->topLeft);
        FREE(pStorage->goodPps->bottomRight);
        FREE(pStorage->goodPps->sliceGroupId);
        FREE(pStorage->goodPps);
    }

    FREE(pStorage->mbLayer);
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

    if(pStorage->conversionBuffer != NULL) FREE(pStorage->conversionBuffer);

    h264bsdFreeDpb(pStorage->dpb);
}

/*------------------------------------------------------------------------------

    Function name: h264bsdAlloc

        Functional description:
            Allocate storage for a decoder

        Inputs:
            none

        Outputs:
            none

        Returns:
            pStorage            pointer to uninitialized storage structure

------------------------------------------------------------------------------*/

#if H264BSD_SPLIT_PLANES
/* OPT: see h264bsd_decoder.h */
void h264bsdPicturePlanes(storage_t *pStorage, const u8 *luma, u8 **cb, u8 **cr)
{
    image_t *im = pStorage->currImage;
    *cb = h264bsdCbOf(im, luma);
    *cr = *cb + (im->yBase ? im->crDelta : 64 * (long)(im->width * im->height));
}
#endif

storage_t* h264bsdAlloc()
{
    return (storage_t*)h264bsdMalloc(sizeof(storage_t));
}

/*------------------------------------------------------------------------------

    Function name: h264bsdFree

        Functional description:
            Free storage for a decoder

        Inputs:
            pStorage            pointer to storage structure

        Outputs:
            none

        Returns:
            none

------------------------------------------------------------------------------*/

void h264bsdFree(storage_t *pStorage)
{
    h264bsdRelease(pStorage);   /* OPT: the allocator it came from */
}

/* OPT: see h264bsd_decoder.h */
void h264bsdSetMaxReferences(storage_t *pStorage, u32 maxRefs)
{
    pStorage->maxRefFrames = maxRefs;
}

void h264bsdSetMaxSize(storage_t *pStorage, u32 maxWidthMbs, u32 maxHeightMbs)
{
    pStorage->maxWidthMbs = maxWidthMbs;
    pStorage->maxHeightMbs = maxHeightMbs;
}

#if H264BSD_LIMITS
/* OPT: see h264bsd_decoder.h */
void h264bsdSetLimits(storage_t *pStorage, u32 maxMbs, u32 maxRefFrames)
{
    pStorage->maxMbs = maxMbs;
    pStorage->maxRefFrames = pStorage->maxDpbFrames = maxRefFrames;
}
#endif

#if H264BSD_BUFFER_GATE
/* OPT: see h264bsd_decoder.h */
u32 h264bsdSetBufferGate(storage_t *pStorage,
    void (*fn)(void *ctx, const u8 *pic), void *ctx)
{
#if H264BSD_STRIP
    if (fn && pStorage->stripMode)
        return(HANTRO_NOK);   /* strip mode: no picture buffers to gate */
#endif
    pStorage->bufferGateFn = fn;
    pStorage->bufferGateCtx = ctx;
    return(HANTRO_OK);
}
#endif

/* OPT: see h264bsd_decoder.h */
void h264bsdSetMaxCropped(storage_t *pStorage, u32 maxWidth, u32 maxHeight)
{
    pStorage->maxCroppedWidth = maxWidth;
    pStorage->maxCroppedHeight = maxHeight;
}

/* OPT: see h264bsd_decoder.h */
u32 h264bsdCroppingOffsets(const storage_t *pStorage, u32 *left, u32 *right, u32 *top, u32 *bottom)
{
    const seqParamSet_t *sps = pStorage->activeSps;
    *left = *right = *top = *bottom = 0;
    if (sps == NULL)
        return HANTRO_NOK;
    if (sps->frameCroppingFlag)
    {
        *left = sps->frameCropLeftOffset;
        *right = sps->frameCropRightOffset;
        *top = sps->frameCropTopOffset;
        *bottom = sps->frameCropBottomOffset;
    }
    return HANTRO_OK;
}

/* OPT: see h264bsd_decoder.h */
void h264bsdSetProgress(storage_t *pStorage,
    void (*fn)(void *ctx, const u8 *pic, u32 widthMbs, u32 heightMbs, u32 rowsFinal), void *ctx)
{
    pStorage->progressFn = fn;
    pStorage->progressCtx = ctx;
    pStorage->progressRows = 0;
}

/* OPT: see h264bsd_decoder.h */
u32 h264bsdPictureStarted(const storage_t *pStorage)
{
    return pStorage->picStarted || pStorage->validSliceInAccessUnit ||
        pStorage->prevBufNotFinished;
}

/* OPT: see h264bsd_decoder.h */
void h264bsdAbandonPicture(storage_t *pStorage)
{
#if H264BSD_LOWMEM
    if (pStorage->lmFlags &&
        (pStorage->picStarted || pStorage->validSliceInAccessUnit))
        h264bsdLowMemDrop(pStorage);
#endif
    pStorage->picStarted = HANTRO_FALSE;
    pStorage->validSliceInAccessUnit = HANTRO_FALSE;
    pStorage->skipRedundantSlices = HANTRO_FALSE;
    /* the next buffer is always new: never resume inside the old one (the
     * caller reuses one buffer for every access unit) */
    pStorage->prevBufNotFinished = HANTRO_FALSE;
    pStorage->prevBufPointer = NULL;
    pStorage->numConcealedMbs = 0;
    /* mb is sized for picSizeInMbs only once a pending activation is done
     * (and then cleared whole) */
#if H264BSD_LOWMEM
    if (!pStorage->pendingActivation && (pStorage->mb || pStorage->lmRingRows))
#else
    if (!pStorage->pendingActivation && pStorage->mb)
#endif
        h264bsdResetStorage(pStorage);
    pStorage->dbNextMb = pStorage->dbRowsDone = pStorage->dbBad = 0;
    pStorage->progressRows = 0;
}

static seqParamSet_t *CopySps(const seqParamSet_t *src)
{
    seqParamSet_t *d;
    ALLOCATE(d, 1, seqParamSet_t);
    if (d == NULL)
        return NULL;
    *d = *src;
    d->offsetForRefFrame = NULL;
    d->vuiParameters = NULL;
    if (src->offsetForRefFrame && src->numRefFramesInPicOrderCntCycle)
    {
        ALLOCATE(d->offsetForRefFrame, src->numRefFramesInPicOrderCntCycle, i32);
        if (d->offsetForRefFrame == NULL)
            goto fail;
        memcpy(d->offsetForRefFrame, src->offsetForRefFrame,
            src->numRefFramesInPicOrderCntCycle * sizeof(i32));
    }
    if (src->vuiParameters)
    {
        ALLOCATE(d->vuiParameters, 1, vuiParameters_t);
        if (d->vuiParameters == NULL)
            goto fail;
        *d->vuiParameters = *src->vuiParameters;
    }
    return d;
fail:
    FREE(d->offsetForRefFrame);
    FREE(d);
    return NULL;
}

static void FreeSps(seqParamSet_t **p)
{
    if (*p)
    {
        FREE((*p)->offsetForRefFrame);
        FREE((*p)->vuiParameters);
        FREE(*p);
    }
}

static u32 *CopyU32(const u32 *src, u32 n, u32 *ok)
{
    u32 *d = NULL;
    if (src == NULL || n == 0)
        return NULL;
    ALLOCATE(d, n, u32);
    if (d == NULL)
        *ok = HANTRO_FALSE;
    else
        memcpy(d, src, n * sizeof(u32));
    return d;
}

static void FreePps(picParamSet_t **p)
{
    if (*p)
    {
        FREE((*p)->runLength);
        FREE((*p)->topLeft);
        FREE((*p)->bottomRight);
        FREE((*p)->sliceGroupId);
        FREE(*p);
    }
}

static picParamSet_t *CopyPps(const picParamSet_t *src)
{
    picParamSet_t *d;
    u32 ok = HANTRO_TRUE;
    u32 groups = src->numSliceGroups;
    ALLOCATE(d, 1, picParamSet_t);
    if (d == NULL)
        return NULL;
    *d = *src;
    d->runLength = CopyU32(src->runLength, groups, &ok);
    d->topLeft = CopyU32(src->topLeft, groups ? groups - 1 : 0, &ok);
    d->bottomRight = CopyU32(src->bottomRight, groups ? groups - 1 : 0, &ok);
    d->sliceGroupId = CopyU32(src->sliceGroupId, src->picSizeInMapUnits, &ok);
    if (!ok)
        FreePps(&d);
    return d;
}

/* OPT: see h264bsd_decoder.h */
u32 h264bsdKeepParamSets(storage_t *pStorage)
{
    seqParamSet_t *sps;
    picParamSet_t *pps;

    if (pStorage->activeSps == NULL || pStorage->activePps == NULL)
        return HANTRO_OK;
    /* The format locks even if the copies below can't be made. */
    pStorage->lockWidthMbs = pStorage->activeSps->picWidthInMbs;
    pStorage->lockHeightMbs = pStorage->activeSps->picHeightInMbs;
    pStorage->lockProfileIdc = pStorage->activeSps->profileIdc;
    sps = CopySps(pStorage->activeSps);
    pps = sps ? CopyPps(pStorage->activePps) : NULL;
    if (pps == NULL)
    {
        FreeSps(&sps);
        return MEMORY_ALLOCATION_ERROR;
    }
    FreeSps(&pStorage->goodSps);
    FreePps(&pStorage->goodPps);
    pStorage->goodSps = sps;
    pStorage->goodPps = pps;
    return HANTRO_OK;
}

/* OPT: see h264bsd_decoder.h */
void h264bsdAllowParamSetChange(storage_t *pStorage, u32 allow)
{
    pStorage->ppsChangeAllowed = allow;
}

/* OPT: see h264bsd_decoder.h */
u32 h264bsdRestoreParamSets(storage_t *pStorage)
{
    seqParamSet_t *sps;
    picParamSet_t *pps;
    u32 tmp, allowed;

    if (pStorage->goodSps == NULL || pStorage->goodPps == NULL)
        return HANTRO_OK;
    sps = CopySps(pStorage->goodSps);
    pps = sps ? CopyPps(pStorage->goodPps) : NULL;
    if (pps == NULL)
    {
        FreeSps(&sps);
        return MEMORY_ALLOCATION_ERROR;
    }
    /* Stored as if they had come in the stream: unchanged ones are no-ops,
     * changed ones are re-activated at the next IDR picture. The store takes
     * the copies' arrays (and frees them when it doesn't keep them). */
    allowed = pStorage->ppsChangeAllowed;
    pStorage->ppsChangeAllowed = HANTRO_TRUE;   /* the kept ones: never refused */
    tmp = h264bsdStoreSeqParamSet(pStorage, sps);
    if (tmp == MEMORY_ALLOCATION_ERROR)
        FreeSps(&sps);   /* not taken */
    FREE(sps);
    if (tmp == HANTRO_OK)
        tmp = h264bsdStorePicParamSet(pStorage, pps);
    else
        FreePps(&pps);
    if (tmp == MEMORY_ALLOCATION_ERROR)
        FreePps(&pps);
    FREE(pps);
    pStorage->ppsChangeAllowed = allowed;
    return tmp;
}
