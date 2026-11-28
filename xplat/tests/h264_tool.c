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
 * H.264 test tool for xplat/tinyh264: decodes one stream and prints an MD5
 * per output picture, for bit-exact comparisons (tests/h264_corpus.py).
 *
 *   h264_tool [--crop] [--yuv OUT.yuv] IN
 *
 * IN is an Annex-B byte stream (.264), split here into access units (slices
 * in any order stay with their picture), or a
 * fuzzing record file (.au: one mode byte, then [u16 LE length][access unit]
 * records, as esp32/tests/vectors/h264/regress). Each access unit goes to
 * h264bsdDecode in one call, the way the firmware hands them over; one that
 * leaves a picture unfinished has it abandoned (h264bsdAbandonPicture).
 *
 * Output, one line per event:
 *   f N WxH MD5    picture N: the whole coded picture (I420, macroblock
 *                  aligned), or with --crop the frame_crop window, which is
 *                  what ffmpeg's framemd5 hashes
 *   e K RC         access unit K returned error RC
 *   end PICTURES ERRORS ACCESS_UNITS
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264bsd_decoder.h"

/* Under ASan the bytes past each access unit are poisoned, so a decoder read
 * beyond the unit is reported instead of finding stale bytes. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TOOL_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define TOOL_ASAN 1
#endif
#ifdef TOOL_ASAN
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(p, n) ((void)(p), (void)(n))
#define ASAN_UNPOISON_MEMORY_REGION(p, n) ((void)(p), (void)(n))
#endif

/* ---- MD5 (RFC 1321) ------------------------------------------------------ */

typedef struct {
    uint32_t h[4];
    uint64_t n;
    uint8_t buf[64];
} md5_t;

static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(md5_t *m, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const uint8_t R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 16; i++) {
        w[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d), g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d, g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d), g = (7 * i) & 15;
        }
        uint32_t t = d;
        d = c;
        c = b;
        b += rol(a + f + K[i] + w[g], R[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    m->h[0] += a, m->h[1] += b, m->h[2] += c, m->h[3] += d;
}

static void md5_init(md5_t *m)
{
    m->h[0] = 0x67452301, m->h[1] = 0xefcdab89, m->h[2] = 0x98badcfe, m->h[3] = 0x10325476;
    m->n = 0;
}

static void md5_add(md5_t *m, const uint8_t *p, size_t len)
{
    while (len) {
        size_t at = m->n & 63, k = 64 - at < len ? 64 - at : len;
        memcpy(m->buf + at, p, k);
        m->n += k, p += k, len -= k;
        if ((m->n & 63) == 0) {
            md5_block(m, m->buf);
        }
    }
}

static void md5_hex(md5_t *m, char out[33])
{
    uint64_t bits = m->n * 8;
    uint8_t pad[72] = { 0x80 };
    size_t padlen = ((m->n & 63) < 56 ? 56 : 120) - (m->n & 63);
    for (int i = 0; i < 8; i++) {
        pad[padlen + i] = (uint8_t)(bits >> (8 * i));
    }
    md5_add(m, pad, padlen + 8);
    for (int i = 0; i < 16; i++) {
        sprintf(out + 2 * i, "%02x", (m->h[i / 4] >> (8 * (i % 4))) & 0xff);
    }
}

/* ---- access units ---------------------------------------------------------- */

static int start_code(const uint8_t *d, size_t n, size_t i)
{
    return i + 3 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
}

/* Exp-Golomb reader over one NAL unit's payload, emulation prevention bytes
 * (00 00 03) skipped. Reading past the end sets bad. */
typedef struct {
    const uint8_t *d;
    size_t n, i, zeros;
    unsigned bit;
    uint8_t cur;
    int bad;
} rbsp_t;

static void rbsp_init(rbsp_t *b, const uint8_t *d, size_t n)
{
    memset(b, 0, sizeof *b);
    b->d = d, b->n = n, b->bit = 8;
}

static unsigned rbsp_bit(rbsp_t *b)
{
    if (b->bit == 8) {
        if (b->zeros >= 2 && b->i < b->n && b->d[b->i] == 3) {
            b->i++, b->zeros = 0;
        }
        if (b->i >= b->n) {
            b->bad = 1;
            return 0;
        }
        b->cur = b->d[b->i++];
        b->zeros = b->cur ? 0 : b->zeros + 1;
        b->bit = 0;
    }
    return (b->cur >> (7 - b->bit++)) & 1;
}

static uint32_t rbsp_u(rbsp_t *b, unsigned bits)
{
    uint32_t v = 0;
    while (bits--) {
        v = v << 1 | rbsp_bit(b);
    }
    return v;
}

static uint32_t rbsp_ue(rbsp_t *b)
{
    unsigned lz = 0;
    while (!rbsp_bit(b) && !b->bad) {
        if (++lz > 31) {
            b->bad = 1;
            return 0;
        }
    }
    return lz ? (rbsp_u(b, lz) + (1u << lz) - 1) : 0;
}

static int32_t rbsp_se(rbsp_t *b)
{
    uint32_t v = rbsp_ue(b);
    return v & 1 ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

/* What the slice header fields that tell pictures apart need (7.4.1.2.4) */
typedef struct {
    int ok;
    unsigned frame_num_bits, poc_type, poc_lsb_bits, poc_always_zero, frame_mbs_only, separate_planes;
} sps_info_t;
typedef struct {
    int ok;
    unsigned sps, bottom_poc_present;
} pps_info_t;
typedef struct {
    int ok;
    unsigned type, ref_idc, first_mb, pps, frame_num, field, bottom, idr_id, poc_lsb;
    int32_t delta[2];
} slice_key_t;

static void parse_sps(rbsp_t *b, sps_info_t *sps)
{
    unsigned profile = rbsp_u(b, 8);
    rbsp_u(b, 16);
    unsigned id = rbsp_ue(b);
    sps_info_t v = { 0 };
    if (id >= 32) {
        return;
    }
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        unsigned chroma = rbsp_ue(b);
        if (chroma == 3) {
            v.separate_planes = rbsp_u(b, 1);
        }
        rbsp_ue(b), rbsp_ue(b), rbsp_u(b, 1);
        if (rbsp_u(b, 1)) {   /* scaling matrices: skipped */
            for (unsigned k = 0; k < (chroma == 3 ? 12u : 8u) && !b->bad; k++) {
                if (rbsp_u(b, 1)) {
                    int last = 8, next = 8;
                    for (unsigned j = 0; j < (k < 6 ? 16u : 64u) && next && !b->bad; j++) {
                        int delta = rbsp_se(b);
                        if (delta < -128 || delta > 127) {   /* delta_scale's range */
                            b->bad = 1;
                            break;
                        }
                        next = (last + delta + 256) % 256;
                        last = next ? next : last;
                    }
                }
            }
        }
    }
    v.frame_num_bits = rbsp_ue(b) + 4;
    v.poc_type = rbsp_ue(b);
    if (v.poc_type == 0) {
        v.poc_lsb_bits = rbsp_ue(b) + 4;
    } else if (v.poc_type == 1) {
        v.poc_always_zero = rbsp_u(b, 1);
        rbsp_se(b), rbsp_se(b);
        for (uint32_t k = rbsp_ue(b); k && !b->bad; k--) {
            rbsp_se(b);
        }
    }
    rbsp_ue(b), rbsp_u(b, 1), rbsp_ue(b), rbsp_ue(b);
    v.frame_mbs_only = rbsp_u(b, 1);
    v.ok = !b->bad && v.frame_num_bits <= 16 && v.poc_type <= 2 && v.poc_lsb_bits <= 16;
    sps[id] = v;
}

static void parse_pps(rbsp_t *b, pps_info_t *pps)
{
    unsigned id = rbsp_ue(b), sps = rbsp_ue(b);
    rbsp_u(b, 1);
    unsigned bottom = rbsp_u(b, 1);
    if (id < 256) {
        pps[id] = (pps_info_t){ !b->bad && sps < 32, sps, bottom };
    }
}

static slice_key_t parse_slice(rbsp_t *b, unsigned type, unsigned ref_idc, const sps_info_t *sps_tab,
                               const pps_info_t *pps_tab)
{
    slice_key_t k = { 0 };
    k.type = type, k.ref_idc = ref_idc;
    k.first_mb = rbsp_ue(b);
    rbsp_ue(b);
    k.pps = rbsp_ue(b);
    if (b->bad || k.pps >= 256 || !pps_tab[k.pps].ok || !sps_tab[pps_tab[k.pps].sps].ok) {
        return k;
    }
    const sps_info_t *sps = &sps_tab[pps_tab[k.pps].sps];
    if (sps->separate_planes) {
        rbsp_u(b, 2);
    }
    k.frame_num = rbsp_u(b, sps->frame_num_bits);
    if (!sps->frame_mbs_only && (k.field = rbsp_u(b, 1))) {
        k.bottom = rbsp_u(b, 1);
    }
    if (type == 5) {
        k.idr_id = rbsp_ue(b);
    }
    unsigned bottom = pps_tab[k.pps].bottom_poc_present && !k.field;
    if (sps->poc_type == 0) {
        k.poc_lsb = rbsp_u(b, sps->poc_lsb_bits);
        if (bottom) {
            k.delta[0] = rbsp_se(b);
        }
    } else if (sps->poc_type == 1 && !sps->poc_always_zero) {
        k.delta[0] = rbsp_se(b);
        if (bottom) {
            k.delta[1] = rbsp_se(b);
        }
    }
    k.ok = !b->bad;
    return k;
}

/* The first slice of a new primary picture (7.4.1.2.4). With a header that
 * doesn't parse (damage), a slice at macroblock 0 starts one. */
static int new_picture(const slice_key_t *a, const slice_key_t *b)
{
    if (!a->ok || !b->ok) {
        return b->first_mb == 0;
    }
    return a->frame_num != b->frame_num || a->pps != b->pps || a->field != b->field || a->bottom != b->bottom ||
           (a->ref_idc != b->ref_idc && (a->ref_idc == 0 || b->ref_idc == 0)) || (a->type == 5) != (b->type == 5) ||
           (a->type == 5 && b->type == 5 && a->idr_id != b->idr_id) || a->poc_lsb != b->poc_lsb ||
           a->delta[0] != b->delta[0] || a->delta[1] != b->delta[1];
}

/* Annex-B: a new access unit at an AUD, SEI, SPS or PPS (or types 14-18)
 * once the current one has a slice, or at the first slice of a new picture,
 * told apart by the slice header as the decoder does, so slices in any order
 * (arbitrary slice order) stay in their picture's access unit, as the
 * firmware receives them. */
static size_t split_annexb(const uint8_t *d, size_t n, size_t *off, size_t *len, size_t max)
{
    static sps_info_t sps[32];
    static pps_info_t pps[256];
    slice_key_t prev = { 0 };
    size_t nau = 0, start = 0;
    int slice = 0;
    for (size_t i = 0; i + 3 < n && nau + 1 < max; i++) {
        if (!start_code(d, n, i)) {
            continue;
        }
        size_t s = i > 0 && d[i - 1] == 0 ? i - 1 : i;
        int t = d[i + 3] & 0x1f;
        size_t e = i + 4;   /* the NAL unit runs to the next start code */
        while (e < n && !start_code(d, n, e)) {
            e++;
        }
        rbsp_t b;
        rbsp_init(&b, d + i + 4, e - (i + 4));
        int cut = 0;
        if (t == 1 || t == 5) {
            slice_key_t k = parse_slice(&b, (unsigned)t, (d[i + 3] >> 5) & 3, sps, pps);
            cut = slice && new_picture(&prev, &k);
            prev = k;
        } else {
            cut = slice && (t == 6 || t == 7 || t == 8 || t == 9 || (t >= 14 && t <= 18));
        }
        if (cut) {
            off[nau] = start, len[nau++] = s - start;
            start = s, slice = 0;
        }
        if (t == 1 || t == 5) {
            slice = 1;
        } else if (t == 7) {
            parse_sps(&b, sps);
        } else if (t == 8) {
            parse_pps(&b, pps);
        }
        i += 2;
    }
    if (start < n) {
        off[nau] = start, len[nau++] = n - start;
    }
    return nau;
}

static size_t split_records(const uint8_t *d, size_t n, size_t *off, size_t *len, size_t max)
{
    size_t nau = 0, p = 1;
    while (p + 2 <= n && nau < max) {
        size_t l = d[p] | d[p + 1] << 8;
        p += 2;
        if (l > n - p) {
            l = n - p;
        }
        off[nau] = p, len[nau++] = l;
        p += l;
    }
    return nau;
}

int main(int argc, char **argv)
{
    int crop = 0;
    const char *yuv = NULL, *in = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--crop")) {
            crop = 1;
        } else if (!strcmp(argv[i], "--yuv") && i + 1 < argc) {
            yuv = argv[++i];
        } else {
            in = argv[i];
        }
    }
    if (!in) {
        fprintf(stderr, "usage: h264_tool [--crop] [--yuv OUT.yuv] IN.264|IN.au\n");
        return 2;
    }
    FILE *f = fopen(in, "rb");
    if (!f) {
        perror(in);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(n + 1);
    if (!data || fread(data, 1, n, f) != n) {
        fprintf(stderr, "%s: read failed\n", in);
        return 2;
    }
    fclose(f);
    size_t max = n / 2 + 2, *off = malloc(max * sizeof *off), *len = malloc(max * sizeof *len);
    size_t nlen = strlen(in);
    size_t nau = nlen > 3 && !strcmp(in + nlen - 3, ".au") ? split_records(data, n, off, len, max)
                                                           : split_annexb(data, n, off, len, max);
    FILE *o = yuv ? fopen(yuv, "wb") : NULL;

    storage_t *s = h264bsdAlloc();
    h264bsdInit(s, 1);
    /* One reused buffer, as the firmware decodes (no access unit keeps its
     * own address), with nothing readable past the unit: h264bsdDecode reads
     * only the length it is given. */
    uint8_t *buf = malloc(n ? n : 1);
    unsigned pics = 0, errors = 0;
    for (size_t k = 0; k < nau; k++) {
        ASAN_UNPOISON_MEMORY_REGION(buf, n);
        memcpy(buf, data + off[k], len[k]);
        ASAN_POISON_MEMORY_REGION(buf + len[k], n - len[k]);
        u8 *pic = NULL;
        u32 w = 0, h = 0;
        u32 rc = h264bsdDecode(s, buf, (u32)len[k], &pic, &w, &h);
        if (rc == H264BSD_PIC_RDY && pic) {
            u32 x0 = 0, y0 = 0, cw = w, ch = h;
            u32 l, r, t, b;
            if (crop && h264bsdCroppingOffsets(s, &l, &r, &t, &b) == 0 && 2 * (l + r) < w && 2 * (t + b) < h) {
                x0 = 2 * l, y0 = 2 * t, cw = w - 2 * (l + r), ch = h - 2 * (t + b);
            }
            md5_t m;
            md5_init(&m);
            for (u32 y = 0; y < ch; y++) {
                md5_add(&m, pic + (y0 + y) * w + x0, cw);
                if (o) {
                    fwrite(pic + (y0 + y) * w + x0, 1, cw, o);
                }
            }
            for (int c = 0; c < 2; c++) {
                const u8 *pl = pic + w * h + c * (w / 2) * (h / 2);
                for (u32 y = 0; y < ch / 2; y++) {
                    md5_add(&m, pl + (y0 / 2 + y) * (w / 2) + x0 / 2, cw / 2);
                    if (o) {
                        fwrite(pl + (y0 / 2 + y) * (w / 2) + x0 / 2, 1, cw / 2, o);
                    }
                }
            }
            char hex[33];
            md5_hex(&m, hex);
            printf("f %u %ux%u %s\n", pics++, cw, ch, hex);
        } else if (rc != H264BSD_RDY && rc != H264BSD_PIC_RDY) {
            printf("e %zu %u\n", k, rc);
            errors++;
        }
        if (rc != H264BSD_PIC_RDY && h264bsdPictureStarted(s)) {
            h264bsdAbandonPicture(s);
        }
    }
    h264bsdShutdown(s);
    h264bsdFree(s);
    if (o) {
        fclose(o);
    }
    printf("end %u %u %zu\n", pics, errors, nau);
    ASAN_UNPOISON_MEMORY_REGION(buf, n);
    free(buf);
    free(off);
    free(len);
    free(data);
    return 0;
}
