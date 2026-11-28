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
#include "muse_avatar_pack.h"
#include <string.h>
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
uint32_t muse_avatar_pack_crc(const void *data, size_t size) {
    const uint8_t *p = data; uint32_t crc = ~0u;
    for (size_t i=0;i<size;i++) { crc ^= p[i]; for (int b=0;b<8;b++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); }
    return ~crc;
}
bool muse_avatar_pack_parse(const uint8_t *data, size_t size, const uint8_t source[32], muse_avatar_pack_t *out) {
    if (!data || !source || !out || size < 64 || size > MUSE_AVATAR_PACK_MAX ||
        memcmp(data,"MAVPK01",8) || u32(data+8)!=size || memcmp(data+28,source,32) ||
        u32(data+60)!=muse_avatar_pack_crc(data,60) || u32(data+24)!=muse_avatar_pack_crc(data+64,size-64)) return false;
    muse_avatar_pack_t p = {.data=data,.size=size,.width=u16(data+12),.height=u16(data+14),.fps=u16(data+16)};
    unsigned count=u16(data+18);
    if (!p.width || !p.height || p.width>240 || p.height>240 || p.width%16 || p.height%16 ||
        p.fps<1 || p.fps>24 || !count || count>MUSE_AVATAR_CLIPS) return false;
    size_t off=64;
    for (unsigned i=0;i<count;i++) {
        if (size-off<16) return false;
        unsigned kind=u16(data+off), flags=u16(data+off+2);
        uint32_t frames=u32(data+off+4), bytes=u32(data+off+8), crc=u32(data+off+12);
        off+=16;
        if (kind>=MUSE_AVATAR_CLIPS || flags>1 || !frames || frames>240 || bytes>size-off ||
            (p.coverage&(1u<<kind)) || crc!=muse_avatar_pack_crc(data+off,bytes)) return false;
        /* Idle is the worker's EOF fallback. A one-shot idle would repeatedly
         * select itself at EOF without yielding, starving the idle task. */
        if (kind==MUSE_CLIP_IDLE && flags!=1) return false;
        p.clip[kind]=(muse_avatar_clip_t){(uint32_t)off,bytes,frames,flags==1};
        p.coverage|=1u<<kind;
        size_t end=off+bytes;
        for (uint32_t f=0;f<frames;f++) {
            if (end-off<8) return false;
            uint32_t len=u32(data+off), checksum=u32(data+off+4); off+=8;
            if (!len || len>MUSE_AVATAR_FRAME_MAX || len>end-off ||
                checksum!=muse_avatar_pack_crc(data+off,len)) return false;
            /* AUD-prefixed access units; SPS dimensions are bounded again by the decoder. */
            if (len<6 || memcmp(data+off,"\0\0\0\1\11",5)) return false;
            off+=len;
        }
        if (off!=end) return false;
    }
    if (off!=size || p.coverage!=u32(data+20) || !(p.coverage&1)) return false;
    *out=p; return true;
}
bool muse_avatar_pack_frame(const muse_avatar_pack_t *p,unsigned kind,uint32_t *cursor,const uint8_t **data,uint32_t *size) {
    if (!p || !cursor || !data || !size || kind>=MUSE_AVATAR_CLIPS || !(p->coverage&(1u<<kind))) return false;
    const muse_avatar_clip_t *c=&p->clip[kind];
    uint32_t end=c->offset+c->bytes;
    if (!*cursor) *cursor=c->offset;
    if (*cursor<c->offset || *cursor>end || end-*cursor<8) return false;
    uint32_t len=u32(p->data+*cursor);
    if (!len || len>MUSE_AVATAR_FRAME_MAX || len>end-*cursor-8) return false;
    *data=p->data+*cursor+8; *size=len; *cursor+=8+len; return true;
}
