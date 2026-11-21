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

#include "muse_theme_policy.h"
#include <math.h>
static float linear(unsigned x) {
    float c=(float)x/255.0f;
    return c<=0.04045f?c/12.92f:powf((c+0.055f)/1.055f,2.4f);
}
static float luminance(uint32_t c) {
    return 0.2126f*linear((c>>16)&255)+0.7152f*linear((c>>8)&255)+0.0722f*linear(c&255);
}
float muse_theme_contrast(uint32_t a,uint32_t b) {
    float x=luminance(a),y=luminance(b);
    return x>y?(x+0.05f)/(y+0.05f):(y+0.05f)/(x+0.05f);
}
uint16_t muse_theme_rgb565(uint32_t rgb) {
    return (uint16_t)(((rgb>>8)&0xf800)|((rgb>>5)&0x07e0)|((rgb>>3)&31));
}
muse_theme_palette_t muse_theme_palette(muse_theme_mode_t mode,bool valid,uint32_t hint) {
    if(mode!=MUSE_THEME_AUTO && mode!=MUSE_THEME_DARK && mode!=MUSE_THEME_LIGHT)mode=MUSE_THEME_AUTO;
    uint32_t bg=mode==MUSE_THEME_LIGHT?0xffffff:mode==MUSE_THEME_AUTO && valid?(hint&0xffffff):0;
    bool light=mode==MUSE_THEME_LIGHT || (mode==MUSE_THEME_AUTO && valid && luminance(bg)>=0.179f);
    return (muse_theme_palette_t){light,bg,!light && bg==0};
}
static uint32_t readable(uint32_t color,uint32_t bg) {
    if(muse_theme_contrast(color,bg)>=4.5f)return color;
    uint32_t ink=muse_theme_contrast(0,bg)>muse_theme_contrast(0xffffff,bg)?0:0xffffff;
    for(unsigned n=1;n<=16;n++) {
        uint32_t out=0;
        for(unsigned s=0;s<24;s+=8)out|=((((color>>s)&255)*(16-n)+((ink>>s)&255)*n)/16)<<s;
        if(muse_theme_contrast(out,bg)>=4.5f)return out;
    }
    return ink;
}
uint32_t muse_theme_map(const muse_theme_palette_t *p,uint32_t legacy,bool foreground) {
    legacy&=0xffffff;
    if(p->original_dark)return legacy;
    if(legacy==0)return foreground?(p->light?0xffffff:0):p->background;
    /* Background tracks remain distinct without competing with labels. */
    static const struct {uint32_t dark,light;bool surface;} colors[]={
        {0xf2efff,0x251e31,false},{0xf0edf8,0x251e31,false},{0xffffff,0x251e31,false},
        {0x8b84a8,0x62576e,false},{0xd8d2ff,0x3b2b55,false},
        {0xa77dff,0x6331a6,false},{0x6ff0bf,0x08734c,false},
        {0xffb45c,0x8b4a04,false},{0xff5c5c,0xb52d32,false},
        {0x1a1530,0xf0edf7,true},{0x2e2552,0xded7ed,true},
        {0x140f22,0xe6e0ee,true},{0x1d1733,0xe0d9eb,true},
        {0x3a3358,0xafa5c2,true},{0x201a35,0xeee9f5,true},
        {0x3a3052,0xcac0da,true},{0x2a2345,0xd9d1e5,true},
        {0x171321,0xf3f0f8,true},{0x211a32,0xe5deef,true},
        {0x80bfff,0x165b9c,false},{0x64d4ff,0x006d8d,false},
        {0xffd060,0x855400,false},{0x8b7845,0x73602c,false},
    };
    uint32_t result=legacy;bool surface=false,found=false;
    for(unsigned i=0;i<sizeof(colors)/sizeof(colors[0]);i++)if(colors[i].dark==legacy) {
        if(p->light)result=colors[i].light;
        surface=colors[i].surface;found=true;break;
    }
    if(p->light && !found && !foreground) {
        unsigned r=(legacy>>16)&255,g=(legacy>>8)&255,b=legacy&255;
        unsigned hi=r>g?r:g;hi=hi>b?hi:b;unsigned lo=r<g?r:g;lo=lo<b?lo:b;
        if(hi<128) result=(((255*7+r)/8)<<16)|(((255*7+g)/8)<<8)|((255*7+b)/8);
        else if(hi-lo>40) result=readable(result,p->background);
    }
    if(legacy==0xffffff && !foreground)return p->light?0:0xffffff;
    return foreground && !surface?readable(result,p->background):result;
}

uint32_t muse_theme_text_on(const muse_theme_palette_t *p,uint32_t legacy,uint32_t surface) {
    uint32_t color=muse_theme_map(p,legacy,true);
    if(p->original_dark)return color;
    return readable(color,muse_theme_map(p,surface,false));
}
