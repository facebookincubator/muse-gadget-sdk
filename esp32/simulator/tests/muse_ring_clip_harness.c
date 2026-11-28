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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "src/misc/lv_area_private.h"
#include "lvgl.h"
#include "muse_ring_clip.h"

#define W 368
#define H 448
static uint16_t s_pixels[W * H], s_reference[W * H];
static uint8_t s_draw[W * 64 * 2];
static bool s_integer;
static unsigned s_integer_hits;

static void fail(const char *why)
{
    fprintf(stderr, "%s\n", why);
    exit(1);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    bool swapped = lv_display_get_color_format(d) == LV_COLOR_FORMAT_RGB565_SWAPPED;
    const uint16_t *src = (const uint16_t *)p;
    for (int y = a->y1; y <= a->y2; y++) {
        for (int x = a->x1; x <= a->x2; x++) {
            uint16_t c = *src++;
            s_pixels[y * W + x] = swapped ? (uint16_t)((c << 8) | (c >> 8)) : c;
        }
    }
    lv_display_flush_ready(d);
}

static void draw(lv_event_t *e)
{
    if (s_integer) s_integer_hits++;
    /* Frozen pre-optimization Muse hook, except the optimized run replaces
     * sqrt/ceil with the exact integer widths under test. */
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    lv_obj_t *arc = lv_event_get_current_target(e);
    lv_area_t c;
    lv_obj_get_coords(arc, &c);
    int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    int32_t out = lv_area_get_width(&c) / 2 + 1;
    int32_t hole = out - 1 - lv_obj_get_style_arc_width(arc, LV_PART_MAIN) - 2;
    lv_event_stop_processing(e);
    for (int32_t y = LV_MAX(clip.y1, cy - out); y <= LV_MIN(clip.y2, cy + out); y += 52) {
        int32_t y2 = LV_MIN(y + 51, clip.y2);
        int32_t near = y <= cy && cy <= y2 ? 0 : LV_MIN(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t far = LV_MAX(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t ow = s_integer ? muse_ring_outer_width(out, near) :
                     (near < out ? (int32_t)ceilf(sqrtf((float)(out * out - near * near))) : 0);
        int32_t iw = s_integer ? muse_ring_inner_width(hole, far) :
                     (far < hole ? (int32_t)sqrtf((float)(hole * hole - far * far)) : 0);
        lv_area_t pieces[2] = {{cx - ow, y, iw ? cx - iw : cx + ow, y2}, {cx + iw, y, cx + ow, y2}};
        for (int i = 0; i < (iw ? 2 : 1); i++) {
            if (lv_area_intersect(&layer->_clip_area, &clip, &pieces[i])) lv_obj_event_base(NULL, e);
        }
    }
    layer->_clip_area = clip;
}

static void render(lv_display_t *d, bool cached)
{
    s_integer = cached;
    memset(s_pixels, 0x5a, sizeof(s_pixels));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(d);
}

int main(void)
{
    /* Exact sqrt bounds over the complete practical geometry range. The C6
     * table and the fallback must agree with the old float clipping. */
    for (int radius = 1; radius <= 1024; radius++) {
        for (int distance = 0; distance <= radius + 1; distance++) {
            uint32_t n = distance < radius ? (uint32_t)(radius * radius - distance * distance) : 0;
            int outer = (int)ceilf(sqrtf((float)n)), inner = (int)sqrtf((float)n);
            if (muse_ring_outer_width(radius, distance) != outer ||
                muse_ring_inner_width(radius, distance) != inner) fail("integer width mismatch");
        }
    }
    const uint32_t edge[] = {0, 1, 2, 3, 4, 15, 16, UINT32_MAX, UINT32_MAX - 1, UINT32_C(1) << 31};
    for (unsigned i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        uint64_t r = muse_ring_isqrt(edge[i]);
        if (r * r > edge[i] || (r + 1) * (r + 1) <= edge[i]) fail("square root bound failed");
    }
    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_draw, NULL, W * 16 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x312746), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    /* Non-black pixels under the ring exercise alpha compositing, not just
     * a pre-blended RGB copy that would erase the avatar beneath it. */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = lv_obj_create(screen);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 130, 90);
        lv_obj_set_pos(r, i * 70, i * 80);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x805024 + i * 0x132b0f), 0);
    }
    /* An RGB565 source image exercises native-source blending into both
     * destination byte orders, as the on-demand avatar decoder does. */
    static uint16_t image_pixels[32 * 32];
    for (int i = 0; i < 32 * 32; i++) image_pixels[i] = (uint16_t)(i * 137);
    static const lv_image_dsc_t image = {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = 32, .h = 32, .stride = 64},
        .data_size = sizeof(image_pixels), .data = (const uint8_t *)image_pixels,
    };
    lv_obj_t *img = lv_image_create(screen);
    lv_image_set_src(img, &image);
    lv_obj_set_pos(img, 170, 39);
    lv_obj_t *arc = lv_arc_create(screen);
    lv_obj_set_size(arc, 360, 360);
    lv_obj_set_pos(arc, 4, 44);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x140f22), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xa77dff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_add_event_cb(arc, draw, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    const int heights[] = {4, 16, 32, 48, 64};
    const int offsets[] = {4, -1, -77, -187, -359, 365};
    const int angles[][2] = {{0, 0}, {0, 60}, {15, 170}, {290, 350}, {300, 420}, {0, 360}};
    unsigned cases = 0;
    for (unsigned cf = 0; cf < 2; cf++) {
        lv_display_set_color_format(d, cf ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565);
        for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
            lv_display_set_buffers(d, s_draw, NULL, W * heights[h] * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
            for (unsigned x = 0; x < sizeof(offsets) / sizeof(offsets[0]); x++) {
                lv_obj_set_pos(arc, offsets[x], 44);
                for (unsigned a = 0; a < sizeof(angles) / sizeof(angles[0]); a++) {
                    lv_arc_set_angles(arc, angles[a][0], angles[a][1]);
                    render(d, false);
                    memcpy(s_reference, s_pixels, sizeof(s_pixels));
                    unsigned before = s_integer_hits;
                    render(d, true);
                    if (memcmp(s_reference, s_pixels, sizeof(s_pixels))) {
                        for (int p = 0; p < W * H; p++) {
                            if (s_reference[p] != s_pixels[p]) {
                                fprintf(stderr, "cf=%u height=%d x=%d angles=%d,%d pixel=%d,%d native=%04x cache=%04x\n",
                                        cf, heights[h], offsets[x], angles[a][0], angles[a][1], p % W, p / W,
                                        s_reference[p], s_pixels[p]);
                                break;
                            }
                        }
                        fail("cached ring differs from LVGL");
                    }
                    if (offsets[x] == 4 && s_integer_hits == before) fail("cache wasn't exercised");
                    cases++;
                }
            }
        }
    }
    /* A changed geometry/style must fall back instead of drawing stale pixels. */
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(arc, 4, 44);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
    render(d, false);
    memcpy(s_reference, s_pixels, sizeof(s_pixels));
    unsigned before = s_integer_hits;
    render(d, true);
    if (s_integer_hits == before || memcmp(s_reference, s_pixels, sizeof(s_pixels))) fail("fallback failed");
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_arc_set_angles(arc, 0, 0);
    lv_display_set_buffers(d, s_draw, NULL, W * 48 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    double times[2];
    for (int cached = 0; cached < 2; cached++) {
        clock_t start = clock();
        for (int i = 0; i < 100; i++) render(d, cached != 0);
        times[cached] = (double)(clock() - start) / CLOCKS_PER_SEC * 1000 / 100;
    }
    printf("%u exact framebuffer comparisons passed; native %.3f / cached %.3f ms per host refresh (not C6 FPS)\n",
           cases, times[0], times[1]);
    lv_display_delete(d);
    lv_deinit();
    return 0;
}
