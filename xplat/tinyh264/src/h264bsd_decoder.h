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

#ifndef H264SWDEC_DECODER_H
#define H264SWDEC_DECODER_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"

#include "h264bsd_storage.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

/* enumerated return values of the functions */
enum {
    H264BSD_RDY,
    H264BSD_PIC_RDY,
    H264BSD_HDRS_RDY,
    H264BSD_ERROR,
    H264BSD_PARAM_SET_ERROR,
    H264BSD_MEMALLOC_ERROR
};

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/



/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

u32 h264bsdInit(storage_t *pStorage, u32 noOutputReordering);
u32 h264bsdDecode(storage_t *pStorage, u8 *byteStrm, u32 len, u8 **picture, u32 *width, u32 *height);
void h264bsdShutdown(storage_t *pStorage);
/* OPT: fn(ctx, picture, widthMbs, heightMbs, rows) whenever more of the
 * picture being decoded is final (decoded and deblocked), from inside
 * h264bsdDecode; rows counts luma rows from the top, reaching the full height
 * when the picture is done. The picture pointer is the one h264bsdDecode
 * returns for it (no output reordering). NULL: off. */
void h264bsdSetProgress(storage_t *pStorage,
    void (*fn)(void *ctx, const u8 *pic, u32 widthMbs, u32 heightMbs, u32 rowsFinal), void *ctx);

/* OPT: refuse sequence parameter sets for pictures wider or taller than
 * this (macroblocks), before anything is allocated for them: a stream can't
 * ask for more memory than its caller planned. Call after h264bsdInit. */
/* Zero keeps the default. Set before the first parameter set. */
void h264bsdSetMaxReferences(storage_t *pStorage, u32 maxRefs);
#if H264BSD_LIMITS
/* OPT: further limits on the sequence parameter sets the decoder takes (0:
 * none): at most maxMbs macroblocks a picture and maxRefFrames reference
 * frames (h264bsdSetMaxReferences' limit), and with output reordering on
 * (h264bsdInit(dec, 0)) at most maxRefFrames frames in the DPB as well
 * (a stream with neither picture order count type 2 nor num_reorder_frames
 * 0 asks for as many as its level allows). A larger one is refused before
 * anything is allocated for it, so what the decoder allocates for a
 * sequence (its macroblock entries, slice group map and at most
 * maxRefFrames + 1 pictures of maxMbs macroblocks) is bounded, for a fixed
 * arena (TINYH264_CUSTOM_ALLOC). A refused one is an error from
 * h264bsdDecode (the rest of that access unit isn't decoded) and drops the
 * set stored with its id, deactivated if active, so the stream needs a new
 * sequence parameter set and an IDR picture. One the kept parameter sets
 * refuse anyway (h264bsdKeepParamSets, h264bsdAllowParamSetChange) is only
 * refused, as any other: the kept ones decode on. The parameter sets the
 * decoder holds aren't bounded: up to 32
 * sequence and 256 picture parameter sets, and one with explicit slice
 * group ids holds a 32-bit id for every map unit it declares. Call after
 * h264bsdInit. */
void h264bsdSetLimits(storage_t *pStorage, u32 maxMbs, u32 maxRefFrames);
#endif
#if H264BSD_BUFFER_GATE
/* OPT: fn(ctx, pic) is called with the picture buffer (its luma) each new
 * picture is about to be decoded into, before any pixel of it is written,
 * and with every picture buffer before it is freed (another sequence
 * parameter set activated, h264bsdShutdown); it returns once nothing else
 * (a display reading pictures in place) still reads that buffer. In place
 * (H264BSD_LOWMEM_INPLACE) pic is the start of the one buffer every
 * picture shares. NULL: off. HANTRO_OK, or HANTRO_NOK (nothing changed)
 * with strip mode on (h264bsdSetStrip). */
u32 h264bsdSetBufferGate(storage_t *pStorage,
    void (*fn)(void *ctx, const u8 *pic), void *ctx);
#endif
void h264bsdSetMaxSize(storage_t *pStorage, u32 maxWidthMbs, u32 maxHeightMbs);
/* OPT: and refuse them when their picture after frame cropping (the
 * sequence parameter set's frame_crop offsets) is wider or taller than this
 * (pixels; 0: no limit). The coded size may be larger than the cropped one
 * (an encoder's padding: iPhone streams code at least 192 x 96), within
 * h264bsdSetMaxSize. Call after h264bsdInit. */
void h264bsdSetMaxCropped(storage_t *pStorage, u32 maxWidth, u32 maxHeight);
/* OPT: the active sequence parameter set's frame_crop_left, right, top and
 * bottom offsets, as coded: in crop units (2 x 2 luma pixels for the 4:2:0
 * frames this decoder takes); 0 without frame_cropping_flag. HANTRO_OK, or
 * HANTRO_NOK with no active sequence parameter set (all 0). */
u32 h264bsdCroppingOffsets(const storage_t *pStorage, u32 *left, u32 *right, u32 *top, u32 *bottom);

/* OPT: error recovery (../README.md). h264bsdDecode leaves a picture it
 * couldn't finish (an error, or slices missing) started; the next call would
 * conceal it at its first NAL unit and return that picture in place of the
 * one it was given, which then goes undecoded. A caller that hands over whole
 * access units abandons it instead: after a call that didn't return a
 * picture, h264bsdPictureStarted says one is left, and h264bsdAbandonPicture
 * drops it (nothing of it is shown or kept as a reference), so the next
 * access unit decodes on its own. */
u32 h264bsdPictureStarted(const storage_t *pStorage);
void h264bsdAbandonPicture(storage_t *pStorage);
/* OPT: after an IDR picture decoded, keep the active parameter sets and lock
 * the stream's size and profile (sequence parameter sets for another are
 * refused from then on); after an error, put the kept ones back, in case
 * damaged bytes replaced them. HANTRO_OK, or MEMORY_ALLOCATION_ERROR (nothing
 * changed). */
u32 h264bsdKeepParamSets(storage_t *pStorage);
u32 h264bsdRestoreParamSets(storage_t *pStorage);
/* OPT: once parameter sets are kept, one that differs from the kept one with
 * its id is taken only while allowed (another size or profile never is): set
 * it for a keyframe's access unit (where the stream's own come), clear it for
 * the rest. */
void h264bsdAllowParamSetChange(storage_t *pStorage, u32 allow);

#if H264BSD_LOWMEM
/* OPT: low-memory mode, for boards without PSRAM (h264bsd_lowmem.c). Call
 * right after h264bsdInit, before the first NAL unit (later calls do
 * nothing).
 *   H264BSD_LOWMEM_MBRING (1): macroblock entries for three macroblock rows
 *     instead of the whole picture.
 *   H264BSD_LOWMEM_INPLACE (2, implies MBRING): with one reference frame and
 *     no output reordering, the reference and the picture being decoded
 *     share one buffer, D luma rows taller than a picture. Bits 8-15: D / 8
 *     (0: from the stream's motion vector limit, at most the picture
 *     height). A picture whose motion reaches further up than D allows is
 *     reported as an error, never decoded wrong. The picture h264bsdDecode
 *     returns stays valid only until the next h264bsdDecode call, and its
 *     chroma planes don't follow its luma (h264bsdPicturePlanes).
 * In this mode a damaged picture (or one using arbitrary slice order, slice
 * groups, redundant slices or frame number gaps) is an error and is dropped
 * instead of concealed; in place, once a damaged picture, or a reference
 * picture that reference marking can't store, has overwritten part of the
 * reference, the decoder waits for an IDR or all-intra reference picture
 * (the pictures skipped while it waits don't count as a frame_num gap). With
 * the macroblock ring alone (or in place with D at least the picture
 * height, where nothing is overwritten) it doesn't wait: a dropped reference
 * picture leaves the one before it as the reference, and the P pictures
 * after it, coded against the dropped one, are decoded against that older
 * one, wrong but not reported as errors, until the next IDR or intra
 * picture (the shipped decoder drifts the same way after a picture it
 * conceals or its caller abandons). */
void h264bsdSetLowMem(storage_t *pStorage, u32 flags);
/* in-place D in use (0: two pictures), pictures refused so far */
void h264bsdLowMemStats(storage_t *pStorage, u32 *inPlaceRows, u32 *refused);
#if H264BSD_ONEREF
/* OPT: H264BSD_ONEREF: in place (H264BSD_LOWMEM_INPLACE), a sequence
 * parameter set allowing several reference frames decodes keeping one (an
 * encoder that declares several and predicts from the last picture only);
 * a slice that needs another (more than one active, reordering, long-term
 * or adaptive marking) is an error, and the decoder waits for an IDR
 * picture. Non-reference pictures, which in place would overwrite the
 * reference, are skipped: an error from h264bsdDecode that leaves the
 * reference and the last picture as they were (nonRefsSkipped counts them;
 * a caller tells such an error from a real one by it growing). Returns the
 * active SPS's max_num_ref_frames (0: none active). */
u32 h264bsdRefInfo(storage_t *pStorage, u32 *nonRefsSkipped);
#endif
#endif
#if H264BSD_STRIP
/* OPT: strip mode, for intra-only streams on boards that can't hold a
 * picture (h264bsd_strip.c): decode into a ring of rowsPerShift + 2
 * macroblock rows (rowsPerShift 1 to 255; more counts as 255) instead of
 * whole pictures; fn gets every macroblock row (16 luma lines, 8 chroma)
 * once it is final, in order, from inside h264bsdDecode. With
 * H264BSD_PIC_RDY, h264bsdDecode returns the picture's width and height and
 * a NULL picture pointer; progress (h264bsdSetProgress) isn't reported.
 * P slices, redundant slices and macroblocks out of raster order are an
 * error and the picture is dropped, nothing concealed. Call right after
 * h264bsdInit(dec, 1), before the first NAL unit; not with h264bsdSetLowMem
 * (the later call wins). HANTRO_OK, or HANTRO_NOK (nothing changed) for 0
 * rows, once a slice has activated parameter sets, with output reordering
 * on (h264bsdInit(dec, 0)) or with a buffer gate set (h264bsdSetBufferGate). */
u32 h264bsdSetStrip(storage_t *pStorage, u32 rowsPerShift,
    void (*fn)(void *ctx, u32 mbRow, const u8 *y, const u8 *cb, const u8 *cr,
               u32 yStride, u32 cStride), void *ctx);
#endif
#if H264BSD_SPLIT_PLANES
/* OPT: the chroma planes of the picture whose luma h264bsdDecode returned */
void h264bsdPicturePlanes(storage_t *pStorage, const u8 *luma, u8 **cb, u8 **cr);
#endif

#if H264BSD_CHANGED_MAP
/* OPT: which macroblocks of the picture the last h264bsdDecode call
 * returned changed, for a display that redraws only those (valid until the
 * next call). Bit 0 clear: no map, the whole picture changed (an IDR, the
 * first picture, one after an error, a frame_num gap, a non-reference or
 * long-term picture, a concealed or partly decoded one, reordered
 * references, output reordering on (h264bsdInit without noOutputReordering
 * and a sequence parameter set that reorders), a picture of more than
 * H264BSD_CHANGED_MAP_MBS macroblocks).
 * Bit 0 set: *clean points at one bit per macroblock in raster order (bit
 * i % 8 of byte i / 8): macroblock i carries the previous output picture's
 * pixels before the loop filter (inter predicted from reference index 0
 * with every motion vector zero, no residual sample and, where its slice
 * deblocks, no luma coefficients, which filter its inner edges); bit 1 is set
 * when a slice deblocked: a clean macroblock beside a changed one may then
 * differ in the 3 luma (1 chroma) rows / columns along their shared edge.
 * Bit 2, with bit 0 clear: an IDR picture. */
u32 h264bsdChangedMap(storage_t *pStorage, const u8 **clean);
/* OPT: the next pictures build no map (1) or do (0) */
void h264bsdChangedMapSkip(storage_t *pStorage, u32 skip);
#endif

storage_t* h264bsdAlloc();
void h264bsdFree(storage_t *pStorage);

#endif /* #ifdef H264SWDEC_DECODER_H */

