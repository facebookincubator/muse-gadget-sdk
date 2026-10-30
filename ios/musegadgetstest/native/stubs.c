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
/* Without a software encoder (no LIB_DIR/libmgx264.a or libmgvpx.a): the
 * app uses the phone's H.264 encoder, and VP8 isn't offered. build.sh
 * compiles each half only when that library is missing. */
#include <stddef.h>

#include "mgcodecs.h"

#ifdef MG_STUB_X264
int mgx264_available(void) { return 0; }
void *mgx264_open(int w, int h, int fps, int kbps, const char *preset, const char *params)
{
    (void)w; (void)h; (void)fps; (void)kbps; (void)preset; (void)params;
    return NULL;
}
int mgx264_encode(void *enc, const uint8_t *i420, int64_t pts, int idr, const uint8_t **out, int info[2])
{
    (void)enc; (void)i420; (void)pts; (void)idr; (void)out; (void)info;
    return 0;
}
void mgx264_set_bitrate(void *enc, int kbps) { (void)enc; (void)kbps; }
void mgx264_close(void *enc) { (void)enc; }
#endif

#ifdef MG_STUB_VPX
int mgvpx_available(void) { return 0; }
void *mgvpx_open(int w, int h, int fps, int kbps, const char *params)
{
    (void)w; (void)h; (void)fps; (void)kbps; (void)params;
    return NULL;
}
int mgvpx_encode(void *enc, const uint8_t *i420, int64_t pts, int key, const uint8_t **out, int info[2])
{
    (void)enc; (void)i420; (void)pts; (void)key; (void)out; (void)info;
    return 0;
}
void mgvpx_set_bitrate(void *enc, int kbps) { (void)enc; (void)kbps; }
void mgvpx_close(void *enc) { (void)enc; }
#endif
