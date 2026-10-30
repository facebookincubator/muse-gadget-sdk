#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""libvpx_mg.py LIBVPX_DIR: two switches in libvpx's VP8 encoder for gadget decoders.

- vp8_mg_lf_max: -1 (default) leaves the loop filter level to libvpx; N caps
  it; 0 turns the filter off and skips its search. A gadget then skips the
  loop filter entirely: about a third of its decode time with the filter on.
- vp8_mg_mvcheap: 1 keeps the sub-pixel search off positions with both
  components fractional, so the gadget interpolates in one pass, not two
  (x264_mvcheap.py's mv-cheap=2 for VP8). About 2-3% more bits at the same
  PSNR; the luma prediction costs half.

Defaults keep libvpx's output byte for byte. Run on a copy of a libvpx
checkout (v1.16.0 tested); jni/build_vpx.sh does.
"""
import sys, os

src = sys.argv[1]


def patch(rel, edits):
    p = os.path.join(src, rel)
    s = open(p).read()
    for old, new in edits:
        if old not in s:
            sys.exit(f"libvpx_mg.py: {rel} doesn't match (libvpx version?): {old[:60]!r}")
        s = s.replace(old, new, 1)
    open(p, "w").write(s)


patch("vp8/encoder/mcomp.c", [(
    """#define IFMVCV(r, c, s, e) \\
  if (c >= minc && c <= maxc && r >= minr && r <= maxr) s else e;""",
    """/* Muse gadgets (jni/libvpx_mg.py): 1 keeps the search off positions with
 * both components fractional (one interpolation pass in the decoder). */
int vp8_mg_mvcheap = 0;
#define IFMVCV(r, c, s, e)                                            \\
  if (c >= minc && c <= maxc && r >= minr && r <= maxr &&             \\
      !(vp8_mg_mvcheap && ((r) & 3) && ((c) & 3))) s else e;""")])

patch("vp8/encoder/onyx_if.c", [(
    """  if (cm->no_lpf) {
    cm->filter_level = 0;
  } else {""",
    """  if (cm->no_lpf || vp8_mg_lf_max == 0) {
    cm->filter_level = 0;
  } else {"""), (
    """    if (cm->filter_level > 0) {
      vp8cx_set_alt_lf_level(cpi, cm->filter_level);
    }""",
    """    if (vp8_mg_lf_max > 0 && cm->filter_level > vp8_mg_lf_max) {
      cm->filter_level = vp8_mg_lf_max;
    }
    if (cm->filter_level > 0) {
      vp8cx_set_alt_lf_level(cpi, cm->filter_level);
    }"""), (
    """void vp8_loopfilter_frame(VP8_COMP *cpi, VP8_COMMON *cm) {""",
    """/* Muse gadgets (jni/libvpx_mg.py): -1 libvpx's loop filter level, else at
 * most this (0: off). */
int vp8_mg_lf_max = -1;

void vp8_loopfilter_frame(VP8_COMP *cpi, VP8_COMMON *cm) {""")])
print("libvpx_mg.py: patched", src)
