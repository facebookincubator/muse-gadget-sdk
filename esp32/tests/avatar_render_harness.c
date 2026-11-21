/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "muse_avatar_render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint16_t src[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0x1234, 0xabcd};
    muse_avatar_pixels_t image = {src, 3, 2};
    for (int size = 1; size <= 512; size += 17) {
        uint16_t *full = malloc((size_t)size * size * 2);
        uint16_t *tiled = calloc((size_t)size * size, 2);
        assert(full && tiled);
        for (int zoom = 950; zoom <= 1000; zoom += 25) {
            assert(muse_avatar_blit(&image, size, zoom, full, size, 0, size - 1, 0, size - 1));
            for (int y = 0; y < size; y += 7) {
                int end = y + 6 < size ? y + 6 : size - 1;
                for (int x = 0; x < size; x += 11) {
                    int right = x + 10 < size ? x + 10 : size - 1;
                    assert(muse_avatar_blit(&image, size, zoom, tiled + y * size + x,
                                            size, x, right, y, end));
                }
            }
            assert(!memcmp(full, tiled, (size_t)size * size * 2));
        }
        for(int filter=MUSE_AVATAR_BILINEAR;filter<=MUSE_AVATAR_FAST_SMOOTH;filter++)
        for(int zoom=900;zoom<=1000;zoom+=50) for(int offset=-32;offset<=32;offset+=32) {
            muse_avatar_transform_t transform={zoom,1000,offset,(muse_avatar_filter_t)filter};
            assert(muse_avatar_blit_transform(&image,size,&transform,0xffff,full,size,0,size-1,0,size-1));
            for(int y=0;y<size;y+=7) for(int x=0;x<size;x+=11) {
                int bottom=y+6<size?y+6:size-1,right=x+10<size?x+10:size-1;
                assert(muse_avatar_blit_transform(&image,size,&transform,0xffff,tiled+y*size+x,size,x,right,y,bottom));
            }
            assert(!memcmp(full,tiled,(size_t)size*size*2));
        }
        free(full); free(tiled);
    }
    uint16_t out[16] = {0};
    assert(muse_avatar_blit(&image, 4, 1000, out, 4, 0, 3, 0, 3));
    assert(out[0] == 0 && out[4] == 0xf800 && out[7] == 0x001f);
    assert(out[8] == 0xffff && out[11] == 0xabcd && out[15] == 0);
    assert(!muse_avatar_blit(NULL, 4, 1000, out, 4, 0, 3, 0, 3));
    assert(!muse_avatar_blit(&image, 4, 1000, out, 3, 0, 3, 0, 3));
    assert(!muse_avatar_blit(&image, 4, 1000, out, 4, -1, 3, 0, 3));
    assert(!muse_avatar_blit(&image, 4, 1000, out, 4, 0, 4, 0, 3));
    assert(!muse_avatar_blit(&image, 4, 1001, out, 4, 0, 3, 0, 3));
    uint16_t two[]={0xf800,0x001f};
    muse_avatar_pixels_t line={two,2,1};
    muse_avatar_transform_t transform={1000,1000,0,MUSE_AVATAR_BILINEAR};
    assert(muse_avatar_blit_transform(&line,4,&transform,0xffff,out,4,0,3,0,3));
    assert(out[4]==0xf800 && out[7]==0x001f && out[5]!=two[0] && out[5]!=two[1]);
    assert((out[5]&0xf800) && (out[5]&0x001f)); /* interpolated, rather than repeated square pixels */
    assert(two[0]==0xf800 && two[1]==0x001f); /* immutable input */
    transform.zoom_y=899;assert(!muse_avatar_blit_transform(&line,4,&transform,0,out,4,0,3,0,3));
    transform.zoom_y=1000;transform.offset_y=33;
    assert(!muse_avatar_blit_transform(&line,4,&transform,0,out,4,0,3,0,3));
    puts("avatar renderer: contain fit, colors, strip equivalence, bounds passed");
}
