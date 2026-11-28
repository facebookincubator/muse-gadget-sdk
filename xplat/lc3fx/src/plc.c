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

#include "plc.h"
#include "tables.h"


/**
 * Reset Packet Loss Concealment state
 */
void lc3_plc_reset(struct lc3_plc_state *plc)
{
    plc->seed = 24607;
    lc3_plc_suspend(plc);
}

/**
 * Suspend PLC execution (Good frame received)
 */
void lc3_plc_suspend(struct lc3_plc_state *plc)
{
    plc->count = 1;
    plc->alpha = 1 << 30;
}

/**
 * Synthesis of a PLC frame
 */
void lc3_plc_synthesize(enum lc3_dt dt, enum lc3_srate sr,
    struct lc3_plc_state *plc, const int32_t *x, int32_t *y)
{
    uint16_t seed = plc->seed;
    int32_t alpha = plc->alpha;
    int ne = lc3_ne(dt, sr);

    /* Attenuation 0.9 and 0.85 in Q31, alpha in Q30 */

    if (plc->count >= 4)
        alpha = lc3fx_mul31(alpha, plc->count < 8 ? 1932735283 : 1825361101);

    /* Inputs magnitude below 2^28: y = mulh(4 x, alpha) = x * alpha */

    for (int i = 0; i < ne; i++) {
        seed = (16831 + seed * 12821) & 0xffff;
        int32_t v = lc3fx_mulh(LC3FX_SHL(x[i], 2), alpha);
        y[i] = seed & 0x8000 ? -v : v;
    }

    plc->seed = seed;
    plc->alpha = alpha;
    plc->count++;
}
