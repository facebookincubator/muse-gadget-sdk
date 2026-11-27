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

#ifndef NULL
#ifdef  __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
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
