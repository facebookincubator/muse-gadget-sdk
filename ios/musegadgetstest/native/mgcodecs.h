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
#pragma once

/*
 * The app's C side, called from Swift (Bridging.h): the software encoders,
 * each built only from your own checkout (native/build_x264.sh,
 * native/build_vpx.sh; neither library is in this repository), and the
 * panel scaler's inner loops. Without a library its calls are stubs and
 * *_available() is 0, as on Android when the APK has no libmgx264/libmgvpx.
 *
 * Encoders: one I420 picture in (w*h luma, then the quarter planes), one
 * access unit (x264: Annex B) or VP8 frame out. The returned pointer stays
 * valid until the next call on the same encoder. info[0]: keyframe,
 * info[1]: quantizer.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libx264 (GPL-2.0: an app built with it is under the GPL). params:
 * "name=value:..." in x264's own option names, plus vbv-ms. NULL i420
 * flushes one frame the lookahead holds (clips). */
int mgx264_available(void);
void *mgx264_open(int w, int h, int fps, int kbps, const char *preset, const char *params);
int mgx264_encode(void *enc, const uint8_t *i420, int64_t pts, int idr, const uint8_t **out, int info[2]);
void mgx264_set_bitrate(void *enc, int kbps);
void mgx264_close(void *enc);

/* libvpx's VP8 encoder (BSD-3-Clause). params as Vpx.java: cpu, deadline,
 * profile, lf, mvcheap, threads, vbv-ms, vbr. */
int mgvpx_available(void);
void *mgvpx_open(int w, int h, int fps, int kbps, const char *params);
int mgvpx_encode(void *enc, const uint8_t *i420, int64_t pts, int key, const uint8_t **out, int info[2]);
void mgvpx_set_bitrate(void *enc, int kbps);
void mgvpx_close(void *enc);

/* PanelScaler's separable filter (PanelScaler.swift builds the taps): the
 * nIn x nIn square at (x0, y0) of src (rowStride, pixStride) to nOut x nOut
 * in dst. taps: nOut x len Q12 weights from start[o]. tmp: nIn * nOut ints. */
void mg_panel_scale(const uint8_t *src, int rowStride, int pixStride, int x0, int y0, int nIn, int nOut,
                    const int32_t *start, const int32_t *taps, int len, int32_t *tmp, uint8_t *dst);

#ifdef __cplusplus
}
#endif
