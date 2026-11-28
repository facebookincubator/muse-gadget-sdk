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

/*------------------------------------------------------------------------------

    Table of contents

    1. Include headers
    2. Module defines
    3. Data types
    4. Function prototypes

------------------------------------------------------------------------------*/

#ifndef H264SWDEC_CFG_H
#define H264SWDEC_CFG_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include <stdlib.h>
#include <memory.h>

/* OPT: knobs for ports to other platforms, each off by default (off, the
 * code is the same as without them). See ../README.md. */
#ifndef TINYH264_CUSTOM_ALLOC
#define TINYH264_CUSTOM_ALLOC 0     /* the port's allocator (h264bsd_util.h) */
#endif
#ifndef H264BSD_LIMITS
#define H264BSD_LIMITS 0            /* h264bsdSetLimits */
#endif
#ifndef H264BSD_BUFFER_GATE
#define H264BSD_BUFFER_GATE 0       /* h264bsdSetBufferGate */
#endif
#ifndef H264BSD_LEAN_VUI
#define H264BSD_LEAN_VUI 0          /* the small VUI parser (h264bsd_vui.c) */
#endif
#ifndef H264BSD_LOWMEM
#define H264BSD_LOWMEM 0            /* h264bsdSetLowMem (h264bsd_lowmem.c) */
#endif
#ifndef H264BSD_STRIP
#define H264BSD_STRIP 0             /* h264bsdSetStrip (h264bsd_strip.c) */
#endif
#ifndef H264BSD_ONEREF
#define H264BSD_ONEREF 0            /* in place keeping one reference (h264bsd_lowmem.c) */
#endif
#if H264BSD_ONEREF && !H264BSD_LOWMEM
#error "H264BSD_ONEREF needs H264BSD_LOWMEM"
#endif
#ifndef H264BSD_CHANGED_MAP
#define H264BSD_CHANGED_MAP 0       /* h264bsdChangedMap */
#endif
#ifndef H264BSD_CHANGED_MAP_MBS
#define H264BSD_CHANGED_MAP_MBS 1024    /* the largest picture with a map, macroblocks */
#endif
/* the chroma planes need not follow the luma plane (image_t.cbData) */
#define H264BSD_SPLIT_PLANES (H264BSD_LOWMEM || H264BSD_STRIP)
#if H264BSD_SPLIT_PLANES && defined(H264BSD_MVE)
/* its motion compensation reads a reference's chroma planes after its luma
 * (and neither mode is tested with its other paths); where TINYH264_MVE
 * builds the C, both modes work */
#error "H264BSD_LOWMEM and H264BSD_STRIP do not go with TINYH264_MVE yet"
#endif
#if H264BSD_SPLIT_PLANES && defined(H264DEC_OMXDL)
/* the OpenMAX DL paths address a picture's chroma after its luma */
#error "H264BSD_LOWMEM and H264BSD_STRIP do not go with H264DEC_OMXDL"
#endif

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

#define MAX_NUM_REF_PICS 16
#define MAX_NUM_SLICE_GROUPS 8
#define MAX_NUM_SEQ_PARAM_SETS 32
#define MAX_NUM_PIC_PARAM_SETS 256

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/


/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/

#endif /* #ifdef H264SWDEC_CFG_H */

