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

/* Loop unrolling and per-width copies of the 6-tap filter make it faster,
 * and bigger: VP8_MGX_COMPACT (CONFIG_MUSE_VP8_IRAM_COMPACT) keeps the
 * algorithm (same arithmetic) without them, for builds that keep this code
 * in IRAM. */
#ifndef VP8_MGX_COMPACT
#define VP8_MGX_COMPACT 0
#endif
#if VP8_MGX_COMPACT
#define MGX_UNROLL(n)
#else
#define MGX_PRAGMA(x) _Pragma(#x)
#define MGX_UNROLL(n) MGX_PRAGMA(GCC unroll n)
#endif

static inline unsigned char clamp255(int v) {
  if ((unsigned)v > 255u) v = ~v >> 31 & 255; /* rarely taken */
  return (unsigned char)v;
}

/* 6-tap along step (1: horizontal, stride: vertical), w x h, into dst.
 * Sliding window: one new source pixel per output pixel, along the row
 * (horizontal) or down the column (vertical); the same sums. Inlined per
 * block size, so the inner loops unroll. */
static inline __attribute__((always_inline)) void sixtap_1d(
    const unsigned char *src, int src_stride, int step, unsigned char *dst,
    int dst_stride, int w, int h, const short *f) {
  const int f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4], f5 = f[5];
  int r, c;
  if (step == 1) {
    if (f0 == 0 && f5 == 0) {
      for (r = 0; r < h; ++r, src += src_stride, dst += dst_stride) {
        int a1 = src[-1], a2 = src[0], a3 = src[1];
MGX_UNROLL(16)
        for (c = 0; c < w; ++c) {
          const int a4 = src[c + 2];
          dst[c] = clamp255((a1 * f1 + a2 * f2 + a3 * f3 + a4 * f4 + 64) >> 7);
          a1 = a2; a2 = a3; a3 = a4;
        }
      }
    } else {
      for (r = 0; r < h; ++r, src += src_stride, dst += dst_stride) {
        int a0 = src[-2], a1 = src[-1], a2 = src[0], a3 = src[1], a4 = src[2];
MGX_UNROLL(16)
        for (c = 0; c < w; ++c) {
          const int a5 = src[c + 3];
          dst[c] = clamp255((a0 * f0 + a1 * f1 + a2 * f2 + a3 * f3 + a4 * f4 +
                             a5 * f5 + 64) >> 7);
          a0 = a1; a1 = a2; a2 = a3; a3 = a4; a4 = a5;
        }
      }
    }
  } else {
    for (c = 0; c < w; ++c) {
      const unsigned char *p = src + c;
      unsigned char *d = dst + c;
      if (f0 == 0 && f5 == 0) {
        int a1 = p[-step], a2 = p[0], a3 = p[step];
        p += 2 * step;
MGX_UNROLL(4)
        for (r = 0; r < h; ++r, p += step, d += dst_stride) {
          const int a4 = p[0];
          *d = clamp255((a1 * f1 + a2 * f2 + a3 * f3 + a4 * f4 + 64) >> 7);
          a1 = a2; a2 = a3; a3 = a4;
        }
      } else {
        int a0 = p[-2 * step], a1 = p[-step], a2 = p[0], a3 = p[step],
            a4 = p[2 * step];
        p += 3 * step;
MGX_UNROLL(4)
        for (r = 0; r < h; ++r, p += step, d += dst_stride) {
          const int a5 = p[0];
          *d = clamp255((a0 * f0 + a1 * f1 + a2 * f2 + a3 * f3 + a4 * f4 +
                         a5 * f5 + 64) >> 7);
          a0 = a1; a1 = a2; a2 = a3; a3 = a4; a4 = a5;
        }
      }
    }
  }
}

/* One out-of-line copy per block width, each with its inner loop unrolled
 * (one copy for all widths when compact). */
static void __attribute__((noinline)) sixtap_1d_w(
    const unsigned char *src, int src_stride, int step, unsigned char *dst,
    int dst_stride, int w, int h, const short *f) {
#if VP8_MGX_COMPACT
  sixtap_1d(src, src_stride, step, dst, dst_stride, w, h, f);
#else
  if (w == 16) sixtap_1d(src, src_stride, step, dst, dst_stride, 16, h, f);
  else if (w == 8) sixtap_1d(src, src_stride, step, dst, dst_stride, 8, h, f);
  else sixtap_1d(src, src_stride, step, dst, dst_stride, 4, h, f);
#endif
}

static void sixtap_predict(unsigned char *src, int src_stride, int xoffset,
                           int yoffset, unsigned char *dst, int dst_stride,
                           int w, int h) {
  if (!yoffset) {
    sixtap_1d_w(src, src_stride, 1, dst, dst_stride, w, h,
                vp8_sub_pel_filters[xoffset]);
  } else if (!xoffset) {
    sixtap_1d_w(src, src_stride, src_stride, dst, dst_stride, w, h,
                vp8_sub_pel_filters[yoffset]);
  } else {
    /* An odd yoffset has zero outer taps: the second pass reads rows -1 to
     * h + 1 only, so the first pass skips rows -2 and h + 2. */
    unsigned char tmp[21 * 16];
    const int y4 = yoffset & 1;
    sixtap_1d_w(src - (2 - y4) * src_stride, src_stride, 1, tmp + y4 * w, w,
                w, h + 5 - 2 * y4, vp8_sub_pel_filters[xoffset]);
    sixtap_1d_w(tmp + 2 * w, w, w, dst, dst_stride, w, h,
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
  if (step == 1) {
    /* each source pixel loaded once: the right pixel of a pair is the
     * left one of the next */
    for (r = 0; r < h; ++r) {
      uint32_t a = src[0];
MGX_UNROLL(16)
      for (c = 0; c < w; c += 2) {
        const uint32_t b = src[c + 1], n = src[c + 2];
        const uint32_t v = ((a | b << 16) * w0 + (b | n << 16) * w1 + 0x00400040u) >> 7;
        dst[c] = (unsigned char)v;
        dst[c + 1] = (unsigned char)(v >> 16);
        a = n;
      }
      src += src_stride;
      dst += dst_stride;
    }
  } else {
    /* down each pair of columns: a row's pair is the next row's top */
    for (c = 0; c < w; c += 2) {
      const unsigned char *p = src + c;
      unsigned char *d = dst + c;
      uint32_t t = pair(p, 1);
MGX_UNROLL(4)
      for (r = 0; r < h; ++r) {
        const uint32_t u = pair(p += step, 1);
        const uint32_t v = (t * w0 + u * w1 + 0x00400040u) >> 7;
        d[0] = (unsigned char)v;
        d[1] = (unsigned char)(v >> 16);
        d += dst_stride;
        t = u;
      }
    }
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
