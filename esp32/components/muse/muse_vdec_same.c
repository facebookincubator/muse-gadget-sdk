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

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
/* Not on boards without PSRAM: their tinyh264 is the low-memory copy (no per-picture macroblock map). */
#include "muse_vdec_same.h"

#include "h264bsd_macroblock_layer.h"

/* A skip copied from prev unchanged: no residual, motion vector 0, prev's buffer. */
static inline int skip0(const mbStorage_t *m, const uint8_t *prev)
{
    return m->mbType == P_Skip && m->mv[0].hor == 0 && m->mv[0].ver == 0 && m->refAddr[0] == prev;
}

/*
 * The deblocking filter can still change a skip's pixels along an edge
 * filtered with a nonzero strength. An edge between two such skips has
 * strength 0 (no coefficients, the same reference, the same motion), so it's
 * safe; any other edge only if its filter is off (disable_deblocking_filter_idc
 * 1). A macroblock's left and top edges are filtered as part of it (its idc),
 * its right and bottom ones as part of those neighbours (theirs).
 */
static inline int edge_ok(const mbStorage_t *owner, const mbStorage_t *neighbour, const uint8_t *prev)
{
    return !neighbour || owner->disableDeblockingFilterIdc == 1 || skip0(neighbour, prev);
}

int muse_h264_same_map(const storage_t *s, const uint8_t *prev, uint8_t *map, size_t cap)
{
    if (!s || !s->activeSps || !s->mb || !prev || s->numConcealedMbs) {
        return -1;
    }
    const u32 w = s->activeSps->picWidthInMbs, h = s->activeSps->picHeightInMbs;
    if ((size_t)w * h > cap) {
        return -1;
    }
    int n = 0;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            const mbStorage_t *m = &s->mb[y * w + x];
            const mbStorage_t *left = x ? m - 1 : NULL, *top = y ? m - w : NULL;
            const mbStorage_t *right = x + 1 < w ? m + 1 : NULL, *bottom = y + 1 < h ? m + w : NULL;
            /* Left and top edges: m's filter; right and bottom: the neighbour's. */
            int same = skip0(m, prev) && edge_ok(m, left, prev) && edge_ok(m, top, prev) &&
                       (!right || edge_ok(right, right, prev)) && (!bottom || edge_ok(bottom, bottom, prev));
            map[y * w + x] = (uint8_t)same;
            n += same;
        }
    }
    return n;
}
