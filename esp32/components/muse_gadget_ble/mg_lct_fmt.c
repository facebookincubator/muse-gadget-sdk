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
 * The live call timeline's summary line (mg_lct.h), with no IDF in it.
 */
#include <stdio.h>

#include "mg_lct.h"

static const char *const LABEL[MG_LCT_COUNT] = {
    [MG_LCT_T0_PRESS] = "T0",
    [MG_LCT_T0R_SEND] = "T0r",
    [MG_LCT_T1C_CONNECTING] = "T1c",
    [MG_LCT_T7R_LIVE] = "T7r",
    [MG_LCT_TV_CAPS] = "Tv",
    [MG_LCT_TS_START] = "Ts",
    [MG_LCT_T8R_BYTES] = "T8r",
    [MG_LCT_T8K_KEYFRAME] = "T8k",
    [MG_LCT_T8D_DECODED] = "T8d",
    [MG_LCT_T9_SHOWN] = "T9",
    [MG_LCT_T6S_AUDIO_START] = "T6s",
    [MG_LCT_T6R_AUDIO_BYTES] = "T6r",
    [MG_LCT_T6P_AUDIO_PLAYED] = "T6p",
    [MG_LCT_TVOL] = "Tvol",
};

int mg_lct_format_summary(char *out, size_t cap, const int64_t at[MG_LCT_COUNT], int64_t base, const char *by,
                          const char *end)
{
    int n = snprintf(out, cap, "summary (ms since %s) by=%s end=%s:", at[MG_LCT_T0_PRESS] ? "T0" : "the phone's first step",
                     by && *by ? by : "-", end && *end ? end : "-");
    for (int i = 0; i < MG_LCT_COUNT && n >= 0 && (size_t)n < cap; i++) {
        if (at[i]) {
            n += snprintf(out + n, cap - (size_t)n, " %s=%lld", LABEL[i], (long long)((at[i] - base) / 1000));
        } else {
            n += snprintf(out + n, cap - (size_t)n, " %s=-", LABEL[i]);
        }
    }
    return n;
}
