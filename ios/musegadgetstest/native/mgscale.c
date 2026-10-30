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
#include "mgcodecs.h"

/* The same integer arithmetic as Android's PanelScaler (bit-identical). */
void mg_panel_scale(const uint8_t *src, int rowStride, int pixStride, int x0, int y0, int nIn, int nOut,
                    const int32_t *start, const int32_t *w, int len, int32_t *t, uint8_t *dst)
{
    int32_t acc[1024];
    if (nOut > 1024) {
        return;
    }
    for (int y = 0; y < nIn; y++) {
        const uint8_t *row = src + (y0 + y) * rowStride + x0 * pixStride;
        int32_t *to = t + y * nOut;
        for (int o = 0; o < nOut; o++) {
            const uint8_t *s = row + start[o] * pixStride;
            const int32_t *wo = w + o * len;
            int v = 0;
            for (int k = 0; k < len; k++) {
                v += s[k * pixStride] * wo[k];
            }
            to[o] = (v + 32) >> 6;
        }
    }
    for (int o = 0; o < nOut; o++) {
        const int32_t *r0 = t + start[o] * nOut;
        const int32_t *wo = w + o * len;
        for (int x = 0; x < nOut; x++) {
            acc[x] = r0[x] * wo[0];
        }
        for (int k = 1; k < len; k++) {
            const int32_t *r = r0 + k * nOut;
            int c = wo[k];
            for (int x = 0; x < nOut; x++) {
                acc[x] += r[x] * c;
            }
        }
        uint8_t *d = dst + o * nOut;
        for (int x = 0; x < nOut; x++) {
            int v = (acc[x] + (1 << 17)) >> 18;
            d[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}
