/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/* Muse gadget port: the small fills and copies the decoder does per row and
 * per block, without a memset/memcpy call each (a call and a byte loop on
 * the RISC-V cores, whose libc does not know the sizes are small). */
#ifndef VPX_VP8_COMMON_MGX_MEM_H_
#define VPX_VP8_COMMON_MGX_MEM_H_

#include <stdint.h>
#include <string.h>

/* n bytes of v at d (a picture border: d and n multiples of 4 there) */
static inline __attribute__((always_inline)) void mgx_fill(unsigned char *d,
                                                           int v, int n) {
  if (((uintptr_t)d | (unsigned)n) & 3) {
    memset(d, v, n);
  } else {
    const uint32_t w = (uint32_t)v * 0x01010101u;
    uint32_t *p = (uint32_t *)d;
    int i;
    for (i = 0; i < n; i += 4) *p++ = w;
  }
}

/* w x h block copy (w a multiple of 4): words when both sides allow */
static inline __attribute__((always_inline)) void mgx_copy(
    const unsigned char *s, int ss, unsigned char *d, int ds, int w, int h) {
  int r, c;
  if (((uintptr_t)s | (uintptr_t)d | (unsigned)ss | (unsigned)ds) & 3) {
    for (r = 0; r < h; ++r, s += ss, d += ds) {
      for (c = 0; c < w; ++c) d[c] = s[c];
    }
  } else {
    for (r = 0; r < h; ++r, s += ss, d += ds) {
      for (c = 0; c < w; c += 4) {
        *(uint32_t *)(d + c) = *(const uint32_t *)(s + c);
      }
    }
  }
}

#endif  // VPX_VP8_COMMON_MGX_MEM_H_
