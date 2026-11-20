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
#include "avatar_cache.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(avatar_cache_header_t) == 704, "cache layout changed: bump version");
uint32_t avatar_cache_crc(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (len--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void avatar_cache_header_init(avatar_cache_header_t *h, uint32_t sequence,
                              int w, int height, const uint8_t binding[32],
                              const avatar_identity_t *identity, const uint16_t *pixels) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "MAVTR01", 8);
    h->sequence = sequence;
    h->width = w; h->height = height;
    h->bytes = (uint32_t)w * height * 2;
    h->pixels_crc = avatar_cache_crc(pixels, h->bytes);
    memcpy(h->binding, binding, 32); h->identity = *identity;
    h->header_crc = avatar_cache_crc(h, offsetof(avatar_cache_header_t, header_crc));
}
bool avatar_cache_header_valid(const avatar_cache_header_t *h, const uint8_t binding[32]) {
    return !memcmp(h->magic, "MAVTR01", 8) && !memcmp(h->binding, binding, 32) &&
           h->width > 0 && h->width <= 466 && h->height > 0 && h->height <= 466 &&
           h->bytes == h->width * h->height * 2 &&
           memchr(h->identity.path, 0, sizeof(h->identity.path)) && h->identity.path[0] &&
           memchr(h->identity.revision, 0, sizeof(h->identity.revision)) &&
           h->header_crc == avatar_cache_crc(h, offsetof(avatar_cache_header_t, header_crc));
}
bool avatar_cache_read(const char *path, const uint8_t binding[32],
                       avatar_cache_header_t *header, void *pixels, size_t capacity) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    avatar_cache_header_t h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && avatar_cache_header_valid(&h, binding);
    if (ok && pixels) {
        ok = h.bytes <= capacity && fread(pixels, 1, h.bytes, f) == h.bytes &&
             fgetc(f) == EOF && !ferror(f) && avatar_cache_crc(pixels, h.bytes) == h.pixels_crc;
    } else if (ok) {
        ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)(sizeof(h) + h.bytes);
    }
    fclose(f);
    if (ok) *header = h;
    return ok;
}
bool avatar_cache_write(const char *path, const avatar_cache_header_t *h, const void *pixels) {
    return avatar_cache_write_while(path, h, pixels, NULL, NULL);
}
bool avatar_cache_write_while(const char *path, const avatar_cache_header_t *h,
                              const void *pixels, bool (*proceed)(void *), void *ctx) {
    if (!avatar_cache_header_valid(h, h->binding) || avatar_cache_crc(pixels, h->bytes) != h->pixels_crc) return false;
    if (proceed && !proceed(ctx)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(h, 1, sizeof(*h), f) == sizeof(*h);
    for (size_t off = 0; ok && off < h->bytes;) {
        if (proceed && !proceed(ctx)) { ok = false; break; }
        size_t n = h->bytes - off; if (n > 4096) n = 4096;
        ok = fwrite((const uint8_t *)pixels + off, 1, n, f) == n; off += n;
    }
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f)) ok = false;
    if (!ok) remove(path);
    return ok;
}

bool avatar_cache_identity_write(const char *path, const char *temporary, const avatar_cache_header_t *h) {
    if (!avatar_cache_header_valid(h, h->binding)) return false;
    FILE *f = fopen(temporary, "wb");
    if (!f) return false;
    bool ok = fwrite(h, 1, sizeof(*h), f) == sizeof(*h) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f)) ok = false;
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) remove(temporary);
    return ok;
}
bool avatar_cache_identity_read(const char *path, avatar_cache_header_t *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    avatar_cache_header_t meta;
    bool ok = fread(&meta, 1, sizeof(meta), f) == sizeof(meta) && fgetc(f) == EOF && !ferror(f) &&
              avatar_cache_header_valid(&meta, h->binding) && meta.sequence == h->sequence &&
              meta.width == h->width && meta.height == h->height && meta.pixels_crc == h->pixels_crc;
    fclose(f);
    if (ok) *h = meta;
    return ok;
}
