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
 * tinyh264 straight, one access unit per h264bsdDecode call, for
 * test_tinyh264_knobs.py: built once as the decoder ships and once per knob,
 * it prints every call's result and a hash of every picture, so the builds
 * can be compared picture for picture. Each access unit gets a buffer of its
 * exact size (an overread is an ASan report, not a read of the next one).
 *
 *   harness [options] stream...
 *     -m vdec|raw   vdec (default): as muse_vdec.c, an unfinished picture is
 *                   abandoned before each access unit and after an error;
 *                   raw: nothing but h264bsdDecode
 *     -l flags      h264bsdSetLowMem(flags)            (H264BSD_LOWMEM)
 *     -s rows       h264bsdSetStrip(rows, ...)         (H264BSD_STRIP)
 *     -c            h264bsdChangedMap, checked against the pictures
 *                                                      (H264BSD_CHANGED_MAP)
 *     -L mbs,refs   h264bsdSetLimits(mbs, refs)        (H264BSD_LIMITS)
 *     -g            h264bsdSetBufferGate, checked      (H264BSD_BUFFER_GATE;
 *                   with TINYH264_CUSTOM_ALLOC also that the last picture
 *                   output is let go before its buffer is freed)
 *     -R            output reordering on (h264bsdInit(dec, 0); else as
 *                   muse_vdec.c, h264bsdInit(dec, 1))
 *     -k            as muse_vdec.c: parameter sets kept after an IDR
 *                   picture (h264bsdKeepParamSets), a changed one allowed
 *                   only in a keyframe's access unit, the kept ones put
 *                   back after an error
 *
 * A stream is Annex B (.264), split into access units here, or the
 * regression vectors' records (.au: a byte, then 16-bit little-endian
 * lengths). One JSON line a stream: {"file", "aus": [[rc, hash], ...],
 * "dropped": the access units that ended inside a picture, "peak": bytes
 * (with TINYH264_CUSTOM_ALLOC; else 0), "rows_lost" (strip mode): rows
 * handed out in access units that output no picture, ...}.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264bsd_decoder.h"

/* ---- the allocator, counted (TINYH264_CUSTOM_ALLOC builds) ---- */

static size_t s_cur, s_peak;

#if TINYH264_CUSTOM_ALLOC
typedef union hdr {
    struct {
        size_t n;
        union hdr *prev, *next;   /* the live blocks */
    } b;
    max_align_t a;
} hdr_t;
static hdr_t *s_blocks;

void *tinyh264_alloc(size_t n)
{
    hdr_t *h = malloc(sizeof(hdr_t) + n);
    if (!h) {
        return NULL;
    }
    h->b.n = n;
    h->b.prev = NULL;
    h->b.next = s_blocks;
    if (s_blocks) {
        s_blocks->b.prev = h;
    }
    s_blocks = h;
    s_cur += n;
    if (s_cur > s_peak) {
        s_peak = s_cur;
    }
    return h + 1;
}

/* the live block p points into (NULL: none) */
static const hdr_t *block_of(const void *p)
{
    for (const hdr_t *h = s_blocks; h; h = h->b.next) {
        const uint8_t *d = (const uint8_t *)(h + 1);
        if ((const uint8_t *)p >= d && (const uint8_t *)p < d + h->b.n) {
            return h;
        }
    }
    return NULL;
}

#if H264BSD_BUFFER_GATE
/* the last picture output, until the gate lets its buffer go: a display may
 * be reading it, so the decoder must not free its buffer before that */
static const u8 *s_shown;
static unsigned s_free_bad;
#endif

void *tinyh264_alloc_hot(size_t n)
{
    return tinyh264_alloc(n);
}

void tinyh264_free(void *p)
{
    if (p) {
        hdr_t *h = (hdr_t *)p - 1;
#if H264BSD_BUFFER_GATE
        if (s_shown && block_of(s_shown) == h) {
            s_free_bad++;
            s_shown = NULL;
        }
#endif
        if (h->b.prev) {
            h->b.prev->b.next = h->b.next;
        } else {
            s_blocks = h->b.next;
        }
        if (h->b.next) {
            h->b.next->b.prev = h->b.prev;
        }
        s_cur -= h->b.n;
        free(h);
    }
}
#endif

/* ---- hashing ---- */

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static uint32_t planes_hash(const uint8_t *y, const uint8_t *u, const uint8_t *v, uint32_t w, uint32_t h)
{
    uint32_t x = 2166136261u;
    x = fnv(y, (size_t)w * h, x);
    x = fnv(u, (size_t)w * h / 4, x);
    return fnv(v, (size_t)w * h / 4, x);
}

#define MAX_SIDE 1024   /* the largest picture side the harness takes */

/* ---- strip mode: the rows land in a whole picture here ---- */

#if H264BSD_STRIP
static uint8_t s_sy[MAX_SIDE * MAX_SIDE], s_su[MAX_SIDE * MAX_SIDE / 4], s_sv[MAX_SIDE * MAX_SIDE / 4];
static uint32_t s_sw, s_rows_seen, s_rows_lost;
static int s_strip_bad;

static void strip_row(void *ctx, u32 mbRow, const u8 *y, const u8 *cb, const u8 *cr, u32 yStride, u32 cStride)
{
    (void)ctx;
    if (mbRow == 0) {
        s_sw = yStride;
    }
    if (mbRow != s_rows_seen || yStride != s_sw || cStride != s_sw / 2 || s_sw > MAX_SIDE ||
        (mbRow + 1) * 16 > MAX_SIDE) {
        s_strip_bad = 1;   /* rows come once each, in order */
        return;
    }
    s_rows_seen = mbRow + 1;
    memcpy(s_sy + (size_t)mbRow * 16 * s_sw, y, (size_t)16 * s_sw);
    memcpy(s_su + (size_t)mbRow * 8 * (s_sw / 2), cb, (size_t)8 * (s_sw / 2));
    memcpy(s_sv + (size_t)mbRow * 8 * (s_sw / 2), cr, (size_t)8 * (s_sw / 2));
}
#endif

/* ---- the buffer gate: called once per picture, before it is written ---- */

static unsigned s_gates;
#if H264BSD_BUFFER_GATE
/* the buffers let go and not yet output: each output picture must be one */
static const u8 *s_gated[64];
static unsigned s_ngated;
static void gate(void *ctx, const u8 *pic)
{
    (void)ctx;
    s_gates++;
#if TINYH264_CUSTOM_ALLOC
    if (s_shown && block_of(s_shown) == block_of(pic)) {
        s_shown = NULL;   /* in place, pic is the start of the buffer both share */
    }
#endif
    for (unsigned i = 0; i < s_ngated; i++) {
        if (s_gated[i] == pic) {
            return;
        }
    }
    if (s_ngated < 64) {
        s_gated[s_ngated++] = pic;
    }
}

static int gated_out(const u8 *pic)
{
    for (unsigned i = 0; i < s_ngated; i++) {
        if (s_gated[i] == pic) {
            s_gated[i] = s_gated[--s_ngated];
            return 1;
        }
    }
    return 0;
}
#endif

/* ---- access units ---- */

typedef struct {
    const uint8_t *p;
    size_t n;
} span_t;

/* the next NAL unit's start (after its start code) at or after *at; n: up to the next start code */
static int next_nal(const uint8_t *b, size_t len, size_t *at, span_t *nal)
{
    size_t i = *at;
    while (i + 3 <= len && !(b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)) {
        i++;
    }
    if (i + 3 >= len) {
        return 0;   /* none, or a start code with no NAL unit header after it */
    }
    size_t s = i + 3, e = s;
    while (e + 3 <= len && !(b[e] == 0 && b[e + 1] == 0 && (b[e + 2] == 1 || (b[e + 2] == 0 && e + 3 < len && b[e + 3] == 1)))) {
        e++;
    }
    if (e + 3 > len) {
        e = len;
    }
    nal->p = b + s;
    nal->n = e - s;
    *at = e;
    return 1;
}

/* first_mb_in_slice == 0 (the RBSP's first bit is ue(v) 0: '1') */
static int first_mb_zero(const span_t *nal)
{
    return nal->n >= 2 && (nal->p[1] & 0x80);
}

/* a keyframe's access unit: its first slice is an IDR one */
static int au_has_idr(const uint8_t *b, size_t len)
{
    size_t at = 0;
    span_t nal;
    while (next_nal(b, len, &at, &nal)) {
        int type = nal.p[0] & 0x1F;
        if (type == 1 || type == 5) {
            return type == 5;
        }
    }
    return 0;
}

/* access unit boundaries: a new one starts at an AUD, SEI, SPS or PPS, or at
 * a slice with first_mb_in_slice 0, once the current one has a slice */
static size_t split_aus(const uint8_t *b, size_t len, span_t *aus, size_t cap)
{
    size_t at = 0, n = 0, au_start = 0;
    int has_vcl = 0, started = 0;
    span_t nal;
    while (next_nal(b, len, &at, &nal)) {
        int type = nal.p[0] & 0x1F;
        size_t nal_start = (size_t)(nal.p - b);
        /* include the start code (and a leading zero) in the access unit */
        size_t sc = nal_start - 3;
        if (sc > 0 && b[sc - 1] == 0) {
            sc--;
        }
        int vcl = type == 1 || type == 5;
        int boundary = has_vcl && ((type >= 6 && type <= 9) || (vcl && first_mb_zero(&nal)));
        if (!started) {
            au_start = sc;
            started = 1;
        } else if (boundary) {
            if (n < cap) {
                aus[n++] = (span_t){ b + au_start, sc - au_start };
            }
            au_start = sc;
            has_vcl = 0;
        }
        if (vcl) {
            has_vcl = 1;
        }
    }
    if (started && n < cap) {
        aus[n++] = (span_t){ b + au_start, len - au_start };
    }
    return n;
}

static size_t split_records(const uint8_t *b, size_t len, span_t *aus, size_t cap)
{
    size_t at = 1, n = 0;
    while (at + 2 <= len && n < cap) {
        size_t l = b[at] | (size_t)b[at + 1] << 8;
        at += 2;
        if (l > len - at) {
            l = len - at;
        }
        aus[n++] = (span_t){ b + at, l };
        at += l;
    }
    return n;
}

/* ---- the changed map, checked against the pictures ---- */

#if H264BSD_CHANGED_MAP
static uint8_t *s_prev;   /* the last output picture, w * h * 3 / 2 */
static uint32_t s_pw, s_ph;
static unsigned s_map_pics, s_map_clean, s_map_bad;
static unsigned s_map_nidr;
static uint32_t s_map_idr[64];   /* hashes of the pictures the map calls IDR */

/* a clean macroblock is the previous output picture's, byte for byte, but
 * for the 3 luma (1 chroma) rows / columns along an edge it shares with a
 * changed macroblock when the picture deblocked (bit 1) */
static void check_map(storage_t *dec, const uint8_t *y, const uint8_t *u, const uint8_t *v, uint32_t w, uint32_t h)
{
    const u8 *clean = NULL;
    u32 m = h264bsdChangedMap(dec, &clean);
    if ((m & 1) && s_prev && s_pw == w && s_ph == h) {
        uint32_t cols = w / 16, rows = h / 16;
        s_map_pics++;
        for (uint32_t r = 0; r < rows; r++) {
            for (uint32_t c = 0; c < cols; c++) {
                uint32_t i = r * cols + c;
                if (!(clean[i >> 3] & (1u << (i & 7)))) {
                    continue;
                }
                s_map_clean++;
                int mL = 0, mR = 0, mT = 0, mB = 0;
                if (m & 2) {
#define CLEAN(rr, cc) (clean[((rr) * cols + (cc)) >> 3] & (1u << (((rr) * cols + (cc)) & 7)))
                    mL = c > 0 && !CLEAN(r, c - 1) ? 3 : 0;
                    mR = c + 1 < cols && !CLEAN(r, c + 1) ? 3 : 0;
                    mT = r > 0 && !CLEAN(r - 1, c) ? 3 : 0;
                    mB = r + 1 < rows && !CLEAN(r + 1, c) ? 3 : 0;
#undef CLEAN
                }
                for (int yy = mT; yy < 16 - mB; yy++) {
                    size_t o = (size_t)(r * 16 + yy) * w + c * 16 + mL;
                    if (memcmp(y + o, s_prev + o, 16 - mL - mR)) {
                        s_map_bad++;
                        goto next;
                    }
                }
                {
                    int cL = mL ? 1 : 0, cR = mR ? 1 : 0, cT = mT ? 1 : 0, cB = mB ? 1 : 0;
                    const uint8_t *pu = s_prev + (size_t)w * h, *pv = pu + (size_t)w * h / 4;
                    for (int yy = cT; yy < 8 - cB; yy++) {
                        size_t o = (size_t)(r * 8 + yy) * (w / 2) + c * 8 + cL;
                        if (memcmp(u + o, pu + o, 8 - cL - cR) || memcmp(v + o, pv + o, 8 - cL - cR)) {
                            s_map_bad++;
                            goto next;
                        }
                    }
                }
            next:;
            }
        }
    }
    size_t ys = (size_t)w * h;
    if (!s_prev || s_pw != w || s_ph != h) {
        free(s_prev);
        s_prev = malloc(ys * 3 / 2);
        s_pw = w;
        s_ph = h;
    }
    memcpy(s_prev, y, ys);
    memcpy(s_prev + ys, u, ys / 4);
    memcpy(s_prev + ys + ys / 4, v, ys / 4);
}
#endif

int main(int argc, char **argv)
{
    int raw = 0, reorder = 0, keep = 0;
    unsigned lowmem = 0, strip = 0, use_map = 0, use_gate = 0, lim_mbs = 0, lim_refs = 0;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        const char *o = argv[a];
        if (!strcmp(o, "-m") && a + 1 < argc) {
            raw = !strcmp(argv[++a], "raw");
        } else if (!strcmp(o, "-R")) {
            reorder = 1;
        } else if (!strcmp(o, "-k")) {
            keep = 1;
#if H264BSD_LOWMEM
        } else if (!strcmp(o, "-l") && a + 1 < argc) {
            lowmem = (unsigned)strtoul(argv[++a], NULL, 0);
#endif
#if H264BSD_STRIP
        } else if (!strcmp(o, "-s") && a + 1 < argc) {
            strip = (unsigned)strtoul(argv[++a], NULL, 0);
#endif
#if H264BSD_CHANGED_MAP
        } else if (!strcmp(o, "-c")) {
            use_map = 1;
#endif
#if H264BSD_BUFFER_GATE
        } else if (!strcmp(o, "-g")) {
            use_gate = 1;
#endif
#if H264BSD_LIMITS
        } else if (!strcmp(o, "-L") && a + 1 < argc) {
            sscanf(argv[++a], "%u,%u", &lim_mbs, &lim_refs);
#endif
        } else {
            fprintf(stderr, "unknown option %s (or its knob not built in)\n", o);
            return 2;
        }
    }
    (void)lowmem, (void)strip, (void)use_map, (void)use_gate, (void)lim_mbs, (void)lim_refs;
    static span_t aus[20000];
    for (; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            perror(argv[a]);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        long fl = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *file = malloc(fl > 0 ? (size_t)fl : 1);
        size_t len = fread(file, 1, (size_t)(fl > 0 ? fl : 0), f);
        fclose(f);
        const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        size_t nl = strlen(name);
        size_t n = nl > 3 && !strcmp(name + nl - 3, ".au") ? split_records(file, len, aus, 20000)
                                                           : split_aus(file, len, aus, 20000);
        s_cur = s_peak = 0;
        s_gates = 0;
#if H264BSD_BUFFER_GATE
        s_ngated = 0;
#endif
        storage_t *dec = h264bsdAlloc();
        if (!dec || h264bsdInit(dec, !reorder) != 0) {
            printf("{\"file\": \"%s\", \"init\": \"failed\"}\n", name);
            return 1;
        }
        /* as muse_vdec.c */
        h264bsdSetMaxSize(dec, MAX_SIDE / 16, MAX_SIDE / 16);
#if H264BSD_LOWMEM
        if (lowmem) {
            h264bsdSetLowMem(dec, lowmem);
        }
#endif
#if H264BSD_STRIP
        if (strip) {
            h264bsdSetStrip(dec, strip, strip_row, NULL);
        }
#endif
#if H264BSD_LIMITS
        if (lim_mbs || lim_refs) {
            h264bsdSetLimits(dec, lim_mbs, lim_refs);
        }
#endif
#if H264BSD_BUFFER_GATE
        if (use_gate) {
            h264bsdSetBufferGate(dec, gate, NULL);
        }
        unsigned gate_bad = 0;
#endif
        printf("{\"file\": \"%s\", \"aus\": [", name);
        unsigned pics = 0, ndropped = 0, dropped[64];
        for (size_t k = 0; k < n; k++) {
            uint8_t *buf = malloc(aus[k].n ? aus[k].n : 1);
            memcpy(buf, aus[k].p, aus[k].n);
            if (!raw && h264bsdPictureStarted(dec)) {
                h264bsdAbandonPicture(dec);
            }
            int idr = keep && au_has_idr(buf, aus[k].n);   /* before the decoder edits buf */
            if (keep) {
                h264bsdAllowParamSetChange(dec, (u32)idr);
            }
            u8 *pic = NULL;
            u32 w = 0, h = 0;
#if H264BSD_STRIP
            s_rows_seen = 0;
            s_strip_bad = 0;
#endif
            u32 rc = aus[k].n ? h264bsdDecode(dec, buf, (u32)aus[k].n, &pic, &w, &h) : 99;
            uint32_t hash = 0;
            if (rc == H264BSD_PIC_RDY && (pic || strip)) {
                const uint8_t *y = pic, *u = NULL, *v = NULL;
                if (pic) {   /* NULL in strip mode */
                    u = pic + (size_t)w * h;
                    v = u + (size_t)w * h / 4;
#if H264BSD_LOWMEM
                    u8 *cb, *cr;
                    h264bsdPicturePlanes(dec, pic, &cb, &cr);
                    u = cb;
                    v = cr;
#endif
                }
#if H264BSD_STRIP
                if (strip) {
                    if (s_strip_bad || s_rows_seen != h / 16 || s_sw != w) {
                        rc = 98;   /* rows missing or out of order */
                    }
                    y = s_sy;
                    u = s_su;
                    v = s_sv;
                }
#endif
                if (rc == H264BSD_PIC_RDY) {
                    hash = planes_hash(y, u, v, w, h);
                    pics++;
#if H264BSD_CHANGED_MAP
                    if (use_map) {
                        check_map(dec, y, u, v, w, h);
                        const u8 *clean;
                        if ((h264bsdChangedMap(dec, &clean) & 4) && s_map_nidr < 64) {
                            s_map_idr[s_map_nidr++] = hash;
                        }
                    }
#endif
                }
            }
#if H264BSD_STRIP
            if (strip && rc != H264BSD_PIC_RDY) {
                s_rows_lost += s_rows_seen;   /* rows of a picture that failed */
            }
#endif
#if H264BSD_BUFFER_GATE
            /* a picture decoded only after its buffer was let go */
            if (use_gate && rc == H264BSD_PIC_RDY && !gated_out(pic)) {
                gate_bad++;
            }
#if TINYH264_CUSTOM_ALLOC
            if (use_gate && rc == H264BSD_PIC_RDY) {
                s_shown = pic;
            }
#endif
#endif
            if (rc == H264BSD_RDY && h264bsdPictureStarted(dec) && ndropped < 64) {
                dropped[ndropped++] = (unsigned)k;   /* the access unit ended inside a picture */
            }
            if (keep && rc == H264BSD_PIC_RDY && idr) {
                h264bsdKeepParamSets(dec);
            }
            if (!raw && rc != H264BSD_PIC_RDY && !(rc == H264BSD_RDY && !h264bsdPictureStarted(dec))) {
                h264bsdAbandonPicture(dec);
                if (keep) {
                    h264bsdRestoreParamSets(dec);
                }
            }
            free(buf);
            printf("%s[%u, %u]", k ? ", " : "", (unsigned)rc, (unsigned)hash);
        }
        printf("], \"pics\": %u, \"dropped\": [", pics);
        for (unsigned i = 0; i < ndropped; i++) {
            printf("%s%u", i ? ", " : "", dropped[i]);
        }
        printf("], \"peak\": %zu", s_peak);
#if H264BSD_LOWMEM
        {
            u32 rows = 0, refused = 0;
            h264bsdLowMemStats(dec, &rows, &refused);
            printf(", \"inplace_rows\": %u, \"refused\": %u", (unsigned)rows, (unsigned)refused);
        }
#endif
#if H264BSD_CHANGED_MAP
        if (use_map) {
            printf(", \"map_pics\": %u, \"map_clean\": %u, \"map_bad\": %u, \"map_idr\": [",
                   s_map_pics, s_map_clean, s_map_bad);
            for (unsigned i = 0; i < s_map_nidr; i++) {
                printf("%s%u", i ? ", " : "", (unsigned)s_map_idr[i]);
            }
            printf("]");
            s_map_pics = s_map_clean = s_map_bad = s_map_nidr = 0;
            free(s_prev);
            s_prev = NULL;
        }
#endif
#if H264BSD_STRIP
        if (strip) {
            printf(", \"rows_lost\": %u", (unsigned)s_rows_lost);
            s_rows_lost = 0;
        }
#endif
        h264bsdShutdown(dec);
        h264bsdFree(dec);
#if H264BSD_BUFFER_GATE
        if (use_gate) {
            printf(", \"gates\": %u, \"gate_bad\": %u", s_gates, gate_bad);
#if TINYH264_CUSTOM_ALLOC
            printf(", \"free_bad\": %u", s_free_bad);
            s_free_bad = 0;
            s_shown = NULL;
#endif
        }
#endif
        printf(", \"leak\": %zu}\n", s_cur);
        fflush(stdout);
        free(file);
    }
    return 0;
}
