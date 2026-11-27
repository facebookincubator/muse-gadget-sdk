/*
 *  Copyright (c) 2011 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef VPX_VP8_COMMON_FILTER_H_
#define VPX_VP8_COMMON_FILTER_H_

#include "vpx_ports/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCK_HEIGHT_WIDTH 4
#define VP8_FILTER_WEIGHT 128
#define VP8_FILTER_SHIFT 7

/* Muse gadget port: the filters' 0..255 clamp (filter.c). VPX_XPULP=1, on
 * RISC-V cores with Xpulp: one clipu (xplat/include/rv_xpulp.h; build with
 * -DRV_XPULP=1 and xplat/include on the include path, or its plain-C twins
 * run, with the same results). Here, not in filter.c, so that file's line
 * numbers (its asserts) stay as they were. */
#if defined(VPX_XPULP) && VPX_XPULP
#include "rv_xpulp.h"
#define VP8_CLAMP255(v) ((unsigned char)rv_xpulp_clipu8(v))
#else
static inline unsigned char vp8_clamp255_c(int v) {
  if ((unsigned)v > 255u) v = ~v >> 31 & 255; /* rarely taken */
  return (unsigned char)v;
}
#define VP8_CLAMP255(v) vp8_clamp255_c(v)
#endif

extern DECLARE_ALIGNED(16, const short, vp8_bilinear_filters[8][2]);
extern DECLARE_ALIGNED(16, const short, vp8_sub_pel_filters[8][6]);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VPX_VP8_COMMON_FILTER_H_
