/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *  Copyright 2026 Meta Platforms, Inc. and affiliates (fixed-point port)
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

#ifndef __LC3_MDCT_H
#define __LC3_MDCT_H

#include "common.h"


/**
 * Forward MDCT transformation (integer)
 * dt, sr          Duration and samplerate
 * x, d            [-nd..ns-nd[ last input samples, [0..nd-1] delayed ones
 * y               Output `ns` coefficients
 * buf             Scratch buffer of `ns` words
 * return          Exponent `e` such that coefficient = y * 2^e
 *
 * The output magnitude is below 2^29.
 */
int lc3_mdct_forward(enum lc3_dt dt, enum lc3_srate sr,
    const int16_t *x, int16_t *d, int32_t *y, int32_t *buf);

/**
 * Inverse MDCT transformation (integer)
 * dt, sr          Duration and samplerate
 * x, e            Input `ns` coefficients, magnitude below 2^30, and exponent
 * d               Delayed samples (nd words, Q12), updated
 * buf0, buf1      Scratch buffers of `ns` words
 * return          `ns` output samples in Q12, stored in `buf0` or `buf1`
 */
int32_t *lc3_mdct_inverse(enum lc3_dt dt, enum lc3_srate sr,
    const int32_t *x, int e, int32_t *d, int32_t *buf0, int32_t *buf1);


#endif /* __LC3_MDCT_H */
