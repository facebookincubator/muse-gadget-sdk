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
#include "avatar_webp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *root, const char *name, size_t *len) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s.webp", root, name);
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n > 0);
    rewind(f); uint8_t *data = malloc(n); assert(data);
    assert(fread(data, 1, n, f) == (size_t)n); fclose(f); *len = n; return data;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    size_t len;
    uint8_t *bytes = load(argv[1], "red", &len);
    avatar_webp_result_t r;
    assert(avatar_webp_decode(bytes, len, 466, 1536 * 1024, &r));
    assert(r.width == 16 && r.height == 8 && r.pixels[0] == 0xf800);
    uint16_t *old = r.pixels;
    size_t live = avatar_webp_live_bytes();
    avatar_webp_result_t bad;
    assert(!avatar_webp_decode(bytes, len, 466, 16, &bad));
    assert(!strcmp(bad.error, "no_memory"));
    assert(old[0] == 0xf800 && avatar_webp_live_bytes() == live);
    for (size_t n = 0; n < len; n++) {
        assert(!avatar_webp_decode(bytes, n, 466, 1536 * 1024, &bad));
        assert(avatar_webp_live_bytes() == live);
    }
    muse_webp_free(old); free(bytes); assert(!avatar_webp_live_bytes());
    bytes = load(argv[1], "alpha", &len);
    assert(avatar_webp_decode(bytes, len, 466, 1536 * 1024, &r));
    assert(r.pixels[0] == 0x0010);
    muse_webp_free(r.pixels); free(bytes); assert(!avatar_webp_live_bytes());
    bytes = load(argv[1], "gradient1600", &len);
    assert(avatar_webp_decode(bytes, len, 466, 1536 * 1024, &r));
    assert(r.width == 466 && r.height == 466);
    printf("1600px scaled decode: input=%zu peak=%zu retained=%zu\n",len,r.peak_bytes,avatar_webp_live_bytes());
    muse_webp_free(r.pixels); free(bytes); assert(!avatar_webp_live_bytes());
    bytes = load(argv[1], "animated", &len);
    assert(!avatar_webp_decode(bytes, len, 466, 1536 * 1024, &r));
    free(bytes); assert(!avatar_webp_live_bytes());
    puts("WebP: RGB565, alpha, scaled decode, memory ceiling, malformed input, animation rejection passed");
}
