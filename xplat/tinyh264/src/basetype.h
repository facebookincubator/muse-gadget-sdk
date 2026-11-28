/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef BASETYPE_H_INCLUDED
#define BASETYPE_H_INCLUDED


#ifdef __arm
#define VOLATILE volatile
#else
#define VOLATILE
#endif

typedef unsigned char   u8;
typedef signed char     i8;
typedef unsigned short  u16;
typedef signed short    i16;
typedef unsigned int    u32;
typedef signed int      i32;

#if defined(VC1SWDEC_16BIT) || defined(MP4ENC_ARM11)
typedef unsigned short  u16x;
typedef signed short    i16x;
#else
typedef unsigned int    u16x;
typedef signed int      i16x;
#endif


/* OPT: TINYH264_MVE=1 (Arm Helium) takes effect only where the compiler
 * targets it; elsewhere the knob builds the C. Like the other backend
 * switches (TINYH264_DSP_MC, TINYH264_PACKED4_MC), 0 is off. */
#if defined(TINYH264_MVE) && TINYH264_MVE && defined(__ARM_FEATURE_MVE)
#define H264BSD_MVE 1
#endif

/* OPT: H264BSD_COLD_LEVEL compiles small or rare functions for size (GCC's
 * optimize("Os") attribute; GCC documents it as a debugging aid, so check
 * that the port's own flags such as its stack protection survive in them;
 * other compilers ignore it) while the rest keeps the build's -O level: 1
 * the ones marked H264BSD_COLD_FN (the neighbour set-up once per sequence,
 * an error's slice marking, strip mode's once-per-row work, and some that do
 * run per macroblock: the luma and chroma DC transforms, a motion vector's
 * prediction from its neighbours), 2 also H264BSD_COLD_FN2 (once per slice
 * or picture: the slice header and its checks, the reference picture
 * marking, the DPB's set-up and output). 0, the default, changes nothing.
 * The same pictures at every level. */
#ifndef H264BSD_COLD_LEVEL
#define H264BSD_COLD_LEVEL 0
#endif
#if H264BSD_COLD_LEVEL >= 1 && defined(__GNUC__) && !defined(__clang__)
#define H264BSD_COLD_FN __attribute__((optimize("Os")))
#else
#define H264BSD_COLD_FN
#endif
#if H264BSD_COLD_LEVEL >= 2 && defined(__GNUC__) && !defined(__clang__)
#define H264BSD_COLD_FN2 __attribute__((optimize("Os")))
#else
#define H264BSD_COLD_FN2
#endif

#ifndef NULL
#ifdef  __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

/* OPT: a port with tightly-coupled (or otherwise fast) RAM names its
 * sections here, as string macros (-DH264BSD_HOT_SECTION='".itcm.h264"'):
 * the code run for every macroblock (H264BSD_HOT) and the tables it reads
 * (H264BSD_HOT_DATA) go there; its linker script places them (and copies
 * them from flash where it must). Without them the markers are empty. The
 * ESP-IDF component places whole objects with linker.lf instead. */
#if defined(H264BSD_HOT_SECTION)
#define H264BSD_HOT __attribute__((section(H264BSD_HOT_SECTION)))
#else
#define H264BSD_HOT
#endif
#if defined(H264BSD_HOT_DATA_SECTION)
#define H264BSD_HOT_DATA __attribute__((section(H264BSD_HOT_DATA_SECTION)))
#else
#define H264BSD_HOT_DATA
#endif

/* OPT: H264BSD_CLZ counts leading zeros with __builtin_clz and decodes
 * Exp-Golomb codes inline. On unless the target is known to lack an
 * instruction for it (RISC-V without Zbb, Arm cores without CLZ), where
 * __builtin_clz is a library call and the upstream code stays; a port whose
 * core has none sets it to 0. The same results either way. */
#ifndef H264BSD_CLZ
#if (defined(__riscv) && !defined(__riscv_zbb)) || \
    (defined(__arm__) && !defined(__ARM_FEATURE_CLZ))
#define H264BSD_CLZ 0
#else
#define H264BSD_CLZ 1
#endif
#endif

#endif  /* BASETYPE_H_INCLUDED */
