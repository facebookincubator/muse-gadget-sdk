/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * OPT: low-memory mode (H264BSD_LOWMEM, h264bsdSetLowMem), for boards
 * without external RAM.
 *
 * Macroblock ring (H264BSD_LOWMEM_MBRING): the decoder keeps a macroblock
 * entry (160 bytes in these builds on 32-bit targets) per macroblock of the
 * picture, but nothing reads an entry more than two macroblock rows back:
 * intra prediction, motion vector prediction and the coefficient contexts
 * look one row up, and this decoder deblocks a macroblock as soon as the
 * macroblock below-right of it is decoded (h264bsd_slice_data.c), so when
 * row r starts, row r-2 is filtered and only rows r-2 (the top neighbours
 * of row r-1's filtering) and r-1 are live. The ring is circular, three
 * rows, row r in slot r % 3, and nothing is ever copied: a row taking over
 * its slot starts empty, its neighbours in the slot above. Inter prediction
 * reads only reference pixels, never another picture's macroblock entries,
 * so P pictures work too.
 *
 * In-place pictures (H264BSD_LOWMEM_INPLACE): with one reference frame and
 * no output reordering the decoder holds two pictures, the reference and the
 * one being decoded. Here they share one buffer of H + D luma rows (and
 * H/2 + D/2 rows per chroma plane): the reference sits D rows down, the new
 * picture is decoded at the top, so writing row y overwrites reference row
 * y - D. A partition of macroblock row r reads reference rows from
 * 16r - M - 2 down (M = the largest upward motion vector, -2 for the 6-tap
 * filter) and by then rows up to 16r + 15 - D are gone, so D >= M + 18 keeps
 * every read intact; chroma has twice the margin. M is the stream's own
 * bound (VUI log2_max_mv_length_vertical, else its level's limit) unless the
 * caller asks for a smaller D; every prediction checks the rows it reads
 * (image_t.ipWater) and a picture that would read an overwritten row is
 * reported as an error, so the output is either the exact picture or an
 * error, never a wrong picture. When the next picture starts, the finished
 * one moves down D rows (one memmove per plane) to become the reference.
 *
 * Either way: no error concealment (it needs the whole picture). A damaged
 * picture is reported as an error and dropped (h264bsd_decoder.c); with the
 * ring alone the next picture predicts from the intact reference, in place
 * the decoder waits for an IDR or all-intra reference picture once the
 * reference has been partly overwritten. Pictures with arbitrary slice
 * order, slice groups, redundant slices or frame number gaps are refused
 * the same way, and non-reference pictures are skipped when they would
 * overwrite the reference.
 */
#include <string.h>
#include "h264bsd_storage.h"
#include "h264bsd_util.h"
#include "h264bsd_decoder.h"

#if H264BSD_LOWMEM

/* Row `row` takes over its slot (row % lmRingRows): no macroblock decoded
 * yet, neighbours in the slot above. */
static void RingInitRow(storage_t *s, u32 row)
{
    u32 w = s->activeSps->picWidthInMbs, rows = s->lmRingRows, c;
    mbStorage_t *e = s->lmMb + (row % rows) * w;
    mbStorage_t *up = row ? s->lmMb + ((row - 1) % rows) * w : NULL;
    for (c = 0; c < w; c++, e++)
    {
        e->sliceId = 0;
        e->decoded = 0;
        e->mbA = c ? e - 1 : NULL;
        e->mbB = up ? up + c : NULL;
        e->mbC = up && c + 1 < w ? up + c + 1 : NULL;
        e->mbD = up && c ? up + c - 1 : NULL;
    }
}

u32 h264bsdLowMemAllocMb(storage_t *s)
{
    u32 w = s->activeSps->picWidthInMbs, h = s->activeSps->picHeightInMbs;
    u32 rows = s->lmRingRowsWanted;
    if (rows > h)
        rows = h;
    s->lmRingRows = s->currImage->mbRingRows = rows;
    ALLOCATE(s->lmMb, rows * w, mbStorage_t);
    if (!s->lmMb)
        return HANTRO_NOK;
    memset(s->lmMb, 0, rows * w * sizeof(mbStorage_t));
    s->mb = NULL;   /* not indexable by address: h264bsdLowMemMb */
    s->lmTop = 0;
    return HANTRO_OK;
}

void h264bsdLowMemFreeMb(storage_t *s)
{
    FREE(s->lmMb);
    s->mb = NULL;
    s->lmRingRows = s->currImage->mbRingRows = 0;
}

/* A new picture: only row 0 is set up. */
void h264bsdLowMemBeginPicture(storage_t *s)
{
    s->lmTop = 0;
    RingInitRow(s, 0);
}

/* The entry of macroblock currMbAddr, setting up the rows it skips to;
 * NULL when its row has left the ring (out of order). */
mbStorage_t *h264bsdLowMemMb(storage_t *s, u32 currMbAddr)
{
    u32 w = s->activeSps->picWidthInMbs, row = currMbAddr / w;
    if (row > s->lmTop)
    {
        u32 r = row - s->lmTop > s->lmRingRows ? row - s->lmRingRows : s->lmTop;
        while (r < row)
            RingInitRow(s, ++r);
        s->lmTop = row;
    }
    else if (row + s->lmRingRows <= s->lmTop)
        return NULL;
    return s->lmMb + (row % s->lmRingRows) * w + currMbAddr % w;
}

/* D for in-place pictures: the caller's, else from the stream's own motion
 * vector bound; even, at most the picture height (H: two whole pictures). */
u32 h264bsdLowMemRows(const storage_t *s)
{
    const seqParamSet_t *sps = s->activeSps;
    u32 h = sps->picHeightInMbs * 16, d, m;
    if (s->lmRowsWanted)
        d = s->lmRowsWanted;
    else
    {
        if (sps->vuiParametersPresentFlag && sps->vuiParameters &&
            sps->vuiParameters->bitstreamRestrictionFlag &&
            sps->vuiParameters->log2MaxMvLengthVertical < 16)
            m = (1u << sps->vuiParameters->log2MaxMvLengthVertical) >> 2;
        else if (sps->levelIdc <= 10 || sps->levelIdc == 9)
            m = 64;
        else if (sps->levelIdc <= 20)
            m = 128;
        else if (sps->levelIdc <= 30)
            m = 256;
        else
            m = 512;
        d = m + 18;
    }
    d = (d + 1) & ~1u;
    return d < h ? d : h;
}

/* The current picture is about to be decoded into dpb->currentOut: put it at
 * the top of the shared buffer and the reference D rows down. */
void h264bsdLowMemPlace(storage_t *s)
{
    dpbStorage_t *dpb = s->dpb;
    image_t *im = s->currImage;
    dpbPicture_t *cur = dpb->currentOut, *other;
    u32 w = im->width * 16, h = im->height * 16, d = dpb->lmRows;
    if (!d)
        return;
    if (cur->data != dpb->lmY)
    {
        other = dpb->buffer[0].data == dpb->lmY ? &dpb->buffer[0] : &dpb->buffer[1];
        if (other->status != UNUSED && other->status != NON_EXISTING)
        {
            memmove(dpb->lmY + d * w, dpb->lmY, h * w);
            memmove(dpb->lmCb + (d / 2) * (w / 2), dpb->lmCb, (h / 2) * (w / 2));
            memmove(dpb->lmCr + (d / 2) * (w / 2), dpb->lmCr, (h / 2) * (w / 2));
        }
        other->data = cur->data;
        cur->data = dpb->lmY;
    }
    h264bsdImageSetData(im, cur->data);
    s->lmIpBad = 0;
    /* nothing overwritten yet; h264bsd_slice_data.c raises it row by row */
    im->ipWater = (i32)(0x80000000u);
    im->ipBad = &s->lmIpBad;
}

/* The decoded picture's image layout after the DPB was (re)initialized */
void h264bsdLowMemLayout(storage_t *s)
{
    image_t *im = s->currImage;
    dpbStorage_t *dpb = s->dpb;
    /* a prediction that failed in place before must not refuse the pictures
     * of a layout that isn't (h264bsdLowMemPlace clears it only in place) */
    s->lmIpBad = 0;
    im->ipWater = (i32)(0x80000000u);
    im->ipBad = &s->lmIpBad;
    if (dpb->lmRows)
    {
        im->yBase = dpb->lmY;
        im->cbBase = dpb->lmCb;
        im->crDelta = (long)(dpb->lmCr - dpb->lmCb);
    }
    else
    {
        im->yBase = im->cbBase = NULL;
    }
}

/* OPT: see h264bsd_decoder.h */
void h264bsdSetLowMem(storage_t *pStorage, u32 flags)
{
    /* only before the first parameter sets are activated (as h264bsdSetStrip) */
    if (pStorage->activeSps || pStorage->sliceGroupMap)
        return;
#if H264BSD_STRIP
    pStorage->stripMode = 0;   /* not both */
#endif
    pStorage->lmFlags = flags & (H264BSD_LOWMEM_MBRING | H264BSD_LOWMEM_INPLACE);
    if (flags & H264BSD_LOWMEM_INPLACE)
        pStorage->lmFlags |= H264BSD_LOWMEM_MBRING;
    pStorage->lmRingRowsWanted = 3;
    pStorage->lmRowsWanted = ((flags >> 8) & 0xFF) * 8;
}

/* OPT: see h264bsd_decoder.h */
void h264bsdLowMemStats(storage_t *pStorage, u32 *inPlaceRows, u32 *refused)
{
    if (inPlaceRows)
        *inPlaceRows = pStorage->dpb->lmRows;
    if (refused)
        *refused = pStorage->lmRefused;
}

#if H264BSD_ONEREF
/* OPT: see h264bsd_decoder.h */
u32 h264bsdRefInfo(storage_t *pStorage, u32 *nonRefsSkipped)
{
    *nonRefsSkipped = pStorage->lmNonRefs;
    return pStorage->activeSps ? pStorage->activeSps->numRefFrames : 0;
}
#endif

/* The picture being decoded is dropped, unfinished (the next access unit has
 * begun, or the caller abandons it): nothing of it is output or concealed.
 * If it had started to overwrite an in-place reference, wait for an IDR or
 * intra picture; otherwise the next picture predicts from the reference as
 * before. */
void h264bsdLowMemDrop(storage_t *s)
{
    if (!s->lmSkip && s->validSliceInAccessUnit && LM_OVERWRITES_REF(s))
        s->lmNeedIdr = HANTRO_TRUE;
    if (!s->lmFail)
        s->lmRefused++;
    s->lmFail = s->lmSkip = s->lmRecover = HANTRO_FALSE;
}

#endif /* H264BSD_LOWMEM */
