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
#include "muse_vdec.h"

#include <stdlib.h>
#include <string.h>

#include "h264bsd_decoder.h"
#include "muse_vstream.h"
#if CONFIG_MUSE_VIDEO_VP8
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#endif

struct muse_vdec {
    uint8_t codec;
    muse_vdec_progress_t cb;
    void *cb_ctx;
    storage_t *h264;
#if CONFIG_MUSE_VIDEO_VP8
    vpx_codec_ctx_t vp8;
    bool vp8_open;
#endif
};

bool muse_vdec_has(uint8_t codec)
{
#if CONFIG_MUSE_VIDEO_VP8
    if (codec == MUSE_VIDEO_CODEC_VP8) {
        return true;
    }
#endif
    return codec == MUSE_VIDEO_CODEC_H264;
}

/* ---- H.264 (tinyh264): packed planes, macroblock-aligned ---------------- */

static void h264_pic(muse_pic_t *p, const u8 *pic, int w, int h)
{
    p->y = pic;
    p->u = pic + (size_t)w * h;
    p->v = p->u + (size_t)(w / 2) * (h / 2);
    p->y_stride = w;
    p->c_stride = w / 2;
    p->w = w;
    p->h = h;
}

static void h264_progress(void *ctx, const u8 *pic, u32 width_mbs, u32 height_mbs, u32 rows)
{
    muse_vdec_t *d = ctx;
    muse_pic_t p;
    h264_pic(&p, pic, (int)width_mbs * 16, (int)height_mbs * 16);
    d->cb(d->cb_ctx, &p, (int)rows);
}

/* ---- VP8 (libvpx_vp8): its frame buffers have borders ------------------- */

#if CONFIG_MUSE_VIDEO_VP8
static void vp8_progress(void *ctx, const unsigned char *y, const unsigned char *u, const unsigned char *v, int ys,
                         int cs, int w, int h, int rows)
{
    muse_vdec_t *d = ctx;
    muse_pic_t p = { y, u, v, ys, cs, (w + 15) & ~15, (h + 15) & ~15 };
    d->cb(d->cb_ctx, &p, rows);
}
#endif

muse_vdec_t *muse_vdec_open(uint8_t codec)
{
    if (!muse_vdec_has(codec)) {
        return NULL;
    }
    muse_vdec_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->codec = codec;
#if CONFIG_MUSE_VIDEO_VP8
    if (codec == MUSE_VIDEO_CODEC_VP8) {
        vpx_codec_dec_cfg_t cfg = { .threads = 1 };
        if (vpx_codec_dec_init(&d->vp8, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
            free(d);
            return NULL;
        }
        d->vp8_open = true;
        return d;
    }
#endif
    d->h264 = h264bsdAlloc();
    if (!d->h264 || h264bsdInit(d->h264, 1) != 0) {
        if (d->h264) {
            h264bsdFree(d->h264);
        }
        free(d);
        return NULL;
    }
    return d;
}

void muse_vdec_close(muse_vdec_t *d)
{
    if (!d) {
        return;
    }
#if CONFIG_MUSE_VIDEO_VP8
    if (d->vp8_open) {
        vpx_codec_destroy(&d->vp8);
    }
#endif
    if (d->h264) {
        h264bsdShutdown(d->h264);
        h264bsdFree(d->h264);
    }
    free(d);
}

void muse_vdec_set_progress(muse_vdec_t *d, muse_vdec_progress_t cb, void *ctx)
{
    d->cb = cb;
    d->cb_ctx = ctx;
#if CONFIG_MUSE_VIDEO_VP8
    if (d->vp8_open) {
        vp8dx_mgx_set_progress(&d->vp8, cb ? vp8_progress : NULL, d);
        return;
    }
#endif
    h264bsdSetProgress(d->h264, cb ? h264_progress : NULL, d);
}

muse_vdec_result_t muse_vdec_decode(muse_vdec_t *d, const uint8_t *buf, uint32_t len, muse_pic_t *pic)
{
#if CONFIG_MUSE_VIDEO_VP8
    if (d->vp8_open) {
        if (vpx_codec_decode(&d->vp8, buf, len, NULL, 0) != VPX_CODEC_OK) {
            return MUSE_VDEC_ERROR;
        }
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img = vpx_codec_get_frame(&d->vp8, &it);
        if (!img) {
            return MUSE_VDEC_NO_PICTURE;   /* a hidden (alt-ref) frame */
        }
        *pic = (muse_pic_t){ img->planes[0], img->planes[1], img->planes[2], img->stride[0], img->stride[1],
                             ((int)img->d_w + 15) & ~15, ((int)img->d_h + 15) & ~15 };
        return MUSE_VDEC_PICTURE;
    }
#endif
    u8 *p = NULL;
    u32 w = 0, h = 0;
    u32 rc = h264bsdDecode(d->h264, (u8 *)buf, len, &p, &w, &h);
    if (rc == H264BSD_PIC_RDY && p) {
        h264_pic(pic, p, (int)w, (int)h);
        return MUSE_VDEC_PICTURE;
    }
    return rc == H264BSD_RDY ? MUSE_VDEC_NO_PICTURE : MUSE_VDEC_ERROR;
}
