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
 * rv_xpulp.h: a few Xpulp instructions for RISC-V cores that have them, each
 * with a plain-C twin that gives the same result.
 *
 * Source: OpenHW Group, "CORE-V CV32E40P User Manual", release
 * cv32e40p_v1.0.0 (github.com/openhwgroup/core-v-docs, tag
 * cv32e40p_v1.0.0), chapter "CORE-V Instruction Set Extensions":
 *   - "Bit Manipulation Operations" and "Bit Manipulation Encoding"
 *     (fl1: opcode 0x33, funct7 0x08);
 *   - "General ALU Operations" and "General ALU Encoding" (abs, min, max:
 *     funct7 0x02; clip, clipu: funct7 0x0a, the bound in the rs2 field).
 * That manual names them cv.*; they are the PULP RI5CY core's p.*
 * instructions (Xpulp), which CV32E40P 1.0 kept. Later CV32E40P releases
 * moved them to other opcodes; this header has the 1.0 encodings only.
 *
 * RV_XPULP=1 (only from the build, -DRV_XPULP=1; never guessed from the
 * target) emits the instructions with `.insn`, so a stock GNU or LLVM
 * assembler builds them. Otherwise, and on every host, the plain-C twins
 * run. A codec built either way gives the same output; the host tests
 * (xplat/tests/test_rv_xpulp.c) check every twin against a model of the
 * manual's definitions.
 *
 * RV_XPULP_CLIP_BIAS: the manual's clip and clipu take an immediate Is2 and
 * clamp to [-2^(Is2-1), 2^(Is2-1)-1] and [0, 2^(Is2-1)-1]. A core whose
 * bound is 2^Is2 instead builds with -DRV_XPULP_CLIP_BIAS=0. The helpers
 * below take the bound's exponent k (clipu: [0, 2^k-1], clip:
 * [-2^k, 2^k-1]) and encode Is2 = k + RV_XPULP_CLIP_BIAS, so callers stay the
 * same on either kind of core. Default 1 (the manual).
 */
#ifndef RV_XPULP_H
#define RV_XPULP_H

#include <stdint.h>

#ifndef RV_XPULP
#define RV_XPULP 0
#endif

#ifndef RV_XPULP_CLIP_BIAS
#define RV_XPULP_CLIP_BIAS 1
#endif

#if RV_XPULP && !(defined(__riscv) && __riscv_xlen == 32)
#error "RV_XPULP=1 needs an RV32 target"
#endif

#if RV_XPULP_CLIP_BIAS != 0 && RV_XPULP_CLIP_BIAS != 1
#error "RV_XPULP_CLIP_BIAS is 1 (bound 2^(Is2-1), the manual) or 0 (bound 2^Is2)"
#endif

#define RV_XPULP_INLINE static inline __attribute__((always_inline))

/* The rs2 field of clip / clipu for a bound of 2^k (k a constant, 0..30). */
#define RV_XPULP_CLIP_IS2(k) ((k) + RV_XPULP_CLIP_BIAS)

#if RV_XPULP

/* clip / clipu as an I-type: funct7 (0x0a) and the bound field are the
 * 12-bit immediate, which the assembler checks. */
#define RV_XPULP_CLIPU(x, k) ({ int32_t _r; \
    __asm__(".insn i 0x33, 2, %0, %1, %2" : "=r"(_r) \
            : "r"((int32_t)(x)), "i"((0x0a << 5) | RV_XPULP_CLIP_IS2(k))); _r; })
#define RV_XPULP_CLIP(x, k) ({ int32_t _r; \
    __asm__(".insn i 0x33, 1, %0, %1, %2" : "=r"(_r) \
            : "r"((int32_t)(x)), "i"((0x0a << 5) | RV_XPULP_CLIP_IS2(k))); _r; })

#define RV_XPULP_R1(name, funct3, funct7, ret, arg) \
    RV_XPULP_INLINE ret name(arg a) { ret r; \
        __asm__(".insn r 0x33, " #funct3 ", " #funct7 ", %0, %1, x0" : "=r"(r) : "r"(a)); return r; }
#define RV_XPULP_R2(name, funct3, funct7, t) \
    RV_XPULP_INLINE t name(t a, t b) { t r; \
        __asm__(".insn r 0x33, " #funct3 ", " #funct7 ", %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }

RV_XPULP_R1(rv_xpulp_abs, 0, 0x02, int32_t, int32_t)
RV_XPULP_R2(rv_xpulp_min, 4, 0x02, int32_t)
RV_XPULP_R2(rv_xpulp_max, 6, 0x02, int32_t)
RV_XPULP_R1(rv_xpulp_fl1, 1, 0x08, int32_t, uint32_t)

#undef RV_XPULP_R1
#undef RV_XPULP_R2

#else /* the plain-C twins */

#define RV_XPULP_CLIPU(x, k) rv_xpulp_clipu_c_((int32_t)(x), (int32_t)((1u << (k)) - 1))
#define RV_XPULP_CLIP(x, k)  rv_xpulp_clip_c_((int32_t)(x), (int32_t)((1u << (k)) - 1))
RV_XPULP_INLINE int32_t rv_xpulp_clipu_c_(int32_t x, int32_t hi)
{ return x <= 0 ? 0 : x >= hi ? hi : x; }
RV_XPULP_INLINE int32_t rv_xpulp_clip_c_(int32_t x, int32_t hi)
{ return x <= -hi - 1 ? -hi - 1 : x >= hi ? hi : x; }

/* abs of INT32_MIN is INT32_MIN, as the instruction (two's complement) */
RV_XPULP_INLINE int32_t rv_xpulp_abs(int32_t a) { return a < 0 ? (int32_t)(0u - (uint32_t)a) : a; }
RV_XPULP_INLINE int32_t rv_xpulp_min(int32_t a, int32_t b) { return a < b ? a : b; }
RV_XPULP_INLINE int32_t rv_xpulp_max(int32_t a, int32_t b) { return a < b ? b : a; }
RV_XPULP_INLINE int32_t rv_xpulp_fl1(uint32_t a) { return a ? 31 - __builtin_clz(a) : 32; }

#endif /* RV_XPULP */

/* The clamps the codecs use */
RV_XPULP_INLINE int32_t rv_xpulp_clipu8(int32_t x) { return RV_XPULP_CLIPU(x, 8); }   /* 0..255 */
RV_XPULP_INLINE int32_t rv_xpulp_clip8(int32_t x) { return RV_XPULP_CLIP(x, 7); }     /* -128..127 */
RV_XPULP_INLINE int32_t rv_xpulp_clip16(int32_t x) { return RV_XPULP_CLIP(x, 15); }   /* -32768..32767 */

#endif /* RV_XPULP_H */
