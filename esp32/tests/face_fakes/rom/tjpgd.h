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
 * The ROM TJpgDec's API as main/mg_face_jpeg.c uses it, on the host: a real
 * JPEG decoder behind it (libjpeg, tests/face_fakes/tjpgd_libjpeg.c) that
 * keeps TJpgDec's rules: baseline only (progressive is JDR_FMT3), output in
 * MCU-sized RGB888 blocks, scale 0-3 = 1/1..1/8.
 */
#pragma once

#include <stdint.h>

typedef unsigned int UINT;
typedef uint8_t BYTE;

typedef enum {
    JDR_OK = 0,
    JDR_INTR,
    JDR_INP,
    JDR_MEM1,
    JDR_MEM2,
    JDR_PAR,
    JDR_FMT1,
    JDR_FMT2,
    JDR_FMT3,
} JRESULT;

typedef struct {
    uint16_t left, right, top, bottom;
} JRECT;

typedef struct JDEC {
    UINT width, height;
    void *device;
    void *host;   /* the fake's own state */
} JDEC;

JRESULT jd_prepare(JDEC *jd, UINT (*infunc)(JDEC *, BYTE *, UINT), void *pool, UINT sz_pool, void *dev);
JRESULT jd_decomp(JDEC *jd, UINT (*outfunc)(JDEC *, void *, JRECT *), BYTE scale);
