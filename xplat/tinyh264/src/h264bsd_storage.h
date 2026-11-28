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

#ifndef H264SWDEC_STORAGE_H
#define H264SWDEC_STORAGE_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_cfg.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_pic_param_set.h"
#include "h264bsd_macroblock_layer.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_slice_header.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_dpb.h"
#include "h264bsd_pic_order_cnt.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

typedef struct
{
    u32 sliceId;
    u32 numDecodedMbs;
    u32 lastMbAddr;
} sliceStorage_t;

/* structure to store parameters needed for access unit boundary checking */
typedef struct
{
    nalUnit_t nuPrev[1];
    u32 prevFrameNum;
    u32 prevIdrPicId;
    u32 prevPicOrderCntLsb;
    i32 prevDeltaPicOrderCntBottom;
    i32 prevDeltaPicOrderCnt[2];
    u32 firstCallFlag;
} aubCheck_t;

/* storage data structure, holds all data of a decoder instance */
typedef struct storage
{
    /* active paramet set ids and pointers */
    u32 oldSpsId;
    u32 activePpsId;
    u32 activeSpsId;
    picParamSet_t *activePps;
    seqParamSet_t *activeSps;
    seqParamSet_t *sps[MAX_NUM_SEQ_PARAM_SETS];
    picParamSet_t *pps[MAX_NUM_PIC_PARAM_SETS];

    /* current slice group map, recomputed for each slice */
    u32 *sliceGroupMap;

    u32 picSizeInMbs;

    /* this flag is set after all macroblocks of a picture successfully
     * decoded -> redundant slices not decoded */
    u32 skipRedundantSlices;
    u32 picStarted;

    /* flag to indicate if current access unit contains any valid slices */
    u32 validSliceInAccessUnit;

    /* store information needed for handling of slice decoding */
    sliceStorage_t slice[1];

    /* number of concealed macroblocks in the current image */
    u32 numConcealedMbs;

    /* picId given by application */
    u32 currentPicId;

    /* macroblock specific storages, size determined by image dimensions */
    mbStorage_t *mb;

    /* flag to store noOutputReordering flag set by the application */
    u32 noReordering;

    /* DPB */
    dpbStorage_t dpb[1];

    /* structure to store picture order count related information */
    pocStorage_t poc[1];

    /* access unit boundary checking related data */
    aubCheck_t aub[1];

    /* current processed image */
    image_t currImage[1];
    /* OPT: row-lagged deblocking state (zeroed per picture) */
    u32 dbNextMb, dbRowsDone, dbBad;
    /* OPT: progress hook (h264bsdSetProgress): luma rows of currImage that
     * are final, reported as they grow */
    void (*progressFn)(void *ctx, const u8 *pic, u32 widthMbs, u32 heightMbs, u32 rowsFinal);
    void *progressCtx;
    u32 progressRows;
    /* OPT: the largest picture a sequence parameter set may describe
     * (h264bsdSetMaxSize), in macroblocks; 0: no limit but the overall one */
    u32 maxWidthMbs, maxHeightMbs;
    u32 maxRefFrames; /* optional caller limit; zero preserves streaming defaults */
#if H264BSD_LIMITS
    /* OPT: h264bsdSetLimits: the most macroblocks a picture may have (0: no
     * limit); its reference-frame limit is maxRefFrames above, and
     * maxDpbFrames holds the DPB to it with output reordering too */
    u32 maxMbs, maxDpbFrames;
#endif
#if H264BSD_BUFFER_GATE
    /* OPT: h264bsdSetBufferGate (NULL: off) */
    void (*bufferGateFn)(void *ctx, const u8 *pic);
    void *bufferGateCtx;
#endif
    /* OPT: the largest picture after frame cropping (h264bsdSetMaxCropped),
     * in pixels; 0: no limit */
    u32 maxCroppedWidth, maxCroppedHeight;
    /* OPT: the stream's format, from its last IDR picture
     * (h264bsdKeepParamSets): sequence parameter sets for another size or
     * profile are refused; 0: not yet. And that picture's parameter sets,
     * which h264bsdRestoreParamSets puts back after an error. */
    u32 lockWidthMbs, lockHeightMbs, lockProfileIdc;
    /* OPT: a parameter set that differs from the kept one with its id is
     * taken only while this is set: in a keyframe's access unit
     * (h264bsdAllowParamSetChange) */
    u32 ppsChangeAllowed;
    seqParamSet_t *goodSps;
    picParamSet_t *goodPps;
#if H264BSD_LOWMEM
    /* OPT: low-memory mode (h264bsdSetLowMem, h264bsd_lowmem.c) */
    u32 lmFlags;            /* H264BSD_LOWMEM_* */
    u32 lmRingRowsWanted;   /* macroblock rows in the ring */
    u32 lmRingRows;         /* 0: one entry per macroblock */
    mbStorage_t *lmMb;      /* the ring */
    u32 lmTop;              /* lowest macroblock row set up this picture */
    u32 lmRowsWanted;       /* in-place D asked for (luma rows), 0: auto */
    u32 lmNeedIdr;          /* a picture failed: wait for an IDR picture */
    u32 lmRecover;          /* this picture is intra so far: it may restart */
    u32 lmFail;             /* the current picture failed */
    u32 lmSkip;             /* the current picture is skipped (non-reference) */
    u32 lmIpBad;            /* a prediction read an overwritten row */
    u32 lmRefused;          /* pictures that failed or were skipped */
#if H264BSD_ONEREF
    /* OPT: an SPS with max_num_ref_frames > 1 decoded in place keeping one
     * reference (a stream that uses only the last one); the current picture
     * was skipped as a non-reference one, and how many were */
    u32 lmOneRef, lmNonRef, lmNonRefs;
#endif
#endif
#if H264BSD_STRIP
    /* OPT: strip mode (h264bsdSetStrip, h264bsd_strip.c): intra-only streams
     * decoded into a ring of macroblock rows instead of whole pictures; rows
     * are handed to stripRowFn as soon as they are final */
    u32 stripMode;          /* K: rows freed per shift; 0 = off */
    u8 *stripY, *stripCb;   /* ring planes (Cr = Cb + currImage->crDelta) */
    mbStorage_t *stripMb;   /* ring of (K + 2) * width entries */
    u32 stripBase;          /* picture macroblock row in ring slot 0 */
    u32 stripEmitted;       /* macroblock rows handed out this picture */
    void (*stripRowFn)(void *ctx, u32 mbRow, const u8 *y, const u8 *cb,
                       const u8 *cr, u32 yStride, u32 cStride);
    void *stripRowCtx;
#endif

#if H264BSD_CHANGED_MAP
    /* OPT: h264bsdChangedMap. chmBits bit i: macroblock i of the picture
     * being decoded is clean (the previous output picture's pixels before
     * the loop filter); chmOk: this picture's map is trusted so far; chmDb:
     * one of its slices deblocks; chmPrev: the last output picture is the
     * one the next P picture predicts from (a fully decoded reference
     * picture, nothing long-term or adaptively marked), chmFn its frame_num;
     * chmOut: the output picture's map (0 none, bit 0 trusted, bit 1
     * deblocked, bit 2 an IDR picture), valid until the next h264bsdDecode
     * call; chmSkip: the caller wants no map (h264bsdChangedMapSkip: the
     * macroblock loop skips its bit writes; the validity above is still
     * tracked, so the picture after the skip ends has a map). A P slice's
     * reference index 0 must be the last output picture's buffer, not just
     * have the frame_num after it: a damaged stream can leave a stale
     * picture with that frame_num in the DPB. */
    u8 chmBits[(H264BSD_CHANGED_MAP_MBS + 7) / 8];
    u8 chmOk, chmDb, chmPrev, chmOut, chmSkip;
    u32 chmFn;
    const u8 *chmPrevData;  /* the last output picture's buffer */
#endif

    /* last valid NAL unit header is stored here */
    nalUnit_t prevNalUnit[1];

    /* slice header, second structure used as a temporary storage while
     * decoding slice header, first one stores last successfully decoded
     * slice header */
    sliceHeader_t sliceHeader[2];

    /* fields to store old stream buffer pointers, needed when only part of
     * a stream buffer is processed by h264bsdDecode function */
    u32 prevBufNotFinished;
    u8 *prevBufPointer;
    u32 prevBytesConsumed;
    strmData_t strm[1];

    /* macroblock layer structure, there is no need to store this but it
     * would have increased the stack size excessively and needed to be
     * allocated from head -> easiest to put it here */
    macroblockLayer_t *mbLayer;

    u32 pendingActivation; /* Activate parameter sets after returning
                              HEADERS_RDY to the user */
    u32 intraConcealmentFlag; /* 0 gray picture for corrupted intra
                                 1 previous frame used if available */
    u32* conversionBuffer; // used to perform yuv conversion
    size_t conversionBufferSize;
} storage_t;

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

void h264bsdInitStorage(storage_t *pStorage);
void h264bsdResetStorage(storage_t *pStorage);
u32 h264bsdIsStartOfPicture(storage_t *pStorage);
u32 h264bsdIsEndOfPicture(storage_t *pStorage);
u32 h264bsdStoreSeqParamSet(storage_t *pStorage, seqParamSet_t *pSeqParamSet);
#if H264BSD_LIMITS
/* h264bsdStoreSeqParamSet: refused by h264bsdSetLimits */
#define H264BSD_SPS_PAST_LIMITS 2
#endif
u32 h264bsdStorePicParamSet(storage_t *pStorage, picParamSet_t *pPicParamSet);
u32 h264bsdActivateParamSets(storage_t *pStorage, u32 ppsId, u32 isIdr);
void h264bsdComputeSliceGroupMap(storage_t *pStorage,
    u32 sliceGroupChangeCycle);

u32 h264bsdCheckAccessUnitBoundary(
  strmData_t *strm,
  nalUnit_t *nuNext,
  storage_t *storage,
  u32 *accessUnitBoundaryFlag);

u32 h264bsdValidParamSets(storage_t *pStorage);
#if H264BSD_BUFFER_GATE
void h264bsdGateDpb(storage_t *pStorage);
#endif

#if H264BSD_LOWMEM
/* OPT: low-memory mode, h264bsd_lowmem.c (flags: h264bsdSetLowMem) */
#define H264BSD_LOWMEM_MBRING 1u
#define H264BSD_LOWMEM_INPLACE 2u
/* in-place decoding overwrites the reference: a failed picture damages it */
#define LM_OVERWRITES_REF(s) ((s)->dpb->lmRows && \
    (s)->dpb->lmRows < (s)->currImage->height * 16)
u32 h264bsdLowMemAllocMb(storage_t *s);
void h264bsdLowMemFreeMb(storage_t *s);
void h264bsdLowMemBeginPicture(storage_t *s);
mbStorage_t *h264bsdLowMemMb(storage_t *s, u32 currMbAddr);
u32 h264bsdLowMemRows(const storage_t *s);
void h264bsdLowMemPlace(storage_t *s);
void h264bsdLowMemLayout(storage_t *s);
void h264bsdLowMemDrop(storage_t *s);
#endif
#if H264BSD_STRIP
/* OPT: strip mode, h264bsd_strip.c */
u32 h264bsdStripAlloc(storage_t *pStorage);
void h264bsdStripFree(storage_t *pStorage);
void h264bsdStripBegin(storage_t *pStorage);
void h264bsdStripRow(storage_t *pStorage, u32 mbRow);
void h264bsdStripFinish(storage_t *pStorage);
#endif
/* OPT: a mode that conceals nothing: what it can't decode exactly is dropped */
#if H264BSD_LOWMEM && H264BSD_STRIP
#define H264BSD_DROPS(s) ((s)->lmFlags || (s)->stripMode)
#elif H264BSD_LOWMEM
#define H264BSD_DROPS(s) ((s)->lmFlags)
#elif H264BSD_STRIP
#define H264BSD_DROPS(s) ((s)->stripMode)
#endif

#endif /* #ifdef H264SWDEC_STORAGE_H */

