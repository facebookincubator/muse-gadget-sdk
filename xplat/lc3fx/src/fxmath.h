/******************************************************************************
 *
 *  Copyright 2026 Meta Platforms, Inc. and affiliates.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*
 * lc3fx fixed-point primitives.
 *
 * The workhorse is `lc3fx_mulh()`: the high 32 bits of a signed 32x32
 * product. It maps to one instruction on every target of interest
 * (RV32IM `mulh`, Arm `smmul`, Xtensa `mulsh`), so Q31 coefficients are
 * applied as `mulh(x, c)` = x * c / 2, the factor 1/2 being folded in the
 * block exponents of the callers.
 */

#ifndef __LC3FX_MATH_H
#define __LC3FX_MATH_H

#include <stdint.h>
#include <stdbool.h>

/**
 * High part of signed / unsigned 32x32 products
 */
static inline int32_t lc3fx_mulh(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> 32);
}

static inline uint32_t lc3fx_mulhu(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * b) >> 32);
}

/**
 * Exact Q31 product (64 bits intermediate), for precision paths
 */
static inline int32_t lc3fx_mul31(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> 31);
}

/**
 * OPT: LC3FX_XPULP=1, on RISC-V cores with Xpulp: the leading-zero count
 * through fl1 and the 16-bit saturation through clip (xplat/include/
 * rv_xpulp.h; build with -DRV_XPULP=1 and xplat/include on the include
 * path, or the header's plain-C twins run, with the same results)
 */
#if defined(LC3FX_XPULP) && LC3FX_XPULP
#include "rv_xpulp.h"
#ifndef LC3FX_SAT16
#define LC3FX_SAT16(v) rv_xpulp_clip16(v)
#endif
#endif

/**
 * Count leading zeros, 32 for 0
 */
static inline int lc3fx_clz(uint32_t x) {
#if defined(LC3FX_XPULP) && LC3FX_XPULP
    return x ? 31 - rv_xpulp_fl1(x) : 32;
#elif defined(LC3FX_SWCLZ) && LC3FX_SWCLZ
    /* No count instruction: branchless and inline (libgcc's __clzsi2 is a
     * call, with branches). Three halvings, then the top nibble through
     * a 2-bit per entry constant (the entry of 0 counts 3, corrected for
     * x = 0 at the end) */
    int n, s;
    n  = s = (x < 0x10000u) << 4;      x <<= s;
    n += s = (x < 0x1000000u) << 3;    x <<= s;
    n += s = (x < 0x10000000u) << 2;   x <<= s;
    n += (0x000055afu >> ((x >> 28) << 1)) & 3;
    return n + (x == 0);
#else
    return x ? __builtin_clz(x) : 32;
#endif
}

static inline int lc3fx_clz64(uint64_t x) {
    return (x >> 32) ? lc3fx_clz((uint32_t)(x >> 32)) :
                       32 + lc3fx_clz((uint32_t)x);
}

/**
 * Number of significant bits of a magnitude (0 for 0)
 */
static inline int lc3fx_nbits(uint32_t m) {
    return 32 - lc3fx_clz(m);
}

/**
 * Saturation
 */
static inline int32_t lc3fx_sat32(int64_t v) {
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

#ifndef LC3FX_SAT16
static inline int32_t lc3fx_sat16(int32_t v) {
    /* In range when v + 2^15 fits 16 bits: one test, out of the way
     * (a taken branch costs on cores without prediction) */
    if (__builtin_expect((uint32_t)(v + 32768) >> 16, 0))
        return (v >> 31) ^ 32767;
    return v;
}
#define LC3FX_SAT16(v) lc3fx_sat16(v)
#endif

/**
 * Shift by a signed amount: left (saturated) when `s > 0`,
 * right (arithmetic, floor) otherwise
 */
static inline int32_t lc3fx_shl_sat(int32_t x, int s) {
    if (s <= 0)
        return s > -32 ? x >> -s : (x < 0 ? -1 : 0);
    if (s > 31)
        return x > 0 ? INT32_MAX : x < 0 ? INT32_MIN : 0;
    int32_t lim = INT32_MAX >> s;
    return x > lim ? INT32_MAX : x < -lim - 1 ? INT32_MIN : (int32_t)((uint32_t)x << s);
}

/**
 * Left shifts of signed values, done on the unsigned type: a left shift of a
 * negative value is undefined in C. The caller keeps the result in range
 */
#define LC3FX_SHL(x, n)   ((int32_t)((uint32_t)(x) << (n)))
#define LC3FX_SHL64(x, n) ((int64_t)((uint64_t)(x) << (n)))

static inline int32_t lc3fx_shift(int32_t x, int s) {
    return s >= 0 ? (int32_t)((uint32_t)x << s) :
           s > -32 ? x >> -s : (x < 0 ? -1 : 0);
}

/**
 * OPT: LC3FX_MERGE_TABLES=1 (GCC, ELF targets): every unit that inlines
 * lc3fx_log2_q16_u32() or lc3fx_pow2_q27() keeps its static table, as
 * before, but in a mergeable section ("aM") whose entity is the whole
 * table, so the linker keeps one copy of the identical tables. The same
 * definitions and code; off (the default), nothing changes.
 */
#if defined(LC3FX_MERGE_TABLES) && LC3FX_MERGE_TABLES && \
    defined(__GNUC__) && !defined(__clang__) && defined(__ELF__)
#if defined(__arm__)
#define LC3FX_MERGE_ARGS_(bytes) ",\"aM\",%progbits," #bytes " @"
#elif defined(__aarch64__)
#define LC3FX_MERGE_ARGS_(bytes) ",\"aM\",@progbits," #bytes " //"
#else
#define LC3FX_MERGE_ARGS_(bytes) ",\"aM\",@progbits," #bytes " #"
#endif
/* The flags the compiler appends to the section name land in the comment */
#define LC3FX_MERGE(name, bytes) \
    __attribute__((aligned(4), section(".rodata." name LC3FX_MERGE_ARGS_(bytes))))
#define LC3FX_LOG2_PAD 1   /* an entity is whole words: 33 entries -> 34, never read */
#else
#define LC3FX_MERGE(name, bytes)
#define LC3FX_LOG2_PAD 0
#endif

/**
 * log2 of an unsigned 32 / 64 bits integer, in Q16
 * Linear interpolation over 32 segments, max abs error ~2e-4
 * Return a large negative value (-2^30) for 0
 */
static inline int32_t lc3fx_log2_q16_u32(uint32_t x)
{
    static const uint16_t t[33 + LC3FX_LOG2_PAD] LC3FX_MERGE("lc3fx_log2_q16", 68) = {
            0,  2909,  5732,  8473, 11136, 13727, 16248, 18704, 21098,
        23433, 25711, 27936, 30109, 32234, 34312, 36346, 38336, 40286,
        42196, 44068, 45904, 47705, 49472, 51207, 52911, 54584, 56229,
        57845, 59434, 60997, 62534, 64047, 65535 };

    if (!x)
        return -(1 << 30);

    int z = lc3fx_clz(x);
    uint32_t m = x << z;                     /* 1.31 in [1, 2) */
    int i = (m >> 26) & 31;
    int32_t f = (m >> 10) & 0xffff;          /* fraction within the segment Q16 */
    int32_t y = t[i] + (((int32_t)(t[i+1] - t[i]) * f) >> 16);

    return ((31 - z) << 16) + y;
}

static inline int32_t lc3fx_log2_q16_u64(uint64_t x)
{
    if (x >> 32) {
        int z = lc3fx_clz((uint32_t)(x >> 32));
        return lc3fx_log2_q16_u32((uint32_t)(x >> (32 - z))) + ((32 - z) << 16);
    }
    return lc3fx_log2_q16_u32((uint32_t)x);
}

/**
 * 2^x, for x in Q27 (range -256 to 256)
 * mant            Return the mantissa in Q30, in range [1, 2[
 * return          The exponent, such that 2^x = mant * 2^(e-30)
 *
  * Table 2^(k/32) and cubic polynomial on the remainder (32 bits
 * products only): relative error < 4e-9
 */
static inline int lc3fx_pow2_q27(int32_t x, int32_t *mant)
{
    static const int32_t t[32] LC3FX_MERGE("lc3fx_pow2_q27", 128) = {
        1073741824, 1097253708, 1121280436, 1145833280, 1170923762,
        1196563654, 1222764986, 1249540052, 1276901417, 1304861917,
        1333434672, 1362633090, 1392470869, 1422962010, 1454120821,
        1485961921, 1518500250, 1551751076, 1585730000, 1620452965,
        1655936265, 1692196547, 1729250827, 1767116489, 1805811301,
        1845353420, 1885761398, 1927054196, 1969251188, 2012372174,
        2056437387, 2101467502 };

    const int32_t c1 = 1488522236;   /* ln2        Q31 */
    const int32_t c2 =  515882496;   /* ln2^2 / 2  Q31 */
    const int32_t c3 =  119194166;   /* ln2^3 / 6  Q31 */

        int e = x >> 27;
    int k = (x >> 22) & 31;
    int32_t r = (x & ((1 << 22) - 1)) << 9;   /* remainder in Q36, < 1/32 */

    int32_t p = c2 + (lc3fx_mulh(c3, r) >> 4);
    p = c1 + (lc3fx_mulh(p, r) >> 4);
    p = lc3fx_mulh(p, r) >> 4;                /* 2^r - 1, Q31 */

    *mant = t[k] + (lc3fx_mulh(t[k], p) << 1);
    return e;
}

/**
 * Gain 10^(g/28) as mantissa Q30 / exponent (see `lc3fx_pow2_q27()`)
 */
static inline int lc3fx_gain_pow(int g, int32_t *mant)
{
    const int64_t k = 254778081;    /* log2(10) / 28 in Q31 */
    int64_t x = ((int64_t)g * k + (1 << 3)) >> 4;

    /* In 64 bits: the log2 reaches +/-30, past the +/-16 of a Q27 int32 */
    return (int)(x >> 27) +
        lc3fx_pow2_q27((int32_t)(x & ((1 << 27) - 1)), mant);
}

#endif /* __LC3FX_MATH_H */
