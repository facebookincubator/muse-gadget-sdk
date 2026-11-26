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

#include "energy.h"
#include "tables.h"


/**
 * Energy estimation per band
 */
LC3_HOT bool lc3_energy_compute(enum lc3_dt dt, enum lc3_srate sr,
    const int32_t *x, int x_exp, uint32_t *e, int *e_exp, int32_t *scratch)
{
    int nb = lc3_num_bands[dt][sr];
    const int *lim = lc3_band_lim[dt][sr];

    /* --- Sum of squares (64 bits) of coefficients within each band --- */

        uint64_t *sx2 = (uint64_t *)scratch, sx2_max = 0;

    for (int iband = 0, i = lim[iband]; iband < nb; iband++) {
        int ie = lim[iband+1];
        uint64_t s = 0;

        for ( ; i < ie; i++) {
            int32_t v = x[i] >> 2;
            s += (uint64_t)((int64_t)v * v);
        }

        sx2[iband] = s;
        sx2_max = s > sx2_max ? s : sx2_max;
    }

    /* --- Mean of the squares, on a common 32 bits scale --- */

    int sh = 32 - lc3fx_clz64(sx2_max);
    sh = sh > 0 ? sh : 0;

    *e_exp = sh + 2 * (x_exp + 2);

    uint64_t e_sum[2] = { 0, 0 };
    static const int8_t nh_tab[LC3_NUM_DT] = {
        [LC3_DT_2M5] = 2, [LC3_DT_5M ] = 3,
        [LC3_DT_7M5] = 4, [LC3_DT_10M] = 2 };
    int iband_h = nb - nh_tab[dt];

    for (int iband = 0; iband < nb; iband++) {
        unsigned n = lim[iband+1] - lim[iband];
        uint32_t s = (uint32_t)(sx2[iband] >> sh);

        e[iband] = n > 1 ? s / n : s;
        e_sum[iband >= iband_h] += e[iband];
    }

    /* --- Return the near nyquist flag --- */

    return e_sum[1] > 30 * e_sum[0];
}
