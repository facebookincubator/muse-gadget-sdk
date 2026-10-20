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
 * libvpx's VP8 encoder for Muse Gadgets Test on iOS (the same settings as
 * Android's jni/mgvpx.c): one I420 picture in, one VP8 frame out, no lag.
 * Built by native/build_vpx.sh against your libvpx checkout, with
 * android/musegadgetstest/jni/libvpx_mg.py's two encoder switches.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgcodecs.h"
#include "vpx/vp8cx.h"
#include "vpx/vpx_encoder.h"

extern int vp8_mg_lf_max;    /* -1: libvpx picks the loop filter level; else at most this (0: off) */
extern int vp8_mg_mvcheap;   /* 1: no sub-pixel position with both components fractional */

typedef struct {
    vpx_codec_ctx_t enc;
    vpx_codec_enc_cfg_t cfg;
    vpx_image_t img;
    unsigned long deadline;
    int w, h, lf, mvcheap;
    uint8_t *buf;
    size_t cap;
} mgvpx_t;

static int opt(const char *params, const char *key, int def)
{
    if (!params) {
        return def;
    }
    size_t n = strlen(key);
    for (const char *p = params; (p = strstr(p, key)) != NULL; p += n) {
        if ((p == params || p[-1] == ':') && p[n] == '=') {
            if (!strncmp(p + n + 1, "rt", 2)) {
                return 1;
            }
            if (!strncmp(p + n + 1, "good", 4)) {
                return 0;
            }
            return atoi(p + n + 1);
        }
    }
    return def;
}

static void set_rate(vpx_codec_enc_cfg_t *c, int kbps, int vbv_ms)
{
    c->rc_target_bitrate = (unsigned)kbps;
    c->rc_buf_sz = (unsigned)vbv_ms;
    c->rc_buf_initial_sz = (unsigned)vbv_ms * 3 / 4;
    c->rc_buf_optimal_sz = (unsigned)vbv_ms;
}

int mgvpx_available(void)
{
    return 1;
}

void *mgvpx_open(int w, int h, int fps, int kbps, const char *params)
{
    mgvpx_t *m = calloc(1, sizeof(*m));
    if (!m || vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &m->cfg, 0) != VPX_CODEC_OK) {
        free(m);
        return NULL;
    }
    vpx_codec_enc_cfg_t *c = &m->cfg;
    m->w = w;
    m->h = h;
    c->g_w = (unsigned)w;
    c->g_h = (unsigned)h;
    c->g_timebase.num = 1;
    c->g_timebase.den = fps > 0 ? fps : 30;
    c->g_threads = (unsigned)opt(params, "threads", 1);
    c->g_profile = (unsigned)opt(params, "profile", 1);
    c->g_lag_in_frames = 0;
    /* er=1 for clip loops: every frame codes against the probabilities its
     * last keyframe left, as each frame's updates are dropped after it. VP8
     * otherwise carries them from frame to frame, and a steady loop played
     * after another steady loop (ClipStreamer) would be parsed with the
     * wrong ones: garbage until the next keyframe. About 2% more bits. */
    c->g_error_resilient = opt(params, "er", 0) ? VPX_ERROR_RESILIENT_DEFAULT : 0;
    int vbr = opt(params, "vbr", 0);
    c->rc_end_usage = vbr ? VPX_VBR : VPX_CBR;
    set_rate(c, kbps, opt(params, "vbv-ms", 200));
    c->rc_undershoot_pct = vbr ? 100 : 50;
    c->rc_overshoot_pct = vbr ? 30 : 5;
    c->rc_min_quantizer = 2;
    c->rc_max_quantizer = 56;
    c->rc_dropframe_thresh = 0;
    c->rc_resize_allowed = 0;
    c->kf_mode = VPX_KF_DISABLED;   /* keyframes only when asked */
    m->deadline = opt(params, "deadline", 0) ? VPX_DL_REALTIME : VPX_DL_GOOD_QUALITY;
    m->lf = opt(params, "lf", 0);
    m->mvcheap = opt(params, "mvcheap", 1);
    vp8_mg_lf_max = m->lf;
    vp8_mg_mvcheap = m->mvcheap;
    if (vpx_codec_enc_init(&m->enc, vpx_codec_vp8_cx(), c, 0) != VPX_CODEC_OK) {
        fprintf(stderr, "MuseCam vpx: encoder init failed\n");
        free(m);
        return NULL;
    }
    vpx_codec_control(&m->enc, VP8E_SET_CPUUSED, opt(params, "cpu", 2));
    vpx_codec_control(&m->enc, VP8E_SET_TOKEN_PARTITIONS, c->g_threads > 1 ? 1 : 0);
    vpx_codec_control(&m->enc, VP8E_SET_STATIC_THRESHOLD, 1);
    vpx_codec_control(&m->enc, VP8E_SET_MAX_INTRA_BITRATE_PCT, 300);
    vpx_codec_control(&m->enc, VP8E_SET_NOISE_SENSITIVITY, 0);
    vpx_img_wrap(&m->img, VPX_IMG_FMT_I420, (unsigned)w, (unsigned)h, 1, NULL);
    return m;
}

int mgvpx_encode(void *enc, const uint8_t *i420, int64_t pts, int key, const uint8_t **out, int info[2])
{
    mgvpx_t *m = enc;
    if (!m || !i420) {
        return 0;
    }
    vp8_mg_lf_max = m->lf;
    vp8_mg_mvcheap = m->mvcheap;
    m->img.planes[0] = (unsigned char *)i420;
    m->img.planes[1] = (unsigned char *)i420 + m->w * m->h;
    m->img.planes[2] = (unsigned char *)i420 + m->w * m->h * 5 / 4;
    m->img.stride[0] = m->w;
    m->img.stride[1] = m->img.stride[2] = m->w / 2;
    if (vpx_codec_encode(&m->enc, &m->img, pts, 1, key ? VPX_EFLAG_FORCE_KF : 0, m->deadline) != VPX_CODEC_OK) {
        return 0;
    }
    vpx_codec_iter_t it = NULL;
    const vpx_codec_cx_pkt_t *pkt;
    int n = 0;
    while ((pkt = vpx_codec_get_cx_data(&m->enc, &it)) != NULL) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT || n) {
            continue;   /* one frame per picture (no lag) */
        }
        size_t sz = pkt->data.frame.sz;
        if (sz > m->cap) {
            free(m->buf);
            m->cap = sz * 2;
            m->buf = malloc(m->cap);
            if (!m->buf) {
                m->cap = 0;
                return 0;
            }
        }
        memcpy(m->buf, pkt->data.frame.buf, sz);
        n = (int)sz;
        if (info) {
            int q = 0;
            vpx_codec_control(&m->enc, VP8E_GET_LAST_QUANTIZER_64, &q);
            info[0] = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) ? 1 : 0;
            info[1] = q;
        }
    }
    *out = m->buf;
    return n;
}

void mgvpx_set_bitrate(void *enc, int kbps)
{
    mgvpx_t *m = enc;
    if (!m) {
        return;
    }
    set_rate(&m->cfg, kbps, (int)m->cfg.rc_buf_sz);
    vpx_codec_enc_config_set(&m->enc, &m->cfg);
}

void mgvpx_close(void *enc)
{
    mgvpx_t *m = enc;
    if (m) {
        vpx_codec_destroy(&m->enc);
        free(m->buf);
        free(m);
    }
}
