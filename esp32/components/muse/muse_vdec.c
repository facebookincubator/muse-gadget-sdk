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

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "muse_vstream.h"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "esp_heap_caps.h"
#include "muse_mem.h"   /* MUSE_SIMD_CAPS: main SRAM for what the SIMD code touches */
#endif
#endif
#include "h264bsd_decoder.h"
#include "muse_vdec_same.h"
#if CONFIG_MUSE_VIDEO_VP8
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#endif

struct muse_vdec {
    uint8_t codec;
    int max_w, max_h;
    muse_vdec_progress_t cb;
    void *cb_ctx;
    storage_t *h264;
    /* The macroblocks that are the previous picture's (muse_vdec_same.h),
     * two maps in turn: the picture on show keeps its own while the next
     * one decodes (the caller holds at most two pictures). */
    uint8_t *same[2];
    size_t same_cap;
    int same_next;
    const u8 *prev_out;   /* the previous picture h264bsdDecode returned */
    uint32_t prev_seq;
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
    return codec == MUSE_VIDEO_CODEC_H264;   /* tinyh264, or its low-memory copy without PSRAM */
}

/* Pictures numbered across every decoder: a number is never reused, also with two decoders on two cores (the
 * band decoder beside the main one, muse_video.c). */
static atomic_uint_fast32_t s_seq;

static uint32_t next_seq(void)
{
    return (uint32_t)atomic_fetch_add_explicit(&s_seq, 1, memory_order_relaxed) + 1;
}

/* ---- H.264 (tinyh264): packed planes, macroblock-aligned ---------------- */

bool muse_h264_crop_window(int coded_w, int coded_h, unsigned chroma_format_idc, bool frame_mbs_only,
                           uint32_t left, uint32_t right, uint32_t top, uint32_t bottom, muse_vcrop_t *win)
{
    *win = (muse_vcrop_t){ 0, 0, coded_w, coded_h };
    /* SubWidthC, SubHeightC (table 6-1); monochrome: 1 x 1 */
    static const uint8_t sub_w[4] = { 1, 2, 2, 1 }, sub_h[4] = { 1, 2, 1, 1 };
    if (chroma_format_idc > 3 || coded_w <= 0 || coded_h <= 0) {
        return false;
    }
    uint64_t ux = sub_w[chroma_format_idc], uy = (uint64_t)sub_h[chroma_format_idc] * (frame_mbs_only ? 1 : 2);
    uint64_t cx = ux * ((uint64_t)left + right), cy = uy * ((uint64_t)top + bottom);
    if (cx >= (uint64_t)coded_w || cy >= (uint64_t)coded_h) {
        return false;   /* nothing left (7.4.2.1.1 forbids it) */
    }
    *win = (muse_vcrop_t){ (int)(ux * left), (int)(uy * top), coded_w - (int)cx, coded_h - (int)cy };
    return true;
}

/* tinyh264's picture: packed planes, macroblock aligned (w x h coded), shown
 * as its sequence parameter set's frame cropping window (4:2:0 frames: the
 * offsets are even, so the chroma window starts on a whole sample). */
static muse_vcrop_t h264_window(const muse_vdec_t *d, int w, int h)
{
    u32 l, r, t, b;
    muse_vcrop_t c;
    h264bsdCroppingOffsets(d->h264, &l, &r, &t, &b);
    muse_h264_crop_window(w, h, 1, true, l, r, t, b, &c);
    return c;
}

static void h264_pic(muse_pic_t *p, const u8 *pic, int w, int h, const muse_vcrop_t *c)
{
    *p = (muse_pic_t){ 0 };
    const u8 *u = pic + (size_t)w * h;
    const u8 *v = u + (size_t)(w / 2) * (h / 2);
    p->y = pic + (size_t)c->y * w + c->x;
    p->u = u + (size_t)(c->y / 2) * (w / 2) + c->x / 2;
    p->v = v + (size_t)(c->y / 2) * (w / 2) + c->x / 2;
    p->y_stride = w;
    p->c_stride = w / 2;
    p->w = c->w;
    p->h = c->h;
}

static void h264_progress(void *ctx, const u8 *pic, u32 width_mbs, u32 height_mbs, u32 rows)
{
    muse_vdec_t *d = ctx;
    muse_pic_t p;
    int w = (int)width_mbs * 16, h = (int)height_mbs * 16;
    muse_vcrop_t c = h264_window(d, w, h);
    h264_pic(&p, pic, w, h, &c);
    /* The window's rows that are final (the coded ones above it aren't shown). */
    int r = (int)rows - c.y;
    d->cb(d->cb_ctx, &p, r < 0 ? 0 : r > c.h ? c.h : r);
}

/* ---- VP8 (libvpx_vp8): its frame buffers have borders ------------------- */

#if CONFIG_MUSE_VIDEO_VP8
static void vp8_progress(void *ctx, const unsigned char *y, const unsigned char *u, const unsigned char *v, int ys,
                         int cs, int w, int h, int rows)
{
    muse_vdec_t *d = ctx;
    muse_pic_t p = { y, u, v, ys, cs, (w + 15) & ~15, (h + 15) & ~15, 0, NULL, 0, 0 };
    d->cb(d->cb_ctx, &p, rows);
}
#endif

muse_vdec_t *muse_vdec_open(uint8_t codec, int max_w, int max_h)
{
    if (!muse_vdec_has(codec)) {
        return NULL;
    }
    /* In PSRAM where there is some, like the decoders' own memory: small
     * internal blocks can land in slow LP RAM (h264bsd_util.h). */
#if defined(ESP_PLATFORM) && CONFIG_SPIRAM
    muse_vdec_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) {
        d = calloc(1, sizeof(*d));
    }
#else
    muse_vdec_t *d = calloc(1, sizeof(*d));
#endif
    if (!d) {
        return NULL;
    }
    d->codec = codec;
    d->max_w = max_w;
    d->max_h = max_h;
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
    /* The picture after cropping: at most the stream's size in whole
     * macroblocks. The coded one: that plus 2 macroblocks a side for the
     * cropping, and at least the iPhone encoder's 192 x 96 floor. */
    u32 mw = (u32)(max_w + 15) / 16, mh = (u32)(max_h + 15) / 16;
    h264bsdSetMaxSize(d->h264, mw + 2 > 12 ? mw + 2 : 12, mh + 2 > 6 ? mh + 2 : 6);
    h264bsdSetMaxCropped(d->h264, mw * 16, mh * 16);
    return d;
}

void muse_vdec_limit_refs(muse_vdec_t *d, unsigned max_refs)
{
    if (d && d->h264) h264bsdSetMaxReferences(d->h264, (u32)max_refs);
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
    free(d->same[0]);
    free(d->same[1]);
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
    if (d->h264) {
        h264bsdSetProgress(d->h264, cb ? h264_progress : NULL, d);
    }
}

/* A VP8 keyframe's size: its 3-byte tag, the start code, then 14-bit width and height. */
__attribute__((unused)) static bool vp8_key_size(const uint8_t *buf, uint32_t len, int *w, int *h)
{
    if (len < 10 || (buf[0] & 1) || buf[3] != 0x9d || buf[4] != 0x01 || buf[5] != 0x2a) {
        return false;
    }
    *w = (buf[6] | buf[7] << 8) & 0x3fff;
    *h = (buf[8] | buf[9] << 8) & 0x3fff;
    return true;
}

/* An H.264 NAL unit header: a parameter set or an IDR slice (types 7, 8, 5). */
static bool h264_may_realloc(uint8_t nal_header)
{
    uint8_t type = nal_header & 0x1f;
    return type == 5 || type == 7 || type == 8;
}

bool muse_vdec_may_realloc(const muse_vdec_t *d, const uint8_t *buf, uint32_t len)
{
#if CONFIG_MUSE_VIDEO_VP8
    if (d->vp8_open) {
        return len > 0 && !(buf[0] & 1);   /* a keyframe (a new size reallocates) */
    }
#endif
    (void)d;
    /* tinyh264 reallocates its pictures when an IDR slice activates a
     * sequence parameter set other than the active one, and that set (or the
     * picture parameter set naming it) may have come in an earlier access
     * unit: any parameter set or IDR slice (NAL types 7, 8 and 5) counts, not
     * only a sequence parameter set in this one. Keyframes carry all three.
     * A buffer that doesn't start 00 00 00 or 00 00 01 is one NAL unit to
     * tinyh264, its header the first byte (h264bsdExtractNalUnit), and
     * nothing makes a bottom band start with a start code: its first byte
     * counts, as do the start codes after it. */
    if (len && (len <= 3 || buf[0] || buf[1] || (buf[2] & 0xfe)) && h264_may_realloc(buf[0])) {
        return true;
    }
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && h264_may_realloc(buf[i + 3])) {
            return true;
        }
    }
    return false;
}

/* An IDR slice (NAL unit type 5) in the access unit. */
static bool au_has_idr(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            uint8_t t = buf[i + 3] & 0x1f;
            if (t == 5) {
                return true;
            }
            if (t == 1) {
                return false;   /* a picture's first slice decides */
            }
            i += 2;
        }
    }
    return false;
}

/* Only parameter sets (SPS, PPS; access unit delimiters too), at least one:
 * an access unit that may rightly make no picture. Read before decoding (the
 * decoder edits its input in place). */
static bool au_ps_only(const uint8_t *buf, uint32_t len)
{
    bool ps = false;
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            uint8_t t = buf[i + 3] & 0x1f;
            if (t != 7 && t != 8 && t != 9) {
                return false;
            }
            ps |= t != 9;
            i += 2;
        }
    }
    return ps;
}

muse_vdec_result_t muse_vdec_decode(muse_vdec_t *d, const uint8_t *buf, uint32_t len, muse_pic_t *pic)
{
#if CONFIG_MUSE_VIDEO_VP8
    if (d->vp8_open) {
        int kw, kh;
        if (vp8_key_size(buf, len, &kw, &kh) && (kw > d->max_w || kh > d->max_h || !kw || !kh)) {
            return MUSE_VDEC_ERROR;   /* larger than the stream: never allocated */
        }
        if (vpx_codec_decode(&d->vp8, buf, len, NULL, 0) != VPX_CODEC_OK) {
            return MUSE_VDEC_ERROR;
        }
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img = vpx_codec_get_frame(&d->vp8, &it);
        if (!img) {
            return MUSE_VDEC_NO_PICTURE;   /* a hidden (alt-ref) frame */
        }
        *pic = (muse_pic_t){ img->planes[0], img->planes[1], img->planes[2], img->stride[0], img->stride[1],
                             ((int)img->d_w + 15) & ~15, ((int)img->d_h + 15) & ~15, next_seq(), NULL, 0, 0 };
        return MUSE_VDEC_PICTURE;
    }
#endif
    u8 *p = NULL;
    u32 w = 0, h = 0;
    /* Each access unit decodes on its own, whatever came before it: a
     * picture an earlier one left unfinished is dropped first, never
     * concealed and returned for this one (tinyh264 would, and then skip this
     * access unit: after one damaged frame every keyframe "decoded" in 3 ms
     * into the old picture and the next frame failed, for 22 s on the Watcher). */
    if (h264bsdPictureStarted(d->h264)) {
        h264bsdAbandonPicture(d->h264);
    }
    bool idr = au_has_idr(buf, len);
    bool ps_only = au_ps_only(buf, len);
    /* Parameter sets of the stream's size and profile only, and a new picture
     * parameter set only beside a keyframe, where the app sends its own:
     * damaged bytes that parse as one never take over (VIDEO.md, Trust). */
    h264bsdAllowParamSetChange(d->h264, idr);
    u32 rc = h264bsdDecode(d->h264, (u8 *)buf, len, &p, &w, &h);
    if (rc == H264BSD_PIC_RDY && p) {
        muse_vcrop_t c = h264_window(d, (int)w, (int)h);
        h264_pic(pic, p, (int)w, (int)h, &c);
        pic->seq = next_seq();
        size_t mbs = (size_t)(w / 16) * (h / 16);
        if (mbs > d->same_cap) {
            free(d->same[0]);
            free(d->same[1]);
            d->same[0] = malloc(mbs);   /* internal RAM: read once per macroblock per picture */
            d->same[1] = malloc(mbs);
            if (!d->same[0] || !d->same[1]) {
                free(d->same[0]);
                free(d->same[1]);
                d->same[0] = d->same[1] = NULL;
            }
            d->same_cap = d->same[0] ? mbs : 0;
        }
        uint8_t *same = d->same[d->same_next];
        /* Against this decoder's previous picture (prev_out is cleared by
         * anything between them that isn't a picture). */
        /* The map is per coded macroblock: it lines up with the picture only
         * when the window starts at the coded one's top left (cropping on
         * the right and bottom alone, the usual padding). */
        if (same && d->prev_out && c.x == 0 && c.y == 0 &&
            muse_h264_same_map(d->h264, d->prev_out, same, d->same_cap) >= 0) {
            d->same_next ^= 1;
            pic->same = same;
            pic->same_as = d->prev_seq;
            pic->same_cols = (int)w / 16;
        }
        d->prev_out = p;
        d->prev_seq = pic->seq;
        if (idr) {
            /* The stream's own parameter sets and format, from a keyframe
             * that made a picture: kept, to put back after damage. Without
             * memory for the copies the format still locks (the next
             * keyframe tries the copies again). */
            h264bsdKeepParamSets(d->h264);
        }
        return MUSE_VDEC_PICTURE;
    }
    d->prev_out = NULL;   /* an error or a hidden picture: the next one starts afresh */
    if (rc == H264BSD_RDY && !h264bsdPictureStarted(d->h264) && ps_only) {
        return MUSE_VDEC_NO_PICTURE;   /* parameter sets alone */
    }
    /* An error, an access unit that ended before its picture did (slices
     * missing), or one with no picture and more than parameter sets (an SEI
     * alone, NAL units nobody decodes): nothing of it is kept, and the
     * parameter sets the last keyframe decoded with come back, in case
     * damaged bytes replaced them. */
    h264bsdAbandonPicture(d->h264);
    h264bsdRestoreParamSets(d->h264);   /* (without memory: nothing put back; the error asks for a keyframe anyway) */
    return MUSE_VDEC_ERROR;
}

bool muse_vdec_no_memory(const muse_vdec_t *d)
{
    (void)d;
    return false;
}

bool muse_vdec_fits(uint8_t codec, int w, int h)
{
    (void)codec, (void)w, (void)h;
    return true;
}

bool muse_vdec_prepare(muse_vdec_t *d, int cw, int ch)
{
    (void)d, (void)cw, (void)ch;
    return d != NULL;
}

bool muse_vdec_coded(const muse_vdec_t *d, int *cw, int *ch, int *reach)
{
    (void)d, (void)cw, (void)ch, (void)reach;
    return false;
}
