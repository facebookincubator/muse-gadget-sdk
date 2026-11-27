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
 * OPT: strip mode (H264BSD_STRIP, h264bsdSetStrip) for intra-only streams:
 * decode into a ring of macroblock rows instead of a whole picture, and hand
 * each row out once it is final.
 *
 * For a board that can't hold one picture (a 360 x 360 picture is 194 KB of
 * YUV 4:2:0; the decoder wants two). An intra-only stream needs only the
 * rows around the one being decoded:
 *
 *   - intra prediction of row r reads the unfiltered bottom line of row r-1;
 *   - this decoder deblocks a macroblock as soon as nothing will predict from
 *     it unfiltered any more (h264bsd_slice_data.c), so when row r starts,
 *     rows <= r-2 are filtered and row r-2's filtering has finished changing
 *     row r-3 (its top edge reaches 3 lines up): row r-3 is final.
 *
 * The ring holds K + 2 rows (K = stripMode). When row r would not fit, the K
 * oldest rows (all final) go to stripRowFn, the two live rows (r-2 filtered,
 * r-1 unfiltered) move to the front, and the picture pointers are re-based
 * so that the rest of the decoder keeps addressing rows by their picture
 * position: currImage->data / cbData and pStorage->mb point K*row-sized
 * steps "before" the ring (never dereferenced there; the decoder adds a
 * row offset that lands in the ring first). Those addresses are computed as
 * uintptr_t and converted to pointers: implementation-defined, and the
 * plain address on flat-address targets, the ones this mode is for (ISO C
 * leaves pointer arithmetic outside an object undefined, and offsetting
 * every row address instead would reach all through the decoder).
 * Neighbour pointers of the macroblock entries are rebuilt
 * after each move. The decoded pixels are bit-exact with whole-picture
 * decoding (the same operations in the same order).
 *
 * Memory at 360 x 360 (23 x 23 macroblocks), K = 4: pixels 6 rows x 8.6 KB =
 * 52 KB, macroblock entries 6 x 23 x 216 B = 30 KB (160 B each with
 * H264BSD_LOWMEM: 22 KB); K = 1 halves both. The decoder keeps no pictures
 * (the DPB's are 64-byte stand-ins).
 *
 * Refused (an error, the picture dropped, nothing concealed): P slices,
 * redundant slices, macroblocks out of raster order (arbitrary slice order,
 * slice groups).
 */
#include <string.h>
#include "h264bsd_storage.h"
#include "h264bsd_util.h"
#include "h264bsd_decoder.h"

#if H264BSD_STRIP

static u32 ring_rows(const storage_t *s) { return s->stripMode + 2; }

/* Re-point the picture at the ring for slot 0 = picture row stripBase. */
H264BSD_COLD_FN static void rebase(storage_t *s)
{
    image_t *im = s->currImage;
    /* addresses before the ring: integers first, then pointers (see the top
     * of this file) */
    uintptr_t w = im->width, b = s->stripBase;
    im->data = (u8 *)((uintptr_t)s->stripY - b * w * 256);
    im->cbData = (u8 *)((uintptr_t)s->stripCb - b * w * 64);
    im->crDelta = ring_rows(s) * im->width * 64;
    s->mb = (mbStorage_t *)((uintptr_t)s->stripMb - b * w * sizeof(mbStorage_t));
}

/* Neighbours of every ring entry, relative to its current slot. Slot 0 has
 * no row above in the ring; by the time a row reaches slot 0 neither its
 * decoding nor its filtering needs one. */
H264BSD_COLD_FN static void neighbours(storage_t *s)
{
    u32 w = s->currImage->width, rows = ring_rows(s), r, c;
    for (r = 0; r < rows; r++)
    {
        for (c = 0; c < w; c++)
        {
            mbStorage_t *e = s->stripMb + r * w + c;
            u32 above = r > 0 && s->stripBase + r > 0;
            e->mbA = c ? e - 1 : NULL;
            e->mbB = above ? e - w : NULL;
            e->mbC = above && c + 1 < w ? e - w + 1 : NULL;
            e->mbD = above && c ? e - w - 1 : NULL;
        }
    }
}

static void emit(storage_t *s, u32 row)
{
    u32 w = s->currImage->width, slot = row - s->stripBase;
    const u8 *y = s->stripY + slot * 16 * w * 16;
    const u8 *cb = s->stripCb + slot * 8 * w * 8;
    if (s->stripRowFn)
        s->stripRowFn(s->stripRowCtx, row, y, cb, cb + s->currImage->crDelta,
            w * 16, w * 8);
    s->stripEmitted = row + 1;
}

H264BSD_COLD_FN u32 h264bsdStripAlloc(storage_t *s)
{
    u32 w = s->activeSps->picWidthInMbs, rows = ring_rows(s);
    u32 ysz = rows * w * 256, csz = rows * w * 64;
    /* every offset into the ring is computed in u32 */
    if ((uint64_t)rows * w * 384 > 0xFFFFFFFFu)
        return HANTRO_NOK;
    ALLOCATE(s->stripY, ysz + 2 * csz, u8);
    ALLOCATE(s->stripMb, rows * w, mbStorage_t);
    if (!s->stripY || !s->stripMb)
        return HANTRO_NOK;
    s->stripCb = s->stripY + ysz;
    s->mb = s->stripMb;
    memset(s->stripMb, 0, rows * w * sizeof(mbStorage_t));
    return HANTRO_OK;
}

void h264bsdStripFree(storage_t *s)
{
    FREE(s->stripY);
    FREE(s->stripMb);
    s->stripCb = NULL;
    s->mb = NULL;
}

void h264bsdStripBegin(storage_t *s)
{
    u32 w = s->currImage->width;
    s->stripBase = 0;
    s->stripEmitted = 0;
    memset(s->stripMb, 0, ring_rows(s) * w * sizeof(mbStorage_t));
    rebase(s);
    neighbours(s);
}

/* Row `row` is about to start and does not fit: emit the K final rows, move
 * the two live rows to the front. */
H264BSD_COLD_FN void h264bsdStripRow(storage_t *s, u32 row)
{
    u32 w = s->currImage->width, k, r;
    u32 ylen = 2 * 16 * w * 16, clen = 2 * 8 * w * 8;
    u32 newBase = row - 2;
    k = newBase - s->stripBase;
    for (r = s->stripBase; r < newBase; r++)
        emit(s, r);
    memmove(s->stripY, s->stripY + k * 16 * w * 16, ylen);
    memmove(s->stripCb, s->stripCb + k * 8 * w * 8, clen);
    memmove(s->stripCb + ring_rows(s) * w * 64,
            s->stripCb + ring_rows(s) * w * 64 + k * 8 * w * 8, clen);
    memmove(s->stripMb, s->stripMb + k * w, 2 * w * sizeof(mbStorage_t));
    memset(s->stripMb + 2 * w, 0, (ring_rows(s) - 2) * w * sizeof(mbStorage_t));
    s->stripBase = newBase;
    rebase(s);
    neighbours(s);
}

/* The picture is complete and fully deblocked: emit the rows still held. */
H264BSD_COLD_FN void h264bsdStripFinish(storage_t *s)
{
    u32 r;
    for (r = s->stripEmitted; r < s->currImage->height; r++)
        emit(s, r);
}

/* OPT: see h264bsd_decoder.h */
u32 h264bsdSetStrip(storage_t *pStorage, u32 rowsPerShift,
    void (*fn)(void *ctx, u32 mbRow, const u8 *y, const u8 *cb, const u8 *cr,
               u32 yStride, u32 cStride), void *ctx)
{
    /* only before the first parameter sets are activated: that allocates
     * what each mode decodes into. Not with output reordering: a call would
     * return another picture than the one whose rows it handed out. Not
     * with a buffer gate: the DPB's pictures are stand-ins, nothing to gate.
     * Not with 0 rows, which would leave strip mode off */
    if (!rowsPerShift || pStorage->activeSps || pStorage->sliceGroupMap ||
        !pStorage->noReordering)
        return(HANTRO_NOK);
#if H264BSD_BUFFER_GATE
    if (pStorage->bufferGateFn)
        return(HANTRO_NOK);
#endif
    pStorage->stripMode = rowsPerShift < 255 ? rowsPerShift : 255;
    pStorage->stripRowFn = fn;
    pStorage->stripRowCtx = ctx;
#if H264BSD_LOWMEM
    pStorage->lmFlags = 0;   /* not both */
#endif
    return(HANTRO_OK);
}

#endif /* H264BSD_STRIP */
