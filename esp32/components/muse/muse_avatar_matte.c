/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "muse_avatar_matte.h"
#include <string.h>

enum { MATTE_EDGE=466, MATTE_PIXELS=466*466 };
static void rgb(uint16_t p,unsigned *r,unsigned *g,unsigned *b) {
    unsigned rr=p>>11,gg=(p>>5)&63u,bb=p&31u;
    *r=(rr<<3)|(rr>>2);*g=(gg<<2)|(gg>>4);*b=(bb<<3)|(bb>>2);
}
static unsigned limits(uint16_t p,unsigned *chroma) {
    unsigned r,g,b;rgb(p,&r,&g,&b);
    unsigned lo=r<g?r:g;if(b<lo)lo=b;
    unsigned hi=r>g?r:g;if(b>hi)hi=b;
    *chroma=hi-lo;return lo;
}
static bool white(uint16_t p) {unsigned c;return limits(p,&c)>=240 && c<=16;}
static bool dimensions(int w,int h) {return w>=1 && h>=1 && w<=MATTE_EDGE && h<=MATTE_EDGE;}
bool muse_avatar_white_candidate(const uint16_t *p,int w,int h) {
    if(!p || !dimensions(w,h))return false;
    size_t n=(size_t)w*h;
    if(!white(p[0]) || !white(p[w-1]) || !white(p[n-w]) || !white(p[n-1]))return false;
    unsigned total=0,matches=0;
    for(int x=0;x<w;x++) {
        total++;matches+=white(p[x]);
        if(h>1) {total++;matches+=white(p[n-w+x]);}
    }
    for(int y=1;y<h-1;y++) {
        total++;matches+=white(p[(size_t)y*w]);
        if(w>1) {total++;matches+=white(p[(size_t)y*w+w-1]);}
    }
    return matches*100u>=total*98u;
}
static void enqueue(const uint16_t *p,uint8_t *a,uint32_t *queue,size_t *tail,size_t i) {
    if(a[i]==255 && white(p[i])) {a[i]=0;queue[(*tail)++]=(uint32_t)i;}
}
static unsigned neighbors(size_t i,int w,int h,size_t out[4]) {
    unsigned n=0;size_t x=i%(size_t)w,y=i/(size_t)w;
    if(x)out[n++]=i-1;
    if(x+1<(size_t)w)out[n++]=i+1;
    if(y)out[n++]=i-w;
    if(y+1<(size_t)h)out[n++]=i+w;
    return n;
}
bool muse_avatar_white_mask(const uint16_t *p,int w,int h,uint8_t *a,
                            uint32_t *queue,size_t capacity,muse_avatar_matte_stats_t *stats) {
    if(stats)*stats=(muse_avatar_matte_stats_t){0};
    if(!p || !a || !queue || !dimensions(w,h))return false;
    size_t count=(size_t)w*h;
    if(capacity<count)return false;
    memset(a,255,count);
    if(!muse_avatar_white_candidate(p,w,h))return true;
    size_t head=0,tail=0;
    for(int x=0;x<w;x++) {
        enqueue(p,a,queue,&tail,(size_t)x);
        if(h>1)enqueue(p,a,queue,&tail,count-w+x);
    }
    for(int y=1;y<h-1;y++) {
        enqueue(p,a,queue,&tail,(size_t)y*w);
        if(w>1)enqueue(p,a,queue,&tail,(size_t)y*w+w-1);
    }
    while(head<tail) {
        size_t adjacent[4],i=queue[head++];unsigned n=neighbors(i,w,h,adjacent);
        for(unsigned j=0;j<n;j++)enqueue(p,a,queue,&tail,adjacent[j]);
    }
    /* A blank white image is not a safely identified foreground. */
    if(tail==count) {memset(a,255,count);return true;}
    unsigned softened=0;
    for(size_t i=0;i<count;i++) {
        if(a[i]!=255)continue;
        unsigned c,lo=limits(p[i],&c);
        if(lo<224 || c>32)continue;
        size_t adjacent[4];unsigned n=neighbors(i,w,h,adjacent),strength=0;bool outside=false;
        for(unsigned j=0;j<n;j++) {
            size_t k=adjacent[j];
            if(!a[k])outside=true;
            else {
                unsigned chroma,fg=limits(p[k],&chroma);
                if(fg<220 && 255-fg>strength)strength=255-fg;
            }
        }
        /* Strong source neighbors never meet the soft criterion themselves;
         * all estimates use immutable RGB, not partially decontaminated output. */
        if(!outside || !strength)continue;
        unsigned alpha=((255-lo)*255+strength/2)/strength;
        if(alpha<1)alpha=1;
        if(alpha>255)alpha=255;
        a[i]=(uint8_t)alpha;softened+=alpha<255;
    }
    if(stats)*stats=(muse_avatar_matte_stats_t){true,(unsigned)tail,softened};
    return true;
}
static unsigned unwhite(unsigned value,unsigned alpha) {
    unsigned white_part=255-alpha;
    unsigned premult=value>white_part?value-white_part:0;
    unsigned result=(premult*255+alpha/2)/alpha;
    return result>255?255:result;
}
void muse_avatar_matte_decontaminate(uint16_t *p,const uint8_t *a,size_t count) {
    if(!p || !a || count>MATTE_PIXELS)return;
    for(size_t i=0;i<count;i++) {
        if(!a[i]) {p[i]=0;continue;}
        if(a[i]==255)continue;
        unsigned r,g,b;rgb(p[i],&r,&g,&b);
        r=unwhite(r,a[i]);g=unwhite(g,a[i]);b=unwhite(b,a[i]);
        p[i]=(uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
    }
}
