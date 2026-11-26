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

/* rom/tjpgd.h on the host, decoded by libjpeg (see the header). */
#include "rom/tjpgd.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

typedef struct {
    uint8_t *data;
    size_t len;
} host_t;

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
    int warnings;
} err_t;

static void on_error(j_common_ptr c)
{
    longjmp(((err_t *)c->err)->jump, 1);
}

static void on_message(j_common_ptr c, int level)
{
    if (level < 0) {
        ((err_t *)c->err)->warnings++;   /* corrupt data, a premature end */
    }
}

/* The stream jd_prepare kept for jd_decomp: one at a time, freed by the next prepare if never decoded. */
static host_t *s_kept;

static void host_free(host_t *h)
{
    if (h == s_kept) s_kept = NULL;
    if (h) free(h->data);
    free(h);
}

JRESULT jd_prepare(JDEC *jd, UINT (*infunc)(JDEC *, BYTE *, UINT), void *pool, UINT sz_pool, void *dev)
{
    (void)pool;
    host_free(s_kept);
    if (sz_pool < 3100) {
        return JDR_MEM1;
    }
    memset(jd, 0, sizeof(*jd));
    jd->device = dev;
    host_t *h = calloc(1, sizeof(*h));
    size_t cap = 0;
    for (;;) {
        if (h->len + 512 > cap) {
            cap = cap ? cap * 2 : 4096;
            h->data = realloc(h->data, cap);
        }
        UINT n = infunc(jd, h->data + h->len, 512);
        if (!n) break;
        h->len += n;
    }
    jd->host = h;
    struct jpeg_decompress_struct c;
    err_t e;
    c.err = jpeg_std_error(&e.mgr);
    e.mgr.error_exit = on_error;
    e.mgr.emit_message = on_message;
    e.warnings = 0;
    if (setjmp(e.jump)) {
        jpeg_destroy_decompress(&c);
        host_free(h);
        jd->host = NULL;
        return JDR_FMT1;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, h->data, (unsigned long)h->len);
    jpeg_read_header(&c, TRUE);
    JRESULT rc = c.progressive_mode || c.arith_code ? JDR_FMT3 : JDR_OK;
    jd->width = c.image_width;
    jd->height = c.image_height;
    jpeg_destroy_decompress(&c);
    if (rc != JDR_OK) {
        host_free(h);   /* nothing to decode: jd_decomp is never called */
        jd->host = NULL;
    } else {
        s_kept = h;
    }
    return rc;
}

JRESULT jd_decomp(JDEC *jd, UINT (*outfunc)(JDEC *, void *, JRECT *), BYTE scale)
{
    host_t *h = jd->host;
    struct jpeg_decompress_struct c;
    err_t e;
    uint8_t *volatile img = NULL;   /* kept across the longjmp */
    c.err = jpeg_std_error(&e.mgr);
    e.mgr.error_exit = on_error;
    e.mgr.emit_message = on_message;
    e.warnings = 0;
    JRESULT rc = JDR_OK;
    if (setjmp(e.jump)) {
        jpeg_destroy_decompress(&c);
        free(img);
        rc = JDR_FMT1;
        goto out;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, h->data, (unsigned long)h->len);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_RGB;
    c.scale_num = 1;
    c.scale_denom = 1u << scale;
    jpeg_start_decompress(&c);
    unsigned w = c.output_width, hh = c.output_height;
    img = malloc((size_t)w * hh * 3);
    while (c.output_scanline < hh) {
        JSAMPROW row = img + (size_t)c.output_scanline * w * 3;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    if (e.warnings) {
        rc = JDR_INP;   /* TJpgDec stops at the stream's end */
        free(img);
        goto out;
    }
    uint8_t block[16 * 16 * 3];
    for (unsigned y = 0; y < hh && rc == JDR_OK; y += 16) {
        for (unsigned x = 0; x < w; x += 16) {
            unsigned bw = w - x < 16 ? w - x : 16, bh = hh - y < 16 ? hh - y : 16;
            for (unsigned r = 0; r < bh; r++) {
                memcpy(block + r * bw * 3, img + ((size_t)(y + r) * w + x) * 3, bw * 3);
            }
            JRECT rect = { (uint16_t)x, (uint16_t)(x + bw - 1), (uint16_t)y, (uint16_t)(y + bh - 1) };
            if (!outfunc(jd, block, &rect)) {
                rc = JDR_INTR;
                break;
            }
        }
    }
    free(img);
out:
    host_free(h);
    jd->host = NULL;
    return rc;
}
