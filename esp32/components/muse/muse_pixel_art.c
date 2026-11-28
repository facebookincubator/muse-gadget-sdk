/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "muse_pixel_art.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
uint32_t muse_pixel_crc_append(uint32_t previous,const void *data,size_t size) {
    const uint8_t *bytes = data;
    uint32_t crc = ~previous;
    while (size--) {
        crc ^= *bytes++;
        for (unsigned i=0;i<8;i++) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
uint32_t muse_pixel_pack_crc(const void *data,size_t size) {return muse_pixel_crc_append(0,data,size);}
bool muse_pixel_pack_parse(const uint8_t *data, size_t size, muse_pixel_pack_t *out) {
    if (!data || !out || size<64 || size>MUSE_PIXEL_PACK_MAX ||
        memcmp(data,"MSPIX01",8) || u32(data+8)!=size || u16(data+18)!=0 ||
        u32(data+20)!=MUSE_PIXEL_CAPABILITIES || u32(data+60)!=muse_pixel_pack_crc(data,60) ||
        u32(data+56)!=muse_pixel_pack_crc(data+64,size-64)) return false;
    unsigned width=u16(data+12), height=u16(data+14), colors=u16(data+16);
    if (width<16 || width>MUSE_PIXEL_EDGE_MAX || height<16 || height>MUSE_PIXEL_EDGE_MAX ||
        !colors || colors>MUSE_PIXEL_PALETTE_MAX) return false;
    size_t pixels=(size_t)width*height;
    if (size!=64+2*colors+2*pixels) return false;
    const uint8_t *palette=data+64, *indices=palette+2*colors, *alpha=indices+pixels;
    bool visible=false;
    for (size_t i=0;i<pixels;i++) {
        if (indices[i]>=colors) return false;
        visible |= alpha[i]!=0;
    }
    if (!visible) return false;
    *out=(muse_pixel_pack_t){data,data+24,palette,indices,alpha,size,(uint16_t)width,(uint16_t)height,(uint16_t)colors};
    return true;
}

static unsigned triangle(uint32_t time, uint32_t period) {
    unsigned phase=(unsigned)(time%period)*2000u/period;
    return phase<=1000u?phase:2000u-phase;
}
static uint16_t rgb565(unsigned r,unsigned g,unsigned b) {
    return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}
static uint16_t blend(uint16_t a,uint16_t b,unsigned amount) {
    if (!amount) return a;
    if (amount>=255) return b;
    unsigned ar=(a>>11)&31u, ag=(a>>5)&63u, ab=a&31u;
    unsigned br=(b>>11)&31u, bg=(b>>5)&63u, bb=b&31u;
    unsigned inv=255u-amount;
    return (uint16_t)((((ar*inv+br*amount+127u)/255u)<<11)|
                      (((ag*inv+bg*amount+127u)/255u)<<5)|
                      ((ab*inv+bb*amount+127u)/255u));
}
bool muse_pixel_geometry(const muse_pixel_pack_t *pack,int canvas_px,
                         const muse_pixel_pose_t *pose,muse_pixel_geometry_t *out) {
    if (!pack || !pose || !out || !pack->width || !pack->height ||
        pack->width>MUSE_PIXEL_EDGE_MAX || pack->height>MUSE_PIXEL_EDGE_MAX ||
        canvas_px<16 || canvas_px>512 || pose->mode>6 || pose->happy>1000 || pose->level>1000) return false;
    /* Leave room for local motion instead of cropping any source anatomy. */
    int longest=pack->width>pack->height?pack->width:pack->height;
    unsigned breath=triangle(pose->time_ms,3200);
    unsigned zoom=pose->mode==1?940u+breath/100u:945u;
    if (pose->happy) zoom-=pose->happy/50u;
    int edge=canvas_px*(int)zoom/1000;
    int width=pack->width*edge/longest, height=pack->height*edge/longest;
    if (width<1) width=1;
    if (height<1) height=1;
    int bob=pose->mode==1?((int)triangle(pose->time_ms,2400)-500)*canvas_px/64000:0;
    int tap=-(int)(pose->happy*triangle(pose->time_ms,650)*4u/1000000u);
    int top=(canvas_px-height)/2+bob+tap;
    if (top<0) top=0;
    if (top>canvas_px-height) top=canvas_px-height;
    uint16_t tint=0;
    unsigned strength=0;
    switch (pose->mode) {
    case 0: tint=rgb565(220,230,255);strength=10;break;
    case 2: tint=rgb565(90,190,255);strength=8u+pose->level/100u;break;
    case 3: tint=pose->working?rgb565(255,180,70):rgb565(210,120,240);
            strength=8u+triangle(pose->time_ms,1500)/100u;break;
    case 4: tint=pose->reply_has_audio?rgb565(100,230,170):rgb565(170,210,235);
            strength=pose->reply_has_audio?8u+pose->level/100u:8u;break;
    case 5: tint=rgb565(255,100,100);strength=18;break;
    case 6: tint=0;strength=60;break;
    default: break;
    }
    *out=(muse_pixel_geometry_t){width,height,(canvas_px-width)/2,top,tint,(uint8_t)strength};
    return true;
}
bool muse_pixel_art_blit(const muse_pixel_pack_t *pack,int canvas_px,
                         const muse_pixel_pose_t *pose,uint16_t background,
                         uint16_t *dst,int stride_px,int x0,int x1,int y0,int y1) {
    muse_pixel_geometry_t g;
    if (!pack || !pack->palette || !pack->indices || !pack->alpha || !dst || x0<0 || y0<0 ||
        x1<x0 || y1<y0 || x1>=canvas_px || y1>=canvas_px || stride_px<x1-x0+1 ||
        !muse_pixel_geometry(pack,canvas_px,pose,&g)) return false;
    for (int y=y0;y<=y1;y++) {
        uint16_t *row=dst+(size_t)(y-y0)*stride_px;
        for (int x=x0;x<=x1;x++) {
            uint16_t color=background;
            if (x>=g.left && x<g.left+g.width && y>=g.top && y<g.top+g.height) {
                unsigned sx=(unsigned)(x-g.left)*pack->width/(unsigned)g.width;
                unsigned sy=(unsigned)(y-g.top)*pack->height/(unsigned)g.height;
                size_t at=(size_t)sy*pack->width+sx;
                unsigned index=pack->indices[at];
                if (index>=pack->colors) return false; /* caller must supply a parsed immutable pack */
                uint16_t source=u16(pack->palette+2*index);
                source=blend(source,g.tint,g.tint_amount);
                color=blend(background,source,pack->alpha[at]);
            }
            row[x-x0]=color;
        }
    }
    return true;
}
