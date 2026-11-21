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
#include "muse_avatar_background.h"
#include <string.h>
static unsigned bin(uint16_t c) {return ((c>>13)<<6)|(((c>>8)&7)<<3)|((c>>2)&7);}
bool muse_avatar_background_estimate(const muse_avatar_pixels_t *image,uint32_t *rgb,unsigned *confidence) {
    if(confidence)*confidence=0;
    if(!image || !image->pixels || !rgb || image->width<1 || image->height<1 ||
       image->width>MUSE_AVATAR_MAX_EDGE || image->height>MUSE_AVATAR_MAX_EDGE)return false;
    uint8_t counts[512]={0};uint16_t pixels[64];unsigned n=0;
    for(unsigned corner=0;corner<4;corner++)for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++) {
        int dx=(int)x*image->width/64,dy=(int)y*image->height/64;
        int sx=(corner&1)?image->width-1-dx:dx;
        int sy=(corner&2)?image->height-1-dy:dy;
        uint16_t c=image->pixels[(size_t)sy*image->width+sx];pixels[n++]=c;
        counts[bin(c)]+=(corner&2)?1:2;
    }
    unsigned best=0;
    for(unsigned i=1;i<512;i++)if(counts[i]>counts[best])best=i;
    unsigned score=counts[best]*100u/96u;
    if(confidence)*confidence=score;
    if(score<60)return false;
    unsigned corners=0,total=0,r=0,g=0,b=0;
    for(unsigned c=0;c<4;c++) {
        unsigned matches=0,weight=(c&2)?1:2;
        for(unsigned i=c*16;i<(c+1)*16;i++)if(bin(pixels[i])==best) {
            uint16_t color=pixels[i];matches++;total+=weight;
            r+=((color>>11)&31)*weight;g+=((color>>5)&63)*weight;b+=(color&31)*weight;
        }
        if(matches>=4)corners++;
    }
    if(corners<3 || !total)return false;
    r=((r+total/2)/total)*255/31;g=((g+total/2)/total)*255/63;b=((b+total/2)/total)*255/31;
    *rgb=(r<<16)|(g<<8)|b;return true;
}
