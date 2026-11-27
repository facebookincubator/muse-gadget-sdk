/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "vpx_config.h"
#include "vp8_rtcd.h"
#include "vpx_mem/vpx_mem.h"

static inline unsigned char clamp255(int a) {
  if ((unsigned)a > 255u) a = ~a >> 31 & 255; /* rarely taken */
  return (unsigned char)a;
}

/* Muse gadget port: a DC-only block whose DC rounds to zero adds nothing
 * ((dc + 4) >> 3 == 0), so it is skipped; bit-exact with libvpx. */
static inline void dc_add(int dc, unsigned char *dst, int stride) {
  int a1 = (dc + 4) >> 3, r;
  if (!a1) return;
  for (r = 0; r < 4; ++r, dst += stride) {
    int a;
    a = dst[0] + a1; dst[0] = clamp255(a);
    a = dst[1] + a1; dst[1] = clamp255(a);
    a = dst[2] + a1; dst[2] = clamp255(a);
    a = dst[3] + a1; dst[3] = clamp255(a);
  }
}

static inline __attribute__((always_inline)) void blk(short *q, short *dq, unsigned char *dst, int stride,
                       char eob) {
  if (eob > 1) {
    vp8_dequant_idct_add_c(q, dq, dst, stride);
  } else if (q[0]) {
    dc_add(q[0] * dq[0], dst, stride);
    q[0] = 0;
  }
  q[1] = 0;
}

void vp8_dequant_idct_add_y_block_c(short *q, short *dq, unsigned char *dst,
                                    int stride, char *eobs) {
  int i, j;
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      blk(q, dq, dst, stride, *eobs++);
      q += 16;
      dst += 4;
    }
    dst += 4 * stride - 16;
  }
}

void vp8_dequant_idct_add_uv_block_c(short *q, short *dq, unsigned char *dst_u,
                                     unsigned char *dst_v, int stride,
                                     char *eobs) {
  int i, j;
  for (i = 0; i < 2; ++i) {
    for (j = 0; j < 2; ++j) {
      blk(q, dq, dst_u, stride, *eobs++);
      q += 16;
      dst_u += 4;
    }
    dst_u += 4 * stride - 8;
  }
  for (i = 0; i < 2; ++i) {
    for (j = 0; j < 2; ++j) {
      blk(q, dq, dst_v, stride, *eobs++);
      q += 16;
      dst_v += 4;
    }
    dst_v += 4 * stride - 8;
  }
}
