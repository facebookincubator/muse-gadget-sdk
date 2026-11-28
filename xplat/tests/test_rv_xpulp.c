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
 * rv_xpulp.h against a model of the CV32E40P 1.0 manual's definitions
 * ("General ALU Operations", "Bit Manipulation Operations"), across edge
 * values. On a host the header's plain-C twins run; built with RV_XPULP=1
 * for a core that has the instructions, the same checks run them. The model
 * takes the encoded Is2 and the build's RV_XPULP_CLIP_BIAS, the helpers the
 * bound itself.
 * Prints "rv_xpulp: N checks, M failed".
 */

#include <stdio.h>
#include <string.h>

#include "rv_xpulp.h"

static long checks, failed;

static void expect(long long got, long long want, const char *what, long long a, long long b)
{
    checks++;
    if (got != want) {
        if (failed++ < 20)
            printf("FAIL %s(%lld, %lld): %lld, want %lld\n", what, a, b, got, want);
    }
}

/* --- The manual, as written. Is2 is the encoded field (0..31); bias 1 is
 * the manual's bound 2^(Is2-1), bias 0 a core's 2^Is2. --- */

static long long bound(int is2, int bias)
{
    int e = is2 - bias;   /* the manual's note: Is2 = 0 gives -1 and 0 */
    return e < 0 ? 0 : 1ll << e;
}

static int32_t model_clip(int32_t rs1, int is2, int bias)
{
    long long lo = is2 - bias < 0 ? -1 : -bound(is2, bias), hi = bound(is2, bias) - 1;
    if (hi < 0) hi = 0;
    if (rs1 <= lo) return (int32_t)lo;
    if (rs1 >= hi) return (int32_t)hi;
    return rs1;
}

static int32_t model_clipu(int32_t rs1, int is2, int bias)
{
    long long hi = bound(is2, bias) - 1;
    if (hi < 0) hi = 0;
    if (rs1 <= 0) return 0;
    if (rs1 >= hi) return (int32_t)hi;
    return rs1;
}

static int32_t model_fl1(uint32_t rs1)
{
    for (int i = 31; i >= 0; i--)
        if (rs1 >> i & 1) return i;
    return 32;
}

/* --- Inputs: the edges of every bound, the bits, and a pseudo-random run --- */

static uint32_t lcg = 12345;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg; }

static int inputs(int32_t *v)
{
    int n = 0;
    static const int32_t fixed[] = {
        0, 1, -1, 2, -2, 127, 128, 129, -127, -128, -129, 254, 255, 256, 257,
        -255, -256, -257, 32767, 32768, -32767, -32768, -32769, 65535, 65536,
        INT32_MAX, INT32_MIN, INT32_MAX - 1, INT32_MIN + 1, 0x55555555, (int32_t)0xaaaaaaaau,
    };
    memcpy(v, fixed, sizeof(fixed));
    n = (int)(sizeof(fixed) / sizeof(fixed[0]));
    for (int k = 0; k < 31; k++) {
        int32_t b = (int32_t)(1u << k);
        v[n++] = b; v[n++] = b - 1; v[n++] = b + 1;
        v[n++] = -b; v[n++] = -b - 1; v[n++] = -b + 1;
    }
    for (int i = 0; i < 2000; i++)
        v[n++] = (int32_t)rnd();
    for (int i = 0; i < 500; i++)
        v[n++] = (int32_t)(rnd() >> (rnd() & 31)) * ((rnd() & 1) ? -1 : 1);
    return n;
}

#define CLIP_K(k) do { \
    for (int i = 0; i < n; i++) { \
        expect(RV_XPULP_CLIPU(v[i], k), model_clipu(v[i], RV_XPULP_CLIP_IS2(k), RV_XPULP_CLIP_BIAS), "clipu" #k, v[i], k); \
        expect(RV_XPULP_CLIP(v[i], k), model_clip(v[i], RV_XPULP_CLIP_IS2(k), RV_XPULP_CLIP_BIAS), "clip" #k, v[i], k); \
    } } while (0)

int main(void)
{
    static int32_t v[4096];
    int n = inputs(v);

    /* The bound the helpers mean, whatever the core: clipu k -> [0, 2^k-1],
     * clip k -> [-2^k, 2^k-1] */
    for (int i = 0; i < n; i++) {
        int32_t x = v[i];
        expect(rv_xpulp_clipu8(x), x < 0 ? 0 : x > 255 ? 255 : x, "clipu8", x, 0);
        expect(rv_xpulp_clip8(x), x < -128 ? -128 : x > 127 ? 127 : x, "clip8", x, 0);
        expect(rv_xpulp_clip16(x), x < -32768 ? -32768 : x > 32767 ? 32767 : x, "clip16", x, 0);
    }

    /* Each bound the field can encode for this convention, against the manual */
    CLIP_K(0); CLIP_K(1); CLIP_K(2); CLIP_K(3); CLIP_K(4); CLIP_K(5); CLIP_K(6);
    CLIP_K(7); CLIP_K(8); CLIP_K(9); CLIP_K(10); CLIP_K(11); CLIP_K(12); CLIP_K(13);
    CLIP_K(14); CLIP_K(15); CLIP_K(16); CLIP_K(17); CLIP_K(18); CLIP_K(19); CLIP_K(20);
    CLIP_K(21); CLIP_K(22); CLIP_K(23); CLIP_K(24); CLIP_K(25); CLIP_K(26); CLIP_K(27);
    CLIP_K(28); CLIP_K(29); CLIP_K(30);

    for (int i = 0; i < n; i++) {
        int32_t a = v[i];
        uint32_t ua = (uint32_t)a;
        /* the manual: rs1 < 0 ? -rs1 : rs1, in 32-bit two's complement */
        expect(rv_xpulp_abs(a), (int32_t)(a < 0 ? 0u - ua : ua), "abs", a, 0);
        expect(rv_xpulp_fl1(ua), model_fl1(ua), "fl1", a, 0);
        for (int j = 0; j < n; j += 7) {
            int32_t b = v[j];
            expect(rv_xpulp_min(a, b), a < b ? a : b, "min", a, b);
            expect(rv_xpulp_max(a, b), a < b ? b : a, "max", a, b);
        }
    }

    printf("rv_xpulp: %ld checks, %ld failed (RV_XPULP=%d, RV_XPULP_CLIP_BIAS=%d)\n",
           checks, failed, RV_XPULP, RV_XPULP_CLIP_BIAS);
    return failed != 0;
}
