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

/* Five fraction bits fit between the packed RGB565 lanes without carrying
 * blue into red. This avoids float math and per-pixel division on the S3. */
static uint16_t blend(uint16_t a,uint16_t b,unsigned fraction) {
    unsigned inverse=32-fraction;
    return (uint16_t)(((((a&0xf81fu)*inverse+(b&0xf81fu)*fraction)>>5)&0xf81fu) |
                      ((((a&0x07e0u)*inverse+(b&0x07e0u)*fraction)>>5)&0x07e0u));
}
static uint16_t average(uint16_t a,uint16_t b) {
    return (uint16_t)((a&b)+(((a^b)&0xf7deu)>>1));
}

static uint16_t sample(const uint16_t *row,int x,const uint16_t *base,const uint8_t *alpha,uint16_t bg) {
    uint16_t p=row[x];
    if(!alpha)return p;
    unsigned a=alpha[(size_t)(row-base)+(size_t)x];
    if(!a)return bg;
    if(a==255)return p;
    unsigned inverse=255-a;
    unsigned r=((p>>11)*a+(bg>>11)*inverse+127)/255;
    unsigned g=(((p>>5)&63u)*a+((bg>>5)&63u)*inverse+127)/255;
    unsigned b=((p&31u)*a+(bg&31u)*inverse+127)/255;
    return (uint16_t)((r<<11)|(g<<5)|b);
}

bool muse_avatar_blit_alpha_transform(const muse_avatar_pixels_t *image,const uint8_t *alpha,int canvas_px,
                               const muse_avatar_transform_t *t,uint16_t background,
                               uint16_t *dst,int stride_px,int x0,int x1,int y0,int y1) {
    if(!image || !image->pixels || !t || !dst || image->width<1 || image->height<1 ||
       image->width>MUSE_AVATAR_MAX_EDGE || image->height>MUSE_AVATAR_MAX_EDGE ||
       canvas_px<1 || canvas_px>512 || x0<0 || y0<0 || x1<x0 || y1<y0 ||
       x1>=canvas_px || y1>=canvas_px || stride_px<x1-x0+1 ||
       t->zoom_x<900 || t->zoom_x>1000 || t->zoom_y<900 || t->zoom_y>1000 ||
       t->offset_y < -32 || t->offset_y>32 || t->filter<MUSE_AVATAR_NEAREST ||
       t->filter>MUSE_AVATAR_FAST_SMOOTH)return false;
    const int source_w=image->width,source_h=image->height;
    const uint16_t *pixels=image->pixels;
    int longest=source_w>source_h?source_w:source_h;
    int w=source_w*(canvas_px*t->zoom_x/1000)/longest;
    int h=source_h*(canvas_px*t->zoom_y/1000)/longest;
    if(w<1)w=1;
    if(h<1)h=1;
    int left=(canvas_px-w)/2,top=(canvas_px-h)/2+t->offset_y;
    if(top<0)top=0;
    if(top>canvas_px-h)top=canvas_px-h;
    int start=x0>left?x0:left,stop=x1<left+w-1?x1:left+w-1;
    int step_x=(source_w<<16)/w,step_y=(source_h<<16)/h;
    int max_x=(source_w-1)<<16,max_y=(source_h-1)<<16;
    for(int y=y0;y<=y1;y++) {
        uint16_t *row=dst+(size_t)(y-y0)*stride_px;
        if(y<top || y>=top+h || start>stop) {
            for(int x=x0;x<=x1;x++)row[x-x0]=background;
            continue;
        }
        for(int x=x0;x<start;x++)row[x-x0]=background;
        for(int x=stop+1;x<=x1;x++)row[x-x0]=background;
        if(t->filter==MUSE_AVATAR_NEAREST) {
            int sy=(y-top)*source_h/h;
            const uint16_t *source=pixels+(size_t)sy*source_w;
            for(int x=start;x<=stop;x++)row[x-x0]=sample(source,(x-left)*source_w/w,pixels,alpha,background);
            continue;
        }
        int fy=(y-top)*step_y+step_y/2-32768;
        if(fy<0)fy=0;
        if(fy>max_y)fy=max_y;
        int sy=fy>>16,sy1=sy+1<source_h?sy+1:sy;
        const uint16_t *upper=pixels+(size_t)sy*source_w,*lower=pixels+(size_t)sy1*source_w;
        unsigned wy=((unsigned)fy>>11)&31u;
        int fx=(start-left)*step_x+step_x/2-32768;
        if(t->filter==MUSE_AVATAR_FAST_SMOOTH) {
            /* Three interpolation phases (0, 1/2, 1). Packed averages avoid
             * multiplications and unnecessary neighbor reads on moving frames. */
            bool mix_y=wy>=8 && wy<24;
            if(wy>=24)upper=lower;
            for(int x=start;x<=stop;x++,fx+=step_x) {
                int sx_fp=fx<0?0:fx>max_x?max_x:fx;
                int sx=sx_fp>>16,sx1=sx+1<source_w?sx+1:sx;
                unsigned wx=((unsigned)sx_fp>>11)&31u;
                bool mix_x=wx>=8 && wx<24;
                if(wx>=24)sx=sx1;
                uint16_t a=mix_x?average(sample(upper,sx,pixels,alpha,background),sample(upper,sx1,pixels,alpha,background)):
                    sample(upper,sx,pixels,alpha,background);
                if(mix_y) {
                    uint16_t b=mix_x?average(sample(lower,sx,pixels,alpha,background),sample(lower,sx1,pixels,alpha,background)):
                        sample(lower,sx,pixels,alpha,background);
                    a=average(a,b);
                }
                row[x-x0]=a;
            }
            continue;
        }
        for(int x=start;x<=stop;x++,fx+=step_x) {
            int sx_fp=fx<0?0:fx>max_x?max_x:fx;
            int sx=sx_fp>>16,sx1=sx+1<source_w?sx+1:sx;
            unsigned wx=((unsigned)sx_fp>>11)&31u;
            uint16_t a=blend(sample(upper,sx,pixels,alpha,background),sample(upper,sx1,pixels,alpha,background),wx);
            uint16_t b=blend(sample(lower,sx,pixels,alpha,background),sample(lower,sx1,pixels,alpha,background),wx);
            row[x-x0]=blend(a,b,wy);
        }
    }
    return true;
}

bool muse_avatar_blit_transform(const muse_avatar_pixels_t *image,int canvas_px,
                               const muse_avatar_transform_t *t,uint16_t background,
                               uint16_t *dst,int stride_px,int x0,int x1,int y0,int y1) {
    return muse_avatar_blit_alpha_transform(image,NULL,canvas_px,t,background,dst,stride_px,x0,x1,y0,y1);
}

bool muse_avatar_blit_background(const muse_avatar_pixels_t *image, int canvas_px, int zoom,
                      uint16_t background, uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1) {
    if (!image || !image->pixels || !dst || image->width < 1 || image->height < 1 ||
        image->width > MUSE_AVATAR_MAX_EDGE || image->height > MUSE_AVATAR_MAX_EDGE ||
        canvas_px < 1 || canvas_px > 512 || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 ||
        x1 >= canvas_px || y1 >= canvas_px || stride_px < x1 - x0 + 1 ||
        zoom < 950 || zoom > 1000) return false;
    int edge = canvas_px * zoom / 1000;
    if (edge < 1) edge = 1;
    int longest = image->width > image->height ? image->width : image->height;
    int w = image->width * edge / longest;
    int h = image->height * edge / longest;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int left = (canvas_px - w) / 2, top = (canvas_px - h) / 2;
    for (int y = y0; y <= y1; y++) {
        int sy = y >= top && y < top + h ? (y - top) * image->height / h : -1;
        uint16_t *row = dst + (size_t)(y - y0) * stride_px;
        for (int x = x0; x <= x1; x++) {
            row[x - x0] = sy >= 0 && x >= left && x < left + w ?
                image->pixels[(size_t)sy * image->width + (x - left) * image->width / w] : background;
        }
    }
    return true;
}

bool muse_avatar_blit(const muse_avatar_pixels_t *image,int canvas_px,int zoom,
                      uint16_t *dst,int stride_px,int x0,int x1,int y0,int y1) {
    return muse_avatar_blit_background(image,canvas_px,zoom,0,dst,stride_px,x0,x1,y0,y1);
}
