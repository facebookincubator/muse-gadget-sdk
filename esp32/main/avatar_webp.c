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
#include "webp/decode.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

typedef union { max_align_t alignment; size_t bytes; } allocation_t;
static atomic_size_t s_live, s_peak;
static size_t s_limit;
size_t avatar_webp_live_bytes(void) { return atomic_load(&s_live); }
void avatar_webp_set_budget(size_t bytes) { s_limit = bytes; }

void *muse_webp_malloc(size_t bytes) {
    if (!bytes || bytes > SIZE_MAX - sizeof(allocation_t)) return NULL;
    size_t need = bytes + sizeof(allocation_t), used = atomic_load(&s_live);
    do {
        if (used > s_limit || need > s_limit - used) return NULL;
    } while (!atomic_compare_exchange_weak(&s_live, &used, used + need));
#ifdef ESP_PLATFORM
    size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (available <= AVATAR_WEBP_RESERVE_BYTES || need > available - AVATAR_WEBP_RESERVE_BYTES) {
        atomic_fetch_sub(&s_live, need); return NULL;
    }
    allocation_t *a = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Other tasks may consume PSRAM after admission. Give this allocation
     * back rather than taking the reserve; callers retain/retry old content. */
    if (a && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < AVATAR_WEBP_RESERVE_BYTES) {
        heap_caps_free(a); a = NULL;
    }
#else
    allocation_t *a = malloc(need);
#endif
    if (!a) { atomic_fetch_sub(&s_live, need); return NULL; }
    a->bytes = need;
    size_t peak = atomic_load(&s_peak);
    while (peak < used + need && !atomic_compare_exchange_weak(&s_peak, &peak, used + need)) {}
    return a + 1;
}
void *muse_webp_calloc(size_t n, size_t bytes) {
    if (!bytes || n > SIZE_MAX / bytes) return NULL;
    void *p = muse_webp_malloc(n * bytes);
    if (p) memset(p, 0, n * bytes);
    return p;
}
void muse_webp_free(void *p) {
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    atomic_fetch_sub(&s_live, a->bytes);
#ifdef ESP_PLATFORM
    heap_caps_free(a);
#else
    free(a);
#endif
}

bool avatar_webp_decode(const uint8_t *bytes, size_t len, int max_edge,
                        size_t budget, avatar_webp_result_t *out) {
    if (!out) return false;
    *out = (avatar_webp_result_t){.error = "invalid_image"};
    if (!bytes || len < 12 || len > 512 * 1024 || max_edge < 1 || max_edge > 466) return false;
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(bytes, len, &cfg.input) != VP8_STATUS_OK ||
        cfg.input.has_animation || cfg.input.width < 1 || cfg.input.height < 1 ||
        cfg.input.width > 2048 || cfg.input.height > 2048) return false;
    /* Alpha uses RGBA while compositing, so cap it to the same steady budget. */
    if (cfg.input.has_alpha && max_edge > 320) max_edge = 320;
    int longest = cfg.input.width > cfg.input.height ? cfg.input.width : cfg.input.height;
    int edge = longest < max_edge ? longest : max_edge;
    int w = cfg.input.width * edge / longest, h = cfg.input.height * edge / longest;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    size_t bpp = cfg.input.has_alpha ? 4 : 2;
    s_limit = budget;
    atomic_store(&s_peak, atomic_load(&s_live));
    uint8_t *pixels = muse_webp_malloc((size_t)w * h * bpp);
    if (!pixels) { out->error = "no_memory"; return false; }
    cfg.options.use_scaling = 1;
    cfg.options.scaled_width = w;
    cfg.options.scaled_height = h;
    cfg.options.no_fancy_upsampling = 1;
    cfg.options.use_threads = 0;
    cfg.output.colorspace = cfg.input.has_alpha ? MODE_RGBA : MODE_RGB_565;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = pixels;
    cfg.output.u.RGBA.stride = w * bpp;
    cfg.output.u.RGBA.size = (size_t)w * h * bpp;
    VP8StatusCode status = WebPDecode(bytes, len, &cfg);
    WebPFreeDecBuffer(&cfg.output);
    out->peak_bytes = atomic_load(&s_peak);
    if (status != VP8_STATUS_OK) {
        muse_webp_free(pixels);
        out->error = status == VP8_STATUS_OUT_OF_MEMORY ? "no_memory" : "decode_failed";
        return false;
    }
    if (cfg.input.has_alpha) {
        uint16_t *rgb = (uint16_t *)pixels;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            unsigned a = pixels[i * 4 + 3];
            unsigned r = pixels[i * 4] * a / 255;
            unsigned g = pixels[i * 4 + 1] * a / 255;
            unsigned b = pixels[i * 4 + 2] * a / 255;
            rgb[i] = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
        }
    }
    *out = (avatar_webp_result_t){(uint16_t *)pixels, w, h, out->peak_bytes, NULL};
    return true;
}
