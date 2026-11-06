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
 * tinyh264's S31 SIMD interpolation (src/h264bsd_interp_pie_s31.S) as a model
 * of its lane operations, on the host: the model against the C on random
 * blocks (every luma 6-tap position it takes, the three chroma cases), then
 * the model standing in for the assembly behind h264bsd_reconstruct.c's
 * hooks (built with TINYH264_PIE_S31), whose first-use check must pass.
 * QEMU can't run PIE; this checks the arithmetic and the C glue.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "basetype.h"
#include "h264bsd_reconstruct.h"
typedef struct { uint8_t b[16]; } q_t;
static q_t ldu(const uint8_t *p) { q_t q; memcpy(q.b, p, 16); return q; }
static int16_t L(q_t q, int i) { int16_t v; memcpy(&v, q.b + 2 * i, 2); return v; }
static void S(q_t *q, int i, int16_t v) { memcpy(q->b + 2 * i, &v, 2); }
static uint32_t W(q_t q, int i) { uint32_t v; memcpy(&v, q.b + 4 * i, 4); return v; }
static void SW(q_t *q, int i, uint32_t v) { memcpy(q->b + 4 * i, &v, 4); }
static q_t andq(q_t a, q_t b) { for (int i = 0; i < 16; i++) a.b[i] &= b.b[i]; return a; }
static q_t orq(q_t a, q_t b) { for (int i = 0; i < 16; i++) a.b[i] |= b.b[i]; return a; }
static q_t vsr(q_t a, int s) { for (int i = 0; i < 4; i++) SW(&a, i, W(a, i) >> s); return a; }
static q_t vsl(q_t a, int s) { for (int i = 0; i < 4; i++) SW(&a, i, W(a, i) << s); return a; }
static q_t vadd(q_t a, q_t b) { for (int i = 0; i < 8; i++) S(&a, i, (int16_t)(L(a, i) + L(b, i))); return a; }
static q_t vsub(q_t a, q_t b) { for (int i = 0; i < 8; i++) S(&a, i, (int16_t)(L(a, i) - L(b, i))); return a; }
static q_t vmul(q_t a, q_t b, int s) { for (int i = 0; i < 8; i++) S(&a, i, (int16_t)((L(a, i) * L(b, i)) >> s)); return a; }
static q_t vmax(q_t a, q_t b) { for (int i = 0; i < 8; i++) S(&a, i, L(a, i) > L(b, i) ? L(a, i) : L(b, i)); return a; }
static q_t vmin(q_t a, q_t b) { for (int i = 0; i < 8; i++) S(&a, i, L(a, i) < L(b, i) ? L(a, i) : L(b, i)); return a; }
static q_t k16(int v) { q_t q; for (int i = 0; i < 8; i++) S(&q, i, v); return q; }

/* six taps of one parity -> clipped result */
static q_t six(q_t A, q_t B, q_t C, q_t D, q_t E, q_t F)
{
    q_t s1 = vadd(C, D), p = vadd(vadd(vsl(s1, 4), vsl(s1, 2)), vadd(vadd(A, F), k16(16)));
    q_t s2 = vadd(B, E), n = vadd(vsl(s2, 2), s2);
    q_t r = vmul(vsub(p, n), k16(1), 5);
    return vmin(vmax(r, k16(0)), k16(255));
}
static q_t avg(q_t r, q_t i) { return vmul(vadd(vadd(r, i), k16(1)), k16(1), 1); }
static void store(uint8_t *dst, q_t e, q_t o, int w) { q_t r = orq(e, vsl(o, 8)); memcpy(dst, r.b, w); }

/* src: tap row 0 of output row 0; off -1: half, else quarter average with row r+2+off */
static void ver(const uint8_t *src, int stride, uint8_t *mb, int w, int h, int off)
{
    q_t scr[21][2], M = k16(0xff);
    for (int r = 0; r < h + 5; r++) {
        q_t q = ldu(src + r * stride);
        scr[r][0] = andq(q, M);
        scr[r][1] = andq(vsr(q, 8), M);
    }
    for (int r = 0; r < h; r++) {
        q_t res[2];
        for (int p = 0; p < 2; p++) {
            res[p] = six(scr[r][p], scr[r + 1][p], scr[r + 2][p], scr[r + 3][p], scr[r + 4][p], scr[r + 5][p]);
            if (off >= 0)
                res[p] = avg(res[p], scr[r + 2 + off][p]);
        }
        store(mb + r * 16, res[0], res[1], w);
    }
}
static void hor(const uint8_t *src, int stride, uint8_t *mb, int w, int h, int off)
{
    q_t M = k16(0xff);
    for (int r = 0; r < h; r++) {
        const uint8_t *row = src + r * stride;
        q_t E[4], O[4];
        for (int k = 0; k < 4; k++) {
            q_t q = ldu(row + 2 * k);
            E[k] = andq(q, M);
            O[k] = andq(vsr(q, 8), M);
        }
        q_t re = six(E[0], O[0], E[1], O[1], E[2], O[2]);
        q_t ro = six(O[0], E[1], O[1], E[2], O[2], E[3]);
        if (off == 0) { re = avg(re, E[1]); ro = avg(ro, O[1]); }
        if (off == 1) { re = avg(re, O[1]); ro = avg(ro, E[2]); }
        store(mb + r * 16, re, ro, w);
    }
}


/* chroma: r = (wA*A + wB*B + wC*C + wD*D + 32) >> 6, A/B row r cols x/x+1, C/D row r+1 */
static void chroma(const uint8_t *src, int stride, uint8_t *dst, int h, int rows, const int wt[4])
{
    q_t scr[9][4], M = k16(0xff);
    for (int r = 0; r < rows; r++) {
        q_t q0 = ldu(src + r * stride), q1 = ldu(src + r * stride + 1);
        scr[r][0] = andq(q0, M); scr[r][1] = andq(vsr(q0, 8), M);
        scr[r][2] = andq(q1, M); scr[r][3] = andq(vsr(q1, 8), M);
    }
    for (int r = 0; r < h; r++) {
        q_t res[2];
        for (int p = 0; p < 2; p++) {
            q_t a = vmul(scr[r][p], k16(wt[0]), 0), b = vmul(scr[r][2 + p], k16(wt[1]), 0);
            q_t c = vmul(rows > h ? scr[r + 1][p] : k16(0), k16(wt[2]), 0), d = vmul(rows > h ? scr[r + 1][2 + p] : k16(0), k16(wt[3]), 0);
            res[p] = vmul(vadd(vadd(vadd(a, b), vadd(c, d)), k16(32)), k16(1), 6);
        }
        store(dst + r * 8, res[0], res[1], 8);
    }
}
static int chroma_test(void)
{
    enum { WD = 40, HT = 40 };
    static uint8_t pic[WD * HT * 2 + 64];
    int bad = 0, n = 0;
    for (int it = 0; it < 20000; it++) {
        for (int i = 0; i < WD * HT * 2; i++) pic[i] = (it & 1) ? rand() : (rand() & 1) * 255;
        int xf = rand() % 8, yf = rand() % 8, h = (rand() & 1) ? 8 : 4;
        int x0 = rand() % (WD - 10), y0 = rand() % (HT - 10);
        uint8_t a[128], b[128];
        for (int f = 0; f < 3; f++) {
            memset(a, 0x55, 128); memset(b, 0x55, 128);
            int wt[4], rows;
            if (f == 0) { if (!xf) continue; h264bsdInterpolateChromaHor(pic, a, x0, y0, WD, HT, xf, 8, h); wt[0] = 8 * (8 - xf); wt[1] = 8 * xf; wt[2] = wt[3] = 0; rows = h; }
            else if (f == 1) { if (!yf) continue; h264bsdInterpolateChromaVer(pic, a, x0, y0, WD, HT, yf, 8, h); wt[0] = 8 * (8 - yf); wt[2] = 8 * yf; wt[1] = wt[3] = 0; rows = h + 1; }
            else { if (!xf || !yf) continue; h264bsdInterpolateChromaHorVer(pic, a, x0, y0, WD, HT, xf, yf, 8, h); wt[0] = (8 - xf) * (8 - yf); wt[1] = xf * (8 - yf); wt[2] = (8 - xf) * yf; wt[3] = xf * yf; rows = h + 1; }
            for (int comp = 0; comp < 2; comp++)
                chroma(pic + (comp * HT + y0) * WD + x0, WD, b + comp * 64, h, rows, wt);
            n++;
            if (memcmp(a, b, 128)) { if (bad++ < 5) printf("chroma mismatch f%d h%d xf%d yf%d\n", f, h, xf, yf); }
        }
    }
    printf("chroma %d/%d blocks match\n", n - bad, n);
    return bad;
}


/* the asm's signatures, on the model: lets h264bsd_reconstruct.c's PIE glue
 * (hooks, FillBlock paths, weights, selftest) run on the host */
void h264bsdPieVer(const uint8_t *src, uint32_t stride, uint8_t *mb, uint32_t w, uint32_t h, int32_t off, uint8_t *scr, const int16_t *k)
{ (void)scr; (void)k; ver(src, stride, mb, w, h, off); }
void h264bsdPieHor(const uint8_t *src, uint32_t stride, uint8_t *mb, uint32_t w, uint32_t h, int32_t off, uint8_t *scr, const int16_t *k)
{ (void)scr; (void)k; hor(src, stride, mb, w, h, off); }
int glue_calls;
void h264bsdPieChroma(const uint8_t *src, uint32_t stride, uint8_t *dst, uint32_t h, uint32_t rows, const int16_t *wt, uint8_t *scr, const int16_t *k)
{ (void)scr; (void)k; int w4[4] = { wt[0], wt[8], wt[16], wt[24] }; glue_calls++;
  if (wt[32] != 32 || wt[40] != 1) printf("bad wt tail\n");
  chroma(src, stride, dst, h, rows, w4); }

static int model_main(void)
{
    int cbad = chroma_test();
    enum { WD = 64, HT = 64 };
    static uint8_t pic[WD * HT + 64];
    srand(1);
    int bad = 0, n = 0;
    for (int it = 0; it < 20000; it++) {
        for (int i = 0; i < WD * HT; i++) pic[i] = (it & 1) ? rand() : (rand() & 1) * 255;
        int w = (rand() & 1) ? 16 : 8, h = (int[]){4, 8, 16}[rand() % 3];
        int x0 = rand() % (WD - 24), y0 = rand() % (HT - 24);
        uint8_t a[256], b[256];
        for (int f = 0; f < 6; f++) {
            memset(a, 0xAA, 256); memset(b, 0xAA, 256);
            switch (f) {
            case 0: h264bsdInterpolateVerHalf(pic, a, x0, y0, WD, HT, w, h); ver(pic + y0 * WD + x0, WD, b, w, h, -1); break;
            case 1: case 2: h264bsdInterpolateVerQuarter(pic, a, x0, y0, WD, HT, w, h, f - 1); ver(pic + y0 * WD + x0, WD, b, w, h, f - 1); break;
            case 3: h264bsdInterpolateHorHalf(pic, a, x0, y0, WD, HT, w, h); hor(pic + y0 * WD + x0, WD, b, w, h, -1); break;
            case 4: case 5: h264bsdInterpolateHorQuarter(pic, a, x0, y0, WD, HT, w, h, f - 4); hor(pic + y0 * WD + x0, WD, b, w, h, f - 4); break;
            }
            n++;
            if (memcmp(a, b, 256)) { if (bad++ < 5) printf("mismatch f%d w%d h%d\n", f, w, h); }
        }
    }
    printf("%d/%d blocks match\n", n - bad, n);
    return bad != 0 || cbad;
}

int main(void)
{
    int bad = model_main();
    /* the first inter call runs h264bsd_reconstruct.c's check through the hooks */
    static u8 pic[64 * 64], mb[256];
    h264bsdInterpolateVerHalf(pic, mb, 4, 4, 64, 64, 16, 16);
    printf("glue: state %d, chroma calls %d\n", (int)h264bsdPieState(), glue_calls);
    if (bad || h264bsdPieState() != 1 || glue_calls == 0)
        return 1;
    printf("PASS tinyh264_pie\n");
    return 0;
}
