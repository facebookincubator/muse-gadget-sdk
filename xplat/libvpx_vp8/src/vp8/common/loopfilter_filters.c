/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <stdlib.h>
#include "loopfilter.h"
#include "onyxc_int.h"

typedef unsigned char uc;

/* Muse gadget port: the libvpx C filters in int arithmetic, a pixel at a
 * time, leaving a pixel early when its mask is off or the filter rounds to
 * nothing. Bit-exact with libvpx. */
/* one rarely taken branch for the clamp (forward taken branches cost the
 * C6 four cycles) */
static inline int sclamp(int t) {
#if defined(__riscv_zbb)
  return t < -128 ? -128 : t > 127 ? 127 : t; /* min and max instructions */
#else
  if ((unsigned)(t + 128) > 255u) t = (t >> 31) ^ 127;
  return t;
#endif
}
static inline int iabs(int v) { return v < 0 ? -v : v; }

/* one pixel across an inner (block) edge; s points at q0, d is the step across */
static inline void filter_px(uc *s, int d, int blimit, int limit, int thresh) {
  const int p3 = s[-4 * d], p2 = s[-3 * d], p1 = s[-2 * d], p0 = s[-d];
  const int q0 = s[0], q1 = s[d], q2 = s[2 * d], q3 = s[3 * d];
  int fv, f1, f2, a, ps1, ps0, qs0, qs1, hev;
  if (iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 > blimit) return;
  if (iabs(p3 - p2) > limit || iabs(p2 - p1) > limit || iabs(p1 - p0) > limit ||
      iabs(q1 - q0) > limit || iabs(q2 - q1) > limit || iabs(q3 - q2) > limit)
    return;
  hev = iabs(p1 - p0) > thresh || iabs(q1 - q0) > thresh;
  ps1 = p1 - 128; ps0 = p0 - 128; qs0 = q0 - 128; qs1 = q1 - 128;
  fv = (hev ? sclamp(ps1 - qs1) : 0) + 3 * (qs0 - ps0);
  if ((unsigned)(fv + 3) <= 6u) return; /* rounds to no change */
  fv = sclamp(fv);
  f1 = sclamp(fv + 4) >> 3;
  f2 = sclamp(fv + 3) >> 3;
  s[0] = (uc)(sclamp(qs0 - f1) + 128);
  s[-d] = (uc)(sclamp(ps0 + f2) + 128);
  if (!hev) {
    a = (f1 + 1) >> 1;
    s[d] = (uc)(sclamp(qs1 - a) + 128);
    s[-2 * d] = (uc)(sclamp(ps1 + a) + 128);
  }
}

/* one pixel across a macroblock edge */
static inline void mbfilter_px(uc *s, int d, int blimit, int limit,
                               int thresh) {
  const int p3 = s[-4 * d], p2 = s[-3 * d], p1 = s[-2 * d], p0 = s[-d];
  const int q0 = s[0], q1 = s[d], q2 = s[2 * d], q3 = s[3 * d];
  int fv, f1, f2, u, w, ps2, ps1, ps0, qs0, qs1, qs2;
  if (iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 > blimit) return;
  if (iabs(p3 - p2) > limit || iabs(p2 - p1) > limit || iabs(p1 - p0) > limit ||
      iabs(q1 - q0) > limit || iabs(q2 - q1) > limit || iabs(q3 - q2) > limit)
    return;
  ps2 = p2 - 128; ps1 = p1 - 128; ps0 = p0 - 128;
  qs0 = q0 - 128; qs1 = q1 - 128; qs2 = q2 - 128;
  fv = sclamp(ps1 - qs1);
  fv = sclamp(fv + 3 * (qs0 - ps0));
  if (iabs(p1 - p0) > thresh || iabs(q1 - q0) > thresh) { /* high edge variance */
    f1 = sclamp(fv + 4) >> 3;
    f2 = sclamp(fv + 3) >> 3;
    s[0] = (uc)(sclamp(qs0 - f1) + 128);
    s[-d] = (uc)(sclamp(ps0 + f2) + 128);
    return;
  }
  w = fv;
  u = sclamp((63 + w * 27) >> 7);
  s[0] = (uc)(sclamp(qs0 - u) + 128);
  s[-d] = (uc)(sclamp(ps0 + u) + 128);
  u = sclamp((63 + w * 18) >> 7);
  s[d] = (uc)(sclamp(qs1 - u) + 128);
  s[-2 * d] = (uc)(sclamp(ps1 + u) + 128);
  u = sclamp((63 + w * 9) >> 7);
  s[2 * d] = (uc)(sclamp(qs2 - u) + 128);
  s[-3 * d] = (uc)(sclamp(ps2 + u) + 128);
}

static inline void simple_px(uc *s, int d, int blimit) {
  const int p1 = s[-2 * d], p0 = s[-d], q0 = s[0], q1 = s[d];
  const int dq = q0 - p0, e = p1 - q1;
  int fv, f1, f2;
  if (iabs(dq) * 2 + iabs(e) / 2 > blimit) return;
  fv = sclamp(e) + 3 * dq;
  /* |fv| <= 3 rounds to no change on either side */
  if (__builtin_expect((unsigned)(fv + 3) <= 6u, 1)) return;
  fv = sclamp(fv);
  f1 = sclamp(fv + 4) >> 3;
  f2 = sclamp(fv + 3) >> 3;
  s[0] = (uc)(sclamp(q0 - 128 - f1) + 128);
  s[-d] = (uc)(sclamp(p0 - 128 + f2) + 128);
}

static void loop_filter_horizontal_edge_c(unsigned char *s, int p,
                                          const unsigned char *blimit,
                                          const unsigned char *limit,
                                          const unsigned char *thresh,
                                          int count) {
  const int bl = blimit[0], l = limit[0], t = thresh[0];
  int i;
  for (i = 0; i < count * 8; ++i) filter_px(s + i, p, bl, l, t);
}

static void loop_filter_vertical_edge_c(unsigned char *s, int p,
                                        const unsigned char *blimit,
                                        const unsigned char *limit,
                                        const unsigned char *thresh,
                                        int count) {
  const int bl = blimit[0], l = limit[0], t = thresh[0];
  int i;
  for (i = 0; i < count * 8; ++i, s += p) filter_px(s, 1, bl, l, t);
}

static void mbloop_filter_horizontal_edge_c(unsigned char *s, int p,
                                            const unsigned char *blimit,
                                            const unsigned char *limit,
                                            const unsigned char *thresh,
                                            int count) {
  const int bl = blimit[0], l = limit[0], t = thresh[0];
  int i;
  for (i = 0; i < count * 8; ++i) mbfilter_px(s + i, p, bl, l, t);
}

static void mbloop_filter_vertical_edge_c(unsigned char *s, int p,
                                          const unsigned char *blimit,
                                          const unsigned char *limit,
                                          const unsigned char *thresh,
                                          int count) {
  const int bl = blimit[0], l = limit[0], t = thresh[0];
  int i;
  for (i = 0; i < count * 8; ++i, s += p) mbfilter_px(s, 1, bl, l, t);
}

void vp8_loop_filter_simple_horizontal_edge_c(unsigned char *y_ptr,
                                              int y_stride,
                                              const unsigned char *blimit) {
  const int bl = blimit[0];
  int i;
  for (i = 0; i < 16; ++i) simple_px(y_ptr + i, y_stride, bl);
}

void vp8_loop_filter_simple_vertical_edge_c(unsigned char *y_ptr, int y_stride,
                                            const unsigned char *blimit) {
  const int bl = blimit[0];
  int i;
  for (i = 0; i < 16; ++i, y_ptr += y_stride) simple_px(y_ptr, 1, bl);
}

void vp8_loop_filter_mbh_c(unsigned char *y_ptr, unsigned char *u_ptr,
                           unsigned char *v_ptr, int y_stride, int uv_stride,
                           loop_filter_info *lfi) {
  mbloop_filter_horizontal_edge_c(y_ptr, y_stride, lfi->mblim, lfi->lim,
                                  lfi->hev_thr, 2);

  if (u_ptr) {
    mbloop_filter_horizontal_edge_c(u_ptr, uv_stride, lfi->mblim, lfi->lim,
                                    lfi->hev_thr, 1);
  }

  if (v_ptr) {
    mbloop_filter_horizontal_edge_c(v_ptr, uv_stride, lfi->mblim, lfi->lim,
                                    lfi->hev_thr, 1);
  }
}

/* Vertical MB Filtering */
void vp8_loop_filter_mbv_c(unsigned char *y_ptr, unsigned char *u_ptr,
                           unsigned char *v_ptr, int y_stride, int uv_stride,
                           loop_filter_info *lfi) {
  mbloop_filter_vertical_edge_c(y_ptr, y_stride, lfi->mblim, lfi->lim,
                                lfi->hev_thr, 2);

  if (u_ptr) {
    mbloop_filter_vertical_edge_c(u_ptr, uv_stride, lfi->mblim, lfi->lim,
                                  lfi->hev_thr, 1);
  }

  if (v_ptr) {
    mbloop_filter_vertical_edge_c(v_ptr, uv_stride, lfi->mblim, lfi->lim,
                                  lfi->hev_thr, 1);
  }
}

/* Horizontal B Filtering */
void vp8_loop_filter_bh_c(unsigned char *y_ptr, unsigned char *u_ptr,
                          unsigned char *v_ptr, int y_stride, int uv_stride,
                          loop_filter_info *lfi) {
  loop_filter_horizontal_edge_c(y_ptr + 4 * y_stride, y_stride, lfi->blim,
                                lfi->lim, lfi->hev_thr, 2);
  loop_filter_horizontal_edge_c(y_ptr + 8 * y_stride, y_stride, lfi->blim,
                                lfi->lim, lfi->hev_thr, 2);
  loop_filter_horizontal_edge_c(y_ptr + 12 * y_stride, y_stride, lfi->blim,
                                lfi->lim, lfi->hev_thr, 2);

  if (u_ptr) {
    loop_filter_horizontal_edge_c(u_ptr + 4 * uv_stride, uv_stride, lfi->blim,
                                  lfi->lim, lfi->hev_thr, 1);
  }

  if (v_ptr) {
    loop_filter_horizontal_edge_c(v_ptr + 4 * uv_stride, uv_stride, lfi->blim,
                                  lfi->lim, lfi->hev_thr, 1);
  }
}

void vp8_loop_filter_bhs_c(unsigned char *y_ptr, int y_stride,
                           const unsigned char *blimit) {
  vp8_loop_filter_simple_horizontal_edge_c(y_ptr + 4 * y_stride, y_stride,
                                           blimit);
  vp8_loop_filter_simple_horizontal_edge_c(y_ptr + 8 * y_stride, y_stride,
                                           blimit);
  vp8_loop_filter_simple_horizontal_edge_c(y_ptr + 12 * y_stride, y_stride,
                                           blimit);
}

/* Vertical B Filtering */
void vp8_loop_filter_bv_c(unsigned char *y_ptr, unsigned char *u_ptr,
                          unsigned char *v_ptr, int y_stride, int uv_stride,
                          loop_filter_info *lfi) {
  loop_filter_vertical_edge_c(y_ptr + 4, y_stride, lfi->blim, lfi->lim,
                              lfi->hev_thr, 2);
  loop_filter_vertical_edge_c(y_ptr + 8, y_stride, lfi->blim, lfi->lim,
                              lfi->hev_thr, 2);
  loop_filter_vertical_edge_c(y_ptr + 12, y_stride, lfi->blim, lfi->lim,
                              lfi->hev_thr, 2);

  if (u_ptr) {
    loop_filter_vertical_edge_c(u_ptr + 4, uv_stride, lfi->blim, lfi->lim,
                                lfi->hev_thr, 1);
  }

  if (v_ptr) {
    loop_filter_vertical_edge_c(v_ptr + 4, uv_stride, lfi->blim, lfi->lim,
                                lfi->hev_thr, 1);
  }
}

void vp8_loop_filter_bvs_c(unsigned char *y_ptr, int y_stride,
                           const unsigned char *blimit) {
  vp8_loop_filter_simple_vertical_edge_c(y_ptr + 4, y_stride, blimit);
  vp8_loop_filter_simple_vertical_edge_c(y_ptr + 8, y_stride, blimit);
  vp8_loop_filter_simple_vertical_edge_c(y_ptr + 12, y_stride, blimit);
}
