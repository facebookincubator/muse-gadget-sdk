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
 * OPT: the packed 4-lane operations h264bsd_interp_packed4.c is written in
 * (TINYH264_PACKED4_MC), as plain C: this header is their reference model,
 * so the knob builds and runs bit-exact on any little-endian host. A port whose core has
 * such instructions supplies its own header with the same names and results
 * (-DTINYH264_PACKED4_OPS_H='"my_ops.h"'): each operation below is one or two
 * instructions on a core with 4 x 8-bit multiplies into 4 x 16-bit lanes.
 *
 *   p4_t                four signed 16-bit lanes (here a 64-bit integer, lane
 *                       k in bits 16k..16k+15); also two signed 32-bit lanes
 *                       for HMUL / HMAC (lane k in bits 32k..32k+31)
 *   mul4(d, c)          lane k = byte k of d (unsigned) * byte k of c (signed)
 *   mac4(a, d, c)       a + mul4(d, c), lane by lane (no lane overflows here)
 *   rnd5_pack(a)        lane k: (lane + 16) >> 5, saturated to 0..255, as byte k
 *   RND6(a)             lane k: (lane + 32) >> 6, as lanes
 *   PACKB(a)            lane k saturated to 0..255, as byte k
 *   align8(hi, lo)      bytes 1..4 of the 8-byte value hi:lo (a funnel shift);
 *   align16, align24    bytes 2..5, 3..6
 *   HMUL(x, N)          two 32-bit lanes: the signed 16-bit halves of x (low,
 *                       high) times the constant N (a literal)
 *   HMAC(a, x, N)       a += HMUL(x, N), lane by lane (a statement)
 *
 * Every lane value the kernels produce fits its lane: no operation here
 * overflows, so a port's saturating or wrapping forms give the same results.
 */

#ifndef H264BSD_PACKED4_OPS_H
#define H264BSD_PACKED4_OPS_H

#include "basetype.h"

typedef unsigned long long p4_t;

#define P4_AI static inline __attribute__((always_inline))

P4_AI p4_t p4_lanes(i32 l0, i32 l1, i32 l2, i32 l3)
{
    return (p4_t)(u16)l0 | (p4_t)(u16)l1 << 16 | (p4_t)(u16)l2 << 32 | (p4_t)(u16)l3 << 48;
}

P4_AI i32 p4_lane(p4_t a, int k)
{
    return (i16)(u16)(a >> (16 * k));
}

P4_AI p4_t mul4(u32 d, u32 c)
{
    return p4_lanes((i32)(d & 0xff) * (i8)(u8)c, (i32)((d >> 8) & 0xff) * (i8)(u8)(c >> 8),
                    (i32)((d >> 16) & 0xff) * (i8)(u8)(c >> 16), (i32)(d >> 24) * (i8)(u8)(c >> 24));
}

P4_AI p4_t mac4(p4_t a, u32 d, u32 c)
{
    p4_t m = mul4(d, c);
    return p4_lanes(p4_lane(a, 0) + p4_lane(m, 0), p4_lane(a, 1) + p4_lane(m, 1),
                    p4_lane(a, 2) + p4_lane(m, 2), p4_lane(a, 3) + p4_lane(m, 3));
}

P4_AI p4_t p4_rshr(p4_t a, int n)
{
    i32 r = 1 << (n - 1);
    return p4_lanes((p4_lane(a, 0) + r) >> n, (p4_lane(a, 1) + r) >> n, (p4_lane(a, 2) + r) >> n,
                    (p4_lane(a, 3) + r) >> n);
}

P4_AI u32 p4_packb(p4_t a)
{
    u32 r = 0;
    for (int k = 0; k < 4; k++) {
        i32 v = p4_lane(a, k);
        r |= (u32)(v < 0 ? 0 : v > 255 ? 255 : v) << (8 * k);
    }
    return r;
}

P4_AI u32 rnd5_pack(p4_t a)
{
    return p4_packb(p4_rshr(a, 5));
}

P4_AI u32 align8(u32 hi, u32 lo)
{
    return (lo >> 8) | (hi << 24);
}

P4_AI u32 align16(u32 hi, u32 lo)
{
    return (lo >> 16) | (hi << 16);
}

P4_AI u32 align24(u32 hi, u32 lo)
{
    return (lo >> 24) | (hi << 8);
}

#define RND6(a) p4_rshr((a), 6)
#define PACKB(a) p4_packb(a)
#define HMUL(x, N) ((p4_t)(u32)((i32)(i16)(u16)(x) * (N)) | \
                    (p4_t)(u32)((i32)(i16)(u16)((x) >> 16) * (N)) << 32)
#define HMAC(a, x, N) ((a) = (p4_t)(u32)((i32)(u32)(a) + (i32)(i16)(u16)(x) * (N)) | \
                       (p4_t)(u32)((i32)(u32)((a) >> 32) + (i32)(i16)(u16)((x) >> 16) * (N)) << 32)

#endif /* H264BSD_PACKED4_OPS_H */
