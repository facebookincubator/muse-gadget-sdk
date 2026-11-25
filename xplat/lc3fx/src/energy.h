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

#ifndef __LC3_ENERGY_H
#define __LC3_ENERGY_H

#include "common.h"


/**
 * Energy estimation per band (integer)
 * dt, sr          Duration and samplerate of the frame
 * x, x_exp        Input MDCT coefficients (magnitude below 2^30), and exponent
 * e, e_exp        Return the energy estimation per band, and their exponent:
 *                 energy = e[] * 2^e_exp
 * scratch         Scratch buffer of 128 words, 8 bytes aligned
 * return          True when high energy detected near Nyquist frequency
 */
bool lc3_energy_compute(enum lc3_dt dt, enum lc3_srate sr,
        const int32_t *x, int x_exp, uint32_t *e, int *e_exp, int32_t *scratch);


#endif /* __LC3_ENERGY_H */
