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
 * libx264 for Muse Gadgets Test on iOS (the same settings as Android's
 * jni/mgx264.c): one I420 picture in, one Annex B access unit out, no delay
 * (zerolatency) unless the params ask for lookahead. Built by
 * native/build_x264.sh against your x264 checkout.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x264.h>

#include "mgcodecs.h"

typedef struct {
    x264_t *enc;
    x264_param_t p;
    int w, h;
} mgx264_t;

static void logcb(void *priv, int level, const char *fmt, va_list ap)
{
    (void)priv;
    if (level <= X264_LOG_WARNING) {
        fprintf(stderr, "MuseCam x264: ");
        vfprintf(stderr, fmt, ap);
    }
}

static void set_rate(x264_param_t *p, int kbps, int vbv_ms)
{
    p->rc.i_rc_method = X264_RC_ABR;
    p->rc.i_bitrate = kbps;
    p->rc.i_vbv_max_bitrate = kbps;
    p->rc.i_vbv_buffer_size = kbps * vbv_ms / 1000 > 1 ? kbps * vbv_ms / 1000 : 1;
}

int mgx264_available(void)
{
    return 1;
}

void *mgx264_open(int w, int h, int fps, int kbps, const char *preset, const char *params)
{
    mgx264_t *m = calloc(1, sizeof(*m));
    if (!m || x264_param_default_preset(&m->p, preset ? preset : "fast", "zerolatency,psnr") < 0) {
        free(m);
        return NULL;
    }
    x264_param_t *p = &m->p;
    p->i_width = m->w = w;
    p->i_height = m->h = h;
    p->i_csp = X264_CSP_I420;
    p->i_fps_num = fps;
    p->i_fps_den = 1;
    p->i_timebase_num = 1;
    p->i_timebase_den = fps;
    p->b_vfr_input = 0;
    p->i_threads = 1;
    p->b_sliced_threads = 0;
    p->i_bframe = 0;
    p->i_frame_reference = 1;
    p->i_keyint_max = 3 * fps;   /* the intra refresh period */
    p->b_intra_refresh = 1;
    p->b_annexb = 1;
    p->b_repeat_headers = 1;
    p->rc.i_lookahead = 0;
    p->rc.b_mb_tree = 0;
    p->i_log_level = X264_LOG_WARNING;
    p->pf_log = logcb;
    set_rate(p, kbps, 200);
    if (params) {
        char *buf = strdup(params), *save = NULL;
        for (char *kv = strtok_r(buf, ":", &save); kv; kv = strtok_r(NULL, ":", &save)) {
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq = 0;
            }
            if (!strcmp(kv, "vbv-ms")) {
                set_rate(p, kbps, eq ? atoi(eq + 1) : 200);
            } else if (x264_param_parse(p, kv, eq ? eq + 1 : NULL) < 0) {
                fprintf(stderr, "MuseCam x264: bad option %s\n", kv);
            }
        }
        free(buf);
    }
    x264_param_apply_profile(p, "baseline");
    m->enc = x264_encoder_open(p);
    if (!m->enc) {
        free(m);
        return NULL;
    }
    return m;
}

int mgx264_encode(void *enc, const uint8_t *i420, int64_t pts, int idr, const uint8_t **out, int info[2])
{
    mgx264_t *m = enc;
    x264_picture_t in, pic;
    x264_nal_t *nal;
    int nn = 0, bytes;
    if (!m) {
        return 0;
    }
    if (!i420) {
        if (x264_encoder_delayed_frames(m->enc) <= 0) {
            return 0;
        }
        bytes = x264_encoder_encode(m->enc, &nal, &nn, NULL, &pic);
    } else {
        x264_picture_init(&in);
        in.img.i_csp = X264_CSP_I420;
        in.img.i_plane = 3;
        in.i_pts = pts;
        in.i_type = idr ? X264_TYPE_IDR : X264_TYPE_AUTO;
        in.img.plane[0] = (uint8_t *)i420;
        in.img.plane[1] = (uint8_t *)i420 + m->w * m->h;
        in.img.plane[2] = (uint8_t *)i420 + m->w * m->h * 5 / 4;
        in.img.i_stride[0] = m->w;
        in.img.i_stride[1] = in.img.i_stride[2] = m->w / 2;
        bytes = x264_encoder_encode(m->enc, &nal, &nn, &in, &pic);
    }
    if (bytes <= 0) {
        return 0;
    }
    *out = nal[0].p_payload;   /* the payloads are contiguous in x264's buffer */
    if (info) {
        info[0] = pic.i_type == X264_TYPE_IDR;
        info[1] = pic.i_qpplus1 - 1;
    }
    return bytes;
}

void mgx264_set_bitrate(void *enc, int kbps)
{
    mgx264_t *m = enc;
    if (!m) {
        return;
    }
    int ms = m->p.rc.i_bitrate > 0 ? m->p.rc.i_vbv_buffer_size * 1000 / m->p.rc.i_bitrate : 200;
    set_rate(&m->p, kbps, ms);
    x264_encoder_reconfig(m->enc, &m->p);
}

void mgx264_close(void *enc)
{
    mgx264_t *m = enc;
    if (m) {
        x264_encoder_close(m->enc);
        free(m);
    }
}
