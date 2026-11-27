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
 * H264BSD_LEAN_VUI's equivalence: tinyh264's SPS parser with the full VUI
 * parser and with the lean one (h264bsd_vui.c), on
 *   - every SPS in the given streams (Annex B scanned),
 *   - each of them mutated (bit flips weighted to its tail, truncations,
 *     random tails, inserted bytes),
 *   - crafted SPSs: every VUI branch (aspect ratio with Extended_SAR, overscan,
 *     video signal with colour description, chroma location in and out of
 *     range, timing with zero fields, NAL and VCL hrd_parameters with 1..33
 *     cpbs and extreme rates, low_delay, pic_struct, bitstream_restriction
 *     absent, valid and inconsistent), whole and cut at every byte,
 * must give the same accept / reject (the SPS parser's return code and the
 * VUI's), the same bit position after the VUI and after the SPS, the same
 * fields the decoder reads (bitstream_restriction_flag, num_reorder_frames,
 * max_dec_frame_buffering, log2_max_mv_length_horizontal / _vertical) and
 * the same seqParamSet_t.
 *
 *   tinyh264_vui_lean_harness <mutations per SPS> <seed> stream...
 *
 * test_tinyh264_knobs.py builds it from h264bsd_vui.c and
 * h264bsd_seq_param_set.c compiled twice (full, lean) with their public names
 * renamed. TINYH264_VUI_DUMP=path writes each case there first, so a
 * sanitizer abort leaves its input.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basetype.h"
#include "h264bsd_stream.h"
#include "h264bsd_byte_stream.h"
#include "h264bsd_nal_unit.h"
#include "h264bsd_seq_param_set.h"
#include "h264bsd_vui.h"
#include "h264bsd_util.h"

u32 sps_full(strmData_t *s, seqParamSet_t *p);
u32 sps_lean(strmData_t *s, seqParamSet_t *p);
u32 vui_full(strmData_t *s, vuiParameters_t *v);
u32 vui_lean(strmData_t *s, vuiParameters_t *v);

/* The SPS parsers call these (renamed at compile time): where each VUI walk ended. */
static int g_called[2];
static u32 g_vrc[2], g_vbits[2];
u32 vui_full_hook(strmData_t *s, vuiParameters_t *v);
u32 vui_lean_hook(strmData_t *s, vuiParameters_t *v);
u32 vui_full_hook(strmData_t *s, vuiParameters_t *v)
{
    u32 r = vui_full(s, v);
    g_called[0] = 1; g_vrc[0] = r; g_vbits[0] = s->strmBuffReadBits;
    return r;
}
u32 vui_lean_hook(strmData_t *s, vuiParameters_t *v)
{
    u32 r = vui_lean(s, v);
    g_called[1] = 1; g_vrc[1] = r; g_vbits[1] = s->strmBuffReadBits;
    return r;
}

static uint64_t s_rng = 1;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static unsigned long long n_cases, n_ok, n_vui, n_vui_ok, n_restrict, n_hrd_ok, n_fail;

struct result { int parsed; u32 rc, end_bits; int called; u32 vrc, vbits; seqParamSet_t sps; };

/* one SPS NAL (with or without its start code) through one parser */
static void run(int lean, const uint8_t *nal, size_t len, struct result *r)
{
    uint8_t *buf = calloc(1, len + 16);
    strmData_t s;
    nalUnit_t nu;
    u32 read = 0;
    memset(r, 0, sizeof(*r));
    memcpy(buf, nal, len);
    if (h264bsdExtractNalUnit(buf, (u32)len, &s, &read) != HANTRO_OK || h264bsdDecodeNalUnit(&s, &nu) != HANTRO_OK ||
        nu.nalUnitType != NAL_SEQ_PARAM_SET) {
        free(buf);
        return;
    }
    r->parsed = 1;
    g_called[lean] = 0;
    r->rc = lean ? sps_lean(&s, &r->sps) : sps_full(&s, &r->sps);
    r->end_bits = s.strmBuffReadBits;
    r->called = g_called[lean]; r->vrc = g_vrc[lean]; r->vbits = g_vbits[lean];
    free(buf);
}

static void report(const char *what, const uint8_t *nal, size_t len, const struct result *a, const struct result *b,
           const char *why)
{
    fprintf(stderr, "MISMATCH (%s): %s: full rc %u vui %d/%u@%u end %u | lean rc %u vui %d/%u@%u end %u\n  ", what, why,
        a->rc, a->called, a->vrc, a->vbits, a->end_bits, b->rc, b->called, b->vrc, b->vbits, b->end_bits);
    for (size_t i = 0; i < len && i < 64; ++i) fprintf(stderr, "%02x", nal[i]);
    fprintf(stderr, "\n");
    n_fail++;
}

static const char *s_dump;   /* TINYH264_VUI_DUMP */

static void compare(const char *what, const uint8_t *nal, size_t len, struct result *pa, struct result *pb)
{
    struct result a = *pa, b = *pb;   /* copies: the pointers are compared apart */
    if (!a.parsed && !b.parsed) return;
    n_cases++;
    if (a.parsed != b.parsed) { report(what, nal, len, &a, &b, "NAL header"); return; }
    if (a.rc != b.rc) { report(what, nal, len, &a, &b, "accept / reject"); return; }
    if (a.called != b.called || (a.called && a.vrc != b.vrc)) { report(what, nal, len, &a, &b, "VUI accept / reject"); return; }
    if (a.called) {
        n_vui++;
        if (a.vrc == HANTRO_OK) {
            n_vui_ok++;
            if (a.vbits != b.vbits) { report(what, nal, len, &a, &b, "bit position after the VUI"); return; }
        }
    }
    if (a.rc != HANTRO_OK) return;
    n_ok++;
    if (a.end_bits != b.end_bits) { report(what, nal, len, &a, &b, "bit position after the SPS"); return; }
    vuiParameters_t *va = a.sps.vuiParameters, *vb = b.sps.vuiParameters;
    if (!!va != !!vb) { report(what, nal, len, &a, &b, "VUI kept"); return; }
    if (va) {
        if (va->bitstreamRestrictionFlag != vb->bitstreamRestrictionFlag || va->numReorderFrames != vb->numReorderFrames ||
            va->maxDecFrameBuffering != vb->maxDecFrameBuffering ||
            va->log2MaxMvLengthHorizontal != vb->log2MaxMvLengthHorizontal ||
            va->log2MaxMvLengthVertical != vb->log2MaxMvLengthVertical) {
            report(what, nal, len, &a, &b, "the VUI fields the decoder reads");
            return;
        }
        if (va->bitstreamRestrictionFlag) n_restrict++;
        if (va->nalHrdParametersPresentFlag || va->vclHrdParametersPresentFlag) n_hrd_ok++;
    }
    /* the rest of the SPS: every field but the two pointers, then what they point at */
    i32 *oa = a.sps.offsetForRefFrame, *ob = b.sps.offsetForRefFrame;
    a.sps.vuiParameters = b.sps.vuiParameters = NULL;
    a.sps.offsetForRefFrame = b.sps.offsetForRefFrame = NULL;
    if (memcmp(&a.sps, &b.sps, sizeof(a.sps))) { report(what, nal, len, &a, &b, "seqParamSet_t"); return; }
    if (!!oa != !!ob || (oa && memcmp(oa, ob, a.sps.numRefFramesInPicOrderCntCycle * sizeof(i32)))) {
        report(what, nal, len, &a, &b, "offset_for_ref_frame");
        return;
    }
}

static void check(const char *what, const uint8_t *nal, size_t len)
{
    struct result a, b;
    if (s_dump) {
        FILE *f = fopen(s_dump, "wb");
        if (f) { fwrite(nal, 1, len, f); fclose(f); }
    }
    run(0, nal, len, &a);
    run(1, nal, len, &b);
    compare(what, nal, len, &a, &b);
    /* what the SPS parsers allocated, kept or not (the decoder frees it the same way) */
    free(a.sps.vuiParameters); free(b.sps.vuiParameters);
    free(a.sps.offsetForRefFrame); free(b.sps.offsetForRefFrame);
}

/* ---- crafted SPSs ---- */
struct bw { uint8_t b[512]; size_t bits; };
static void put(struct bw *w, uint64_t v, unsigned n)
{
    while (n--) {
        if (w->bits / 8 >= sizeof(w->b)) return;
        if ((v >> n) & 1) w->b[w->bits / 8] |= (uint8_t)(0x80 >> (w->bits % 8));
        w->bits++;
    }
}
static void ue(struct bw *w, uint64_t v)
{
    uint64_t x = v + 1;
    unsigned z = 0;
    while ((x >> (z + 1)) != 0) z++;
    put(w, 0, z);
    put(w, x, z + 1);
}
/* the SPS's fields up to vui_parameters_present_flag = 1: Baseline 3.0, 15 x 15 MBs, POC type 2 */
static void sps_head(struct bw *w)
{
    put(w, 66, 8); put(w, 0xC0, 8); put(w, 30, 8);
    ue(w, 0);   /* seq_parameter_set_id */
    ue(w, 0);   /* log2_max_frame_num_minus4 */
    ue(w, 2);   /* pic_order_cnt_type */
    ue(w, 1);   /* max_num_ref_frames */
    put(w, 0, 1);
    ue(w, 14); ue(w, 14);
    put(w, 1, 1); put(w, 1, 1); put(w, 0, 1);
    put(w, 1, 1);   /* vui_parameters_present_flag */
}
static uint32_t pick(const uint32_t *v, unsigned n) { return v[rnd() % n]; }
static void hrd(struct bw *w)
{
    static const uint32_t cnt[] = { 0, 0, 1, 2, 31, 32, 0xFFFFFFFFu };   /* cpb_cnt_minus1: 32 refused, 2^32-1 wraps */
    /* rates and sizes: 2^32 - 2 is the largest accepted; one hrd in four may carry 2^32 - 1 (refused) */
    static const uint32_t big[] = { 0, 1, 1000, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu };
    unsigned nbig = (rnd() & 3) ? 5 : 6;
    uint32_t c = pick(cnt, 7);
    ue(w, c);
    put(w, rnd() & 15, 4); put(w, rnd() & 15, 4);
    for (uint64_t i = 0; i < (uint64_t)c + 1 && i < 40; ++i) {
        ue(w, pick(big, nbig)); ue(w, pick(big, nbig)); put(w, rnd() & 1, 1);
    }
    put(w, rnd(), 20);
}
static void vui(struct bw *w)
{
    if (rnd() & 1) {   /* aspect ratio */
        put(w, 1, 1);
        uint32_t idc = (rnd() & 1) ? 255 : rnd() & 0xFF;
        put(w, idc, 8);
        if (idc == 255) { put(w, rnd(), 16); put(w, rnd(), 16); }
    } else put(w, 0, 1);
    if (rnd() & 1) { put(w, 1, 1); put(w, rnd() & 1, 1); } else put(w, 0, 1);
    if (rnd() & 1) {   /* video signal */
        put(w, 1, 1); put(w, rnd() & 7, 3); put(w, rnd() & 1, 1);
        if (rnd() & 1) { put(w, 1, 1); put(w, rnd(), 8); put(w, rnd(), 8); put(w, rnd(), 8); } else put(w, 0, 1);
    } else put(w, 0, 1);
    if (rnd() & 1) { put(w, 1, 1); ue(w, rnd() % 8); ue(w, rnd() % 8); } else put(w, 0, 1);   /* 6, 7 refused */
    if (rnd() & 1) {   /* timing: a zero field refused */
        put(w, 1, 1);
        put(w, (rnd() % 5) ? rnd() | 1 : 0, 32); put(w, (rnd() % 5) ? rnd() | 1 : 0, 32); put(w, rnd() & 1, 1);
    } else put(w, 0, 1);
    int nal = rnd() & 1, vcl = rnd() & 1;
    put(w, nal, 1); if (nal) hrd(w);
    put(w, vcl, 1); if (vcl) hrd(w);
    if (nal || vcl) put(w, rnd() & 1, 1);
    put(w, rnd() & 1, 1);
    if (rnd() & 1) {   /* bitstream_restriction: denominators over 16 refused; inconsistent frame counts refused by the SPS */
        static const uint32_t den[] = { 0, 2, 16, 16, 17, 100 };
        static const uint32_t fr[] = { 0, 1, 1, 2, 3, 16, 17, 0xFFFFFFFEu };
        put(w, 1, 1); put(w, rnd() & 1, 1);
        for (int i = 0; i < 4; ++i) ue(w, pick(den, 6));
        if (rnd() & 1) {   /* consistent: num_reorder_frames <= max_dec_frame_buffering, 1..16 */
            uint32_t mdfb = 1 + rnd() % 16;
            ue(w, rnd() % (mdfb + 1)); ue(w, mdfb);
        } else {
            ue(w, pick(fr, 8)); ue(w, pick(fr, 8));
        }
    } else put(w, 0, 1);
}
/* the RBSP as a NAL with its start code, emulation prevention inserted */
static size_t nal_of(const struct bw *w, int trailing, uint8_t *out)
{
    uint8_t rbsp[600];
    size_t n = (w->bits + 7) / 8, o = 0, zeros = 0;
    memcpy(rbsp, w->b, n);
    if (trailing) {
        if (w->bits % 8 == 0) rbsp[n++] = 0x80;
        else rbsp[n - 1] |= (uint8_t)(0x80 >> (w->bits % 8));
    }
    out[o++] = 0; out[o++] = 0; out[o++] = 1; out[o++] = 0x67;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && rbsp[i] <= 3) { out[o++] = 3; zeros = 0; }
        out[o++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return o;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void mutate_and_check(const uint8_t *nal, size_t len, int runs)
{
    uint8_t m[1200];
    if (len < 5) return;   /* a start code and a header, no payload to mutate */
    for (int r = 0; r < runs; ++r) {
        size_t n = len < sizeof(m) - 64 ? len : sizeof(m) - 64;
        memcpy(m, nal, n);
        switch (rnd() % 4) {
        case 0:   /* bit flips, weighted to the tail (the VUI is the SPS's last part) */
            for (unsigned k = 1 + rnd() % 3; k--;) {
                size_t lo = n > 8 ? n / 2 : 4;
                size_t at = (rnd() & 3) ? lo + rnd() % (n - lo) : 4 + rnd() % (n - 4);
                m[at] ^= (uint8_t)(1u << (rnd() & 7));
            }
            break;
        case 1:   /* cut */
            n = 5 + rnd() % (n - 4);
            break;
        case 2: { /* a random tail */
            size_t at = 5 + rnd() % (n - 4);
            size_t add = rnd() % 24;
            for (size_t i = at; i < at + add && i < sizeof(m) - 1; ++i) m[i] = (uint8_t)rnd();
            n = at + add;
            break;
        }
        default: { /* inserted bytes */
            size_t at = 5 + rnd() % (n - 4), add = 1 + rnd() % 4;
            memmove(m + at + add, m + at, n - at);
            for (size_t i = 0; i < add; ++i) m[at + i] = (uint8_t)rnd();
            n += add;
            break;
        }
        }
        check("mutated", m, n);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s mutations seed stream...\n", argv[0]);
        return 2;
    }
    int runs = atoi(argv[1]);
    s_dump = getenv("TINYH264_VUI_DUMP");
    s_rng = strtoull(argv[2], NULL, 0) * 0x9E3779B97F4A7C15ull + 1;
    unsigned long long stream_sps = 0, stream_vui = 0;
    for (int c = 3; c < argc; ++c) {
        size_t n;
        uint8_t *b = read_file(argv[c], &n);
        if (!b) { fprintf(stderr, "can't read %s\n", argv[c]); return 2; }
        for (size_t i = 0; i + 4 < n; ++i) {
            if (b[i] || b[i + 1] || b[i + 2] != 1 || (b[i + 3] & 0x1F) != 7) continue;
            size_t e = i + 3;
            while (e + 3 <= n && !(b[e] == 0 && b[e + 1] == 0 && (b[e + 2] == 1 || b[e + 2] == 0))) e++;
            if (e + 3 > n) e = n;
            unsigned long long ok0 = n_vui;
            check(argv[c], b + i, e - i);
            stream_sps++;
            stream_vui += n_vui - ok0;
            mutate_and_check(b + i, e - i, runs);
        }
        free(b);
    }
    /* crafted: random VUIs, each whole (with and without trailing bits) and cut at every byte */
    unsigned long long crafted = 0;
    for (int r = 0; r < 20000; ++r) {
        struct bw w;
        memset(&w, 0, sizeof(w));
        sps_head(&w);
        vui(&w);
        uint8_t nal[1300];
        size_t n = nal_of(&w, (rnd() % 8) != 0, nal);
        check("crafted", nal, n);
        for (size_t cut = 5; cut < n; ++cut) check("crafted, cut", nal, cut);
        crafted++;
    }
    printf("lean VUI equivalence: %llu SPS in the streams (%llu with a VUI), %d mutations each, %llu crafted (+ every cut): "
           "%llu cases, %llu accepted, %llu VUI walks (%llu accepted, %llu with bitstream_restriction, %llu with HRD), "
           "%llu mismatches\n",
           stream_sps, stream_vui, runs, crafted, n_cases, n_ok, n_vui, n_vui_ok, n_restrict, n_hrd_ok, n_fail);
    if (!stream_vui && argc > 3) { fprintf(stderr, "no stream's SPS carried a VUI\n"); return 1; }
    return n_fail ? 1 : 0;
}
