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
"""x264_mvcheap.py X264_DIR: adds an `mv-cheap` option to a copy of x264.

Decoder-friendly motion vectors. tinyh264 spends much of a P frame
interpolating reference pixels, and the positions with BOTH components
fractional (6-tap filters in two directions, then averaging) cost the most.
`mv-cheap=2` keeps the motion search off them: on the ESP-Mosaico at 480 px,
600 kb/s it cuts decode time by a fifth (46 -> 37 ms a frame on the board,
-25% instructions in the simulator) for about 0.2 dB. `mv-cheap=1` only
avoids the diagonal quarter positions; 0 (the default) changes nothing.

This repository carries no x264 code: the script inserts its own lines next
to anchors in the checkout you point it at (edit a copy, as jni/build.sh
does). It is idempotent, and fails without writing if an anchor is missing
(another x264 version): build without it then; the app's mv-cheap request is
then refused by x264 and ignored.
"""
import os
import sys

MARK = 'x264_mv_cheap_bad'

HELPER = '''

/* mv-cheap (added by Muse Gadgets Camera's jni/x264_mvcheap.py): whether a quarter-pel
 * MV is one the decoder interpolates slowly. */
static inline int x264_mv_cheap_bad( int mode, int mx, int my )
{
    if( mode == 1 )
        return (mx & 1) && (my & 1);
    if( mode == 2 )
        return (mx & 3) && (my & 3);
    if( mode >= 3 )   /* half-pel, one component at a time */
        return (mx & 1) || (my & 1) || ((mx & 3) && (my & 3));
    return 0;
}'''

FIX = '''/* mv-cheap: a start point (the predictor's subpel part) may still be one the
 * decoder interpolates slowly: move to the best allowed neighbour. */
#define MV_CHEAP_FIX() \\
if( x264_mv_cheap_bad( h->param.analyse.i_mv_cheap, bmx, bmy ) ) \\
{ \\
    int cand[4][2] = { { bmx & ~3, bmy }, { (bmx | 3) + 1, bmy }, { bmx, bmy & ~3 }, { bmx, (bmy | 3) + 1 } }; \\
    if( h->param.analyse.i_mv_cheap == 1 ) \\
    { \\
        cand[0][0] = bmx - 1; cand[1][0] = bmx + 1; \\
        cand[2][1] = bmy - 1; cand[3][1] = bmy + 1; \\
    } \\
    int best_ = COST_MAX, bx_ = bmx & ~3, by_ = bmy & ~3; \\
    for( int k_ = 0; k_ < 4; k_++ ) \\
    { \\
        int mx_ = x264_clip3( cand[k_][0], h->mb.mv_min_spel[0], h->mb.mv_max_spel[0] ); \\
        int my_ = x264_clip3( cand[k_][1], h->mb.mv_min_spel[1], h->mb.mv_max_spel[1] ); \\
        if( x264_mv_cheap_bad( h->param.analyse.i_mv_cheap, mx_, my_ ) ) \\
            continue; \\
        intptr_t stride_ = 16; \\
        pixel *src_ = h->mc.get_ref( pix, &stride_, &m->p_fref[0], m->i_stride[0], mx_, my_, bw, bh, &m->weight[0] ); \\
        int cost_ = h->pixf.mbcmp_unaligned[i_pixel]( m->p_fenc[0], FENC_STRIDE, src_, stride_ ) \\
                  + p_cost_mvx[mx_] + p_cost_mvy[my_]; \\
        if( cost_ < best_ ) \\
        { \\
            best_ = cost_; bx_ = mx_; by_ = my_; \\
        } \\
    } \\
    if( best_ != COST_MAX ) \\
        bcost = best_; \\
    bmx = bx_; bmy = by_; \\
}

'''

HPEL = '''
            if( h->param.analyse.i_mv_cheap )
            {
                const int mode_ = h->param.analyse.i_mv_cheap;
                if( x264_mv_cheap_bad( mode_, omx, omy-2 ) ) costs[0] = 1<<24;
                if( x264_mv_cheap_bad( mode_, omx, omy+2 ) ) costs[1] = 1<<24;
                if( x264_mv_cheap_bad( mode_, omx-2, omy ) ) costs[2] = 1<<24;
                if( x264_mv_cheap_bad( mode_, omx+2, omy ) ) costs[3] = 1<<24;
            }'''

PSKIP = '!x264_mv_cheap_bad( h->param.analyse.i_mv_cheap, h->mb.cache.pskip_mv[0], h->mb.cache.pskip_mv[1] )'

# (file, anchor, text, how): insert text after or before the anchor, replace
# the anchor with it, or (hpel) split the anchor at its first line.
EDITS = [
    ('x264.h', 'int          i_subpel_refine; /* subpixel motion estimation quality */',
     '\n        int          i_mv_cheap;      /* decoder-friendly MVs (x264_mvcheap.py) */', 'after'),
    ('common/base.c', '    OPT2("subme", "subq")',
     '    OPT("mv-cheap")\n        p->analyse.i_mv_cheap = atoi(value);\n', 'before'),
    ('x264.c', '''    { "subme",                required_argument, NULL, 'm' },''',
     '\n    { "mv-cheap",             required_argument, NULL, 0 },', 'after'),
    ('common/common.h', '#include "base.h"', HELPER, 'after'),
    ('encoder/me.c', 'if( b_refine_qpel || (dir^1) != odir ) \\',
     'if( (b_refine_qpel || (dir^1) != odir) && !x264_mv_cheap_bad( h->param.analyse.i_mv_cheap, mx, my ) ) \\',
     'replace'),
    ('encoder/me.c', '            costs[3] += p_cost_mvx[omx+2] + p_cost_mvy[omy  ];\n            COPY1_IF_LT( bcost, (costs[0]<<6)+2 );',
     HPEL, 'hpel'),
    ('encoder/me.c', 'static void refine_subpel( x264_t *h, x264_me_t *m, int hpel_iters, int qpel_iters, int *p_halfpel_thresh, int b_refine_qpel )\n{',
     FIX, 'before'),
    ('encoder/me.c', '    /* early termination when examining multiple reference frames */\n    if( p_halfpel_thresh )',
     '    MV_CHEAP_FIX();\n\n', 'before'),
    ('encoder/me.c', '    m->cost = bcost;\n    m->mv[0] = bmx;\n    m->mv[1] = bmy;\n    m->cost_mv = p_cost_mvx[bmx] + p_cost_mvy[bmy];\n}',
     '    MV_CHEAP_FIX();\n\n', 'before'),
    ('encoder/analyse.c', '              + abs(m.mv[1]-h->mb.cache.pskip_mv[1]) <= 1\n            && x264_macroblock_probe_pskip( h ) )',
     '              + abs(m.mv[1]-h->mb.cache.pskip_mv[1]) <= 1\n            && ' + PSKIP + '\n            && x264_macroblock_probe_pskip( h ) )',
     'replace'),
    ('encoder/analyse.c', 'int skip_invalid = h->i_thread_frames > 1 && h->mb.cache.pskip_mv[1] > h->mb.mv_max_spel[1];',
     'int skip_invalid = (h->i_thread_frames > 1 && h->mb.cache.pskip_mv[1] > h->mb.mv_max_spel[1])\n                || ' + PSKIP[1:] + ';',
     'replace'),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    files = {}
    for f, *_ in EDITS:
        if f not in files:
            with open(os.path.join(root, f)) as fh:
                files[f] = fh.read()
    if MARK in files['common/common.h']:
        print('x264_mvcheap: already applied')
        return
    for f, anchor, text, how in EDITS:
        s = files[f]
        if s.count(anchor) != 1:
            sys.exit('x264_mvcheap: anchor not found once in %s (another x264 version?): %r' % (f, anchor[:60]))
        if how == 'after':
            s = s.replace(anchor, anchor + text)
        elif how == 'before':
            s = s.replace(anchor, text + anchor)
        elif how == 'replace':
            s = s.replace(anchor, text)
        else:
            first, rest = anchor.split('\n', 1)
            s = s.replace(anchor, first + text + '\n' + rest)
        files[f] = s
    for f, s in files.items():
        with open(os.path.join(root, f), 'w') as fh:
            fh.write(s)
    print('x264_mvcheap: applied')


if __name__ == '__main__':
    main()
