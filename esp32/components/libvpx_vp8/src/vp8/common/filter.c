/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>
#include <stdint.h>
#include "./vp8_rtcd.h"
#include "vp8/common/filter.h"

DECLARE_ALIGNED(16, const short, vp8_bilinear_filters[8][2]) = {
  { 128, 0 }, { 112, 16 }, { 96, 32 }, { 80, 48 },
  { 64, 64 }, { 48, 80 },  { 32, 96 }, { 16, 112 }
};

DECLARE_ALIGNED(16, const short, vp8_sub_pel_filters[8][6]) = {

  { 0, 0, 128, 0, 0,
    0 }, /* note that 1/8 pel positions are just as per alpha -0.5 bicubic */
  { 0, -6, 123, 12, -1, 0 },
  { 2, -11, 108, 36, -8, 1 }, /* New 1/4 pel 6 tap filter */
  { 0, -9, 93, 50, -6, 0 },
  { 3, -16, 77, 77, -16, 3 }, /* New 1/2 pel 6 tap filter */
  { 0, -6, 50, 93, -9, 0 },
  { 1, -8, 36, 108, -11, 2 }, /* New 1/4 pel 6 tap filter */
  { 0, -1, 12, 123, -6, 0 },
};


/* Muse gadget port (bit-exact with libvpx's C): a zero offset is the
 * identity filter ({0,0,128,0,0,0} / {128,0} round back to the pixel), so a
 * block with one whole-pixel component takes one pass instead of two; the
 * 2-D case keeps the first pass in bytes (it is clamped to 0..255 anyway);
 * the odd sixtap positions have zero outer taps and use four. */

static inline unsigned char clamp255(int v) {
  return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* 6-tap along step (1: horizontal, stride: vertical), w x h, into dst */
static void sixtap_1d(const unsigned char *src, int src_stride, int step,
                      unsigned char *dst, int dst_stride, int w, int h,
                      const short *f) {
  const int f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
  int r, c;
  if (f[0] == 0 && f[5] == 0) {
    for (r = 0; r < h; ++r) {
      for (c = 0; c < w; ++c) {
        const unsigned char *p = src + c;
        int t = p[-step] * f1 + p[0] * f2 + p[step] * f3 + p[2 * step] * f4 + 64;
        dst[c] = clamp255(t >> 7);
      }
      src += src_stride;
      dst += dst_stride;
    }
  } else {
    const int f0 = f[0], f5 = f[5];
    for (r = 0; r < h; ++r) {
      for (c = 0; c < w; ++c) {
        const unsigned char *p = src + c;
        int t = p[-2 * step] * f0 + p[-step] * f1 + p[0] * f2 + p[step] * f3 +
                p[2 * step] * f4 + p[3 * step] * f5 + 64;
        dst[c] = clamp255(t >> 7);
      }
      src += src_stride;
      dst += dst_stride;
    }
  }
}

static void sixtap_predict(unsigned char *src, int src_stride, int xoffset,
                           int yoffset, unsigned char *dst, int dst_stride,
                           int w, int h) {
  if (!yoffset) {
    sixtap_1d(src, src_stride, 1, dst, dst_stride, w, h,
              vp8_sub_pel_filters[xoffset]);
  } else if (!xoffset) {
    sixtap_1d(src, src_stride, src_stride, dst, dst_stride, w, h,
              vp8_sub_pel_filters[yoffset]);
  } else {
    unsigned char tmp[21 * 16];
    sixtap_1d(src - 2 * src_stride, src_stride, 1, tmp, w, w, h + 5,
              vp8_sub_pel_filters[xoffset]);
    sixtap_1d(tmp + 2 * w, w, w, dst, dst_stride, w, h,
              vp8_sub_pel_filters[yoffset]);
  }
}

void vp8_sixtap_predict4x4_c(unsigned char *src_ptr, int src_pixels_per_line,
                             int xoffset, int yoffset, unsigned char *dst_ptr,
                             int dst_pitch) {
  sixtap_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
                 dst_pitch, 4, 4);
}

void vp8_sixtap_predict8x8_c(unsigned char *src_ptr, int src_pixels_per_line,
                             int xoffset, int yoffset, unsigned char *dst_ptr,
                             int dst_pitch) {
  sixtap_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
                 dst_pitch, 8, 8);
}

void vp8_sixtap_predict8x4_c(unsigned char *src_ptr, int src_pixels_per_line,
                             int xoffset, int yoffset, unsigned char *dst_ptr,
                             int dst_pitch) {
  sixtap_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
                 dst_pitch, 8, 4);
}

void vp8_sixtap_predict16x16_c(unsigned char *src_ptr, int src_pixels_per_line,
                               int xoffset, int yoffset, unsigned char *dst_ptr,
                               int dst_pitch) {
  sixtap_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
                 dst_pitch, 16, 16);
}

/* Bilinear: two pixels per 32-bit multiply (a*w0 + b*w1 + 64 < 2^16 per
 * lane), the same rounding as libvpx's two passes. */
static inline uint32_t pair(const unsigned char *p, int step) {
  return (uint32_t)p[0] | ((uint32_t)p[step] << 16);
}

static inline __attribute__((always_inline)) void bil_1d(
    const unsigned char *src, int src_stride, int step, unsigned char *dst,
    int dst_stride, int w, int h, int k) {
  const uint32_t w0 = 128 - 16 * k, w1 = 16 * k;
  int r, c;
  for (r = 0; r < h; ++r) {
    for (c = 0; c < w; c += 2) {
      const unsigned char *p = src + c;
      uint32_t v = (pair(p, 1) * w0 + pair(p + step, 1) * w1 + 0x00400040u) >> 7;
      dst[c] = (unsigned char)v;
      dst[c + 1] = (unsigned char)(v >> 16);
    }
    src += src_stride;
    dst += dst_stride;
  }
}

#if defined(MGX_PIE_S3)
/* ESP32-S3: 16- and 8-wide square blocks on its SIMD (xtensa/bilinear_pie.S,
 * the same arithmetic as bil_1d). Weights w0, w1, then 64 and 1, eight lanes
 * each, per offset. */
void vp8_mg_bil16_pie(const unsigned char *src, int src_stride,
                      unsigned char *dst, int dst_stride, int rows,
                      const short *tab, int step);
void vp8_mg_bil8_pie(const unsigned char *src, int src_stride,
                     unsigned char *dst, int dst_stride, int rows,
                     const short *tab, int step);
#define T8(k) \
  { 128 - 16 * (k), 128 - 16 * (k), 128 - 16 * (k), 128 - 16 * (k), \
    128 - 16 * (k), 128 - 16 * (k), 128 - 16 * (k), 128 - 16 * (k), \
    16 * (k), 16 * (k), 16 * (k), 16 * (k), 16 * (k), 16 * (k), 16 * (k), \
    16 * (k), 64, 64, 64, 64, 64, 64, 64, 64, 1, 1, 1, 1, 1, 1, 1, 1 }
static const short pie_tab[8][32] __attribute__((aligned(16))) = {
  T8(0), T8(1), T8(2), T8(3), T8(4), T8(5), T8(6), T8(7)
};

static int bil_pie(unsigned char *src, int src_stride, int xoffset,
                   int yoffset, unsigned char *dst, int dst_stride, int w) {
  void (*f)(const unsigned char *, int, unsigned char *, int, int,
            const short *, int) = w == 16 ? vp8_mg_bil16_pie : vp8_mg_bil8_pie;
  if (((uintptr_t)dst | (unsigned)dst_stride) & (unsigned)(w - 1)) return 0;
  if (!yoffset) {
    f(src, src_stride, dst, dst_stride, w, pie_tab[xoffset], 1);
  } else if (!xoffset) {
    f(src, src_stride, dst, dst_stride, w, pie_tab[yoffset], src_stride);
  } else {
    unsigned char tmp[17 * 16] __attribute__((aligned(16)));
    f(src, src_stride, tmp, 16, w + 1, pie_tab[xoffset], 1);
    f(tmp, 16, dst, dst_stride, w, pie_tab[yoffset], 16);
  }
  return 1;
}
#endif

static inline __attribute__((always_inline)) void bil_predict(
    unsigned char *src, int src_stride, int xoffset, int yoffset,
    unsigned char *dst, int dst_stride, int w, int h) {
#if defined(MGX_PIE_S3)
  if (w == h && w >= 8 && bil_pie(src, src_stride, xoffset, yoffset, dst, dst_stride, w)) return;
#endif
  if (!yoffset) {
    bil_1d(src, src_stride, 1, dst, dst_stride, w, h, xoffset);
  } else if (!xoffset) {
    bil_1d(src, src_stride, src_stride, dst, dst_stride, w, h, yoffset);
  } else {
    unsigned char tmp[17 * 16];
    bil_1d(src, src_stride, 1, tmp, w, w, h + 1, xoffset);
    bil_1d(tmp, w, w, dst, dst_stride, w, h, yoffset);
  }
}

void vp8_bilinear_predict4x4_c(unsigned char *src_ptr, int src_pixels_per_line,
                               int xoffset, int yoffset, unsigned char *dst_ptr,
                               int dst_pitch) {
  assert((xoffset | yoffset) != 0);
  bil_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
              dst_pitch, 4, 4);
}

void vp8_bilinear_predict8x8_c(unsigned char *src_ptr, int src_pixels_per_line,
                               int xoffset, int yoffset, unsigned char *dst_ptr,
                               int dst_pitch) {
  assert((xoffset | yoffset) != 0);
  bil_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
              dst_pitch, 8, 8);
}

void vp8_bilinear_predict8x4_c(unsigned char *src_ptr, int src_pixels_per_line,
                               int xoffset, int yoffset, unsigned char *dst_ptr,
                               int dst_pitch) {
  assert((xoffset | yoffset) != 0);
  bil_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
              dst_pitch, 8, 4);
}

void vp8_bilinear_predict16x16_c(unsigned char *src_ptr,
                                 int src_pixels_per_line, int xoffset,
                                 int yoffset, unsigned char *dst_ptr,
                                 int dst_pitch) {
  assert((xoffset | yoffset) != 0);
  bil_predict(src_ptr, src_pixels_per_line, xoffset, yoffset, dst_ptr,
              dst_pitch, 16, 16);
}
