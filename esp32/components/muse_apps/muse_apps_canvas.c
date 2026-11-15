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

/*
 * The API's cv_* group: immediate-mode drawing on a W_CANVAS object. Each
 * canvas has an RGB565 buffer in the app's budget, sized by P_WIDTH and
 * P_HEIGHT, and each call draws into it at once through an LVGL layer, so it
 * shows on the next frame. Under LVGL's lock, as the object table is.
 */

#include <string.h>

#include "lvgl.h"
#include "muse_apps_priv.h"
#include "muse_apps_ui_priv.h"

#define MAX_SIDE 4096   /* a canvas's width or height, before the pixel limit */

typedef struct {
    uint8_t *buf;          /* RGB565, or NULL until both sides are set */
    int32_t w, h;          /* the buffer's */
    int32_t want_w, want_h;
    lv_opa_t opa;          /* cv_opa: for the calls that follow */
} canvas_t;

/* ---- The buffer (from the object table) ------------------------------------------------------ */

int32_t muse_apps_canvas_size(lv_obj_t *obj, void **ext, bool width, int32_t px)
{
    if (px < 1 || px > MAX_SIDE) {
        return GADGET_ERR_ARG;   /* pixels only: no PCT or SIZE_CONTENT */
    }
    canvas_t *c = *ext;
    if (!c) {
        c = muse_apps_calloc(1, sizeof(canvas_t));
        if (!c) {
            return GADGET_ERR_NO_MEMORY;
        }
        c->opa = LV_OPA_COVER;
        *ext = c;
    }
    int32_t w = width ? px : c->want_w, h = width ? c->want_h : px;
    if (!w || !h || (c->buf && w == c->w && h == c->h)) {
        c->want_w = w;
        c->want_h = h;
        return 0;
    }
    uint32_t was = c->buf ? (uint32_t)(c->w * c->h) : 0, now = (uint32_t)(w * h);
    muse_apps_ui_pixels_give(was);
    if (!muse_apps_ui_pixels_take(now)) {
        muse_apps_ui_pixels_take(was);
        return GADGET_ERR_NO_MEMORY;
    }
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_RGB565);
    uint8_t *buf = muse_apps_alloc((size_t)stride * (size_t)h);
    if (!buf) {
        muse_apps_ui_pixels_give(now);
        muse_apps_ui_pixels_take(was);
        return GADGET_ERR_NO_MEMORY;
    }
    memset(buf, 0, (size_t)stride * (size_t)h);   /* black; a resize starts again */
    lv_canvas_set_buffer(obj, buf, w, h, LV_COLOR_FORMAT_RGB565);
    muse_apps_free(c->buf);
    c->buf = buf;
    c->w = c->want_w = w;
    c->h = c->want_h = h;
    return 0;
}

int32_t muse_apps_canvas_get_size(void *ext, bool width)
{
    const canvas_t *c = ext;
    return c ? (width ? c->want_w : c->want_h) : 0;
}

void muse_apps_canvas_free(void *ext)
{
    canvas_t *c = ext;
    if (c) {
        if (c->buf) {
            muse_apps_ui_pixels_give((uint32_t)(c->w * c->h));
        }
        muse_apps_free(c->buf);
        muse_apps_free(c);
    }
}

/* The canvas objects go with the app's other objects (muse_apps_ui_stop),
 * which frees their buffers: nothing is left here. */
void muse_apps_canvas_stop(void) {}

/* ---- Drawing ------------------------------------------------------------------------------------ */

/* Takes LVGL's lock and finds the canvas: NULL if the handle isn't one. A
 * canvas without a buffer yet draws nothing (and *c is NULL). */
static lv_obj_t *canvas_arg(int32_t handle, canvas_t **c)
{
    muse_apps_ui_lock();
    void *ext = NULL;
    lv_obj_t *obj = muse_apps_ui_canvas(handle, &ext);
    *c = ext && ((canvas_t *)ext)->buf ? ext : NULL;
    return obj;
}

static lv_color_t rgb(int32_t color)
{
    return lv_color_hex((uint32_t)color & 0xFFFFFF);
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Coordinates LVGL's drawing can take without overflowing. */
#define C(v) clamp((v), -32767, 32767)

int32_t gadget_cv_fill(wasm_exec_env_t env, int32_t canvas, int32_t color)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    if (!c) {
        return 0;
    }
    if (c->opa >= LV_OPA_MAX) {
        lv_canvas_fill_bg(obj, rgb(color), LV_OPA_COVER);
        return 0;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = rgb(color);
    dsc.bg_opa = c->opa;
    lv_area_t a = {0, 0, c->w - 1, c->h - 1};
    lv_draw_rect(&layer, &dsc, &a);
    lv_canvas_finish_layer(obj, &layer);
    return 0;
}

int32_t gadget_cv_opa(wasm_exec_env_t env, int32_t canvas, int32_t opa)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    void *ext = NULL;
    muse_apps_ui_canvas(canvas, &ext);   /* kept even before the buffer exists */
    if (ext) {
        ((canvas_t *)ext)->opa = (lv_opa_t)clamp(opa, 0, 255);
    }
    return 0;
}

static int32_t rect(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width,
                    int32_t color, bool fill)
{
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    if (!c || w < 1 || h < 1 || (!fill && width < 1) || !c->opa) {
        return 0;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = clamp(radius, 0, GADGET_RADIUS_CIRCLE);
    if (fill) {
        dsc.bg_color = rgb(color);
        dsc.bg_opa = c->opa;
    } else {
        dsc.bg_opa = LV_OPA_TRANSP;
        dsc.border_color = rgb(color);
        dsc.border_opa = c->opa;
        dsc.border_width = clamp(width, 1, 32767);
        dsc.border_side = LV_BORDER_SIDE_FULL;
    }
    int32_t x0 = C(x), y0 = C(y);
    lv_area_t a = {x0, y0, C(x0 + clamp(w, 1, 65534) - 1), C(y0 + clamp(h, 1, 65534) - 1)};
    lv_draw_rect(&layer, &dsc, &a);
    lv_canvas_finish_layer(obj, &layer);
    return 0;
}

int32_t gadget_cv_rect(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                       int32_t radius, int32_t color)
{
    (void)env;
    return rect(canvas, x, y, w, h, radius, 0, color, true);
}

int32_t gadget_cv_border(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t radius, int32_t width, int32_t color)
{
    (void)env;
    return rect(canvas, x, y, w, h, radius, width, color, false);
}

/* A styled rectangle uses only these paint properties, never a widget's
 * layout, transforms or pointers. Missing values keep LVGL's descriptor
 * defaults: white fill, no border/shadow, square corners. */
static void rect_style(const lv_style_t *style, lv_draw_rect_dsc_t *dsc)
{
    lv_style_value_t v;
#define NUM(prop, field) do { \
    if (lv_style_get_prop(style, LV_STYLE_##prop, &v) == LV_STYLE_RES_FOUND) dsc->field = v.num; \
} while (0)
#define COLOR(prop, field) do { \
    if (lv_style_get_prop(style, LV_STYLE_##prop, &v) == LV_STYLE_RES_FOUND) dsc->field = v.color; \
} while (0)
    NUM(RADIUS, radius);
    COLOR(BG_COLOR, bg_color);
    NUM(BG_OPA, bg_opa);
    dsc->bg_grad.stops[0].color = dsc->bg_color;
    COLOR(BG_GRAD_COLOR, bg_grad.stops[1].color);
    NUM(BG_GRAD_DIR, bg_grad.dir);
    dsc->bg_grad.stops[0].opa = LV_OPA_COVER;
    dsc->bg_grad.stops[1].opa = LV_OPA_COVER;
    COLOR(BORDER_COLOR, border_color);
    NUM(BORDER_WIDTH, border_width);
    NUM(BORDER_OPA, border_opa);
    NUM(BORDER_SIDE, border_side);
    COLOR(SHADOW_COLOR, shadow_color);
    NUM(SHADOW_WIDTH, shadow_width);
    NUM(SHADOW_OPA, shadow_opa);
    NUM(SHADOW_OFFSET_X, shadow_offset_x);
    NUM(SHADOW_OFFSET_Y, shadow_offset_y);
    NUM(SHADOW_SPREAD, shadow_spread);
#undef NUM
#undef COLOR
}

static lv_opa_t mix_opa(lv_opa_t a, lv_opa_t b)
{
    return (lv_opa_t)(((unsigned)a * b + 127) / 255);
}

int32_t gadget_cv_rect_style(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                             int32_t style)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    const lv_style_t *st = muse_apps_ui_style(style);
    if (!obj || !st) {
        return GADGET_ERR_HANDLE;
    }
    if (w < 1 || h < 1 || w > MAX_SIDE || h > MAX_SIDE) {
        return GADGET_ERR_ARG;
    }
    if (!c) {
        return 0;
    }
    /* Do not let a small canvas request a huge off-screen shadow. */
    if (w > c->w || h > c->h) {
        return GADGET_ERR_ARG;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    rect_style(st, &dsc);
    if (dsc.bg_grad.dir != LV_GRAD_DIR_NONE && dsc.bg_grad.dir != LV_GRAD_DIR_HOR &&
        dsc.bg_grad.dir != LV_GRAD_DIR_VER) {
        dsc.bg_grad.dir = LV_GRAD_DIR_NONE;
    }
    dsc.radius = clamp(dsc.radius, 0, GADGET_RADIUS_CIRCLE);
    dsc.border_width = clamp(dsc.border_width, 0, 64);
    dsc.shadow_width = clamp(dsc.shadow_width, 0, 64);
    dsc.shadow_spread = clamp(dsc.shadow_spread, 0, 64);   /* negative: LVGL inverts the shadow's core box and overruns its buffer */
    dsc.shadow_offset_x = clamp(dsc.shadow_offset_x, -MAX_SIDE, MAX_SIDE);
    dsc.shadow_offset_y = clamp(dsc.shadow_offset_y, -MAX_SIDE, MAX_SIDE);
    /* cv_opa and P_OPA multiply each paint property's own opacity. */
    lv_style_value_t v;
    lv_opa_t opa = c->opa;
    if (lv_style_get_prop(st, LV_STYLE_OPA, &v) == LV_STYLE_RES_FOUND) {
        opa = mix_opa(opa, (lv_opa_t)v.num);
    }
    dsc.bg_opa = mix_opa(dsc.bg_opa, opa);
    dsc.border_opa = mix_opa(dsc.border_opa, opa);
    dsc.shadow_opa = mix_opa(dsc.shadow_opa, opa);
    if (!opa) {
        return 0;
    }
    int32_t x0 = C(x), y0 = C(y);
    lv_area_t a = {x0, y0, C(x0 + w - 1), C(y0 + h - 1)};
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_rect(&layer, &dsc, &a);
    lv_canvas_finish_layer(obj, &layer);
    return 0;
}

int32_t gadget_cv_line(wasm_exec_env_t env, int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       int32_t width, int32_t color)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    if (!c || width < 1 || !c->opa) {
        return 0;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = C(x1);
    dsc.p1.y = C(y1);
    dsc.p2.x = C(x2);
    dsc.p2.y = C(y2);
    dsc.width = clamp(width, 1, 1000);
    dsc.color = rgb(color);
    dsc.opa = c->opa;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_draw_line(&layer, &dsc);
    lv_canvas_finish_layer(obj, &layer);
    return 0;
}

int32_t gadget_cv_arc(wasm_exec_env_t env, int32_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t width,
                      int32_t start, int32_t end, int32_t color)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    if (!c || r < 1 || width < 1 || !c->opa) {
        return 0;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center.x = C(cx);
    dsc.center.y = C(cy);
    dsc.radius = (uint16_t)clamp(r, 1, 32767);
    dsc.width = clamp(width, 1, r);
    /* Degrees clockwise from 3 o'clock, as LVGL's. */
    int32_t from = ((start % 360) + 360) % 360, to = ((end % 360) + 360) % 360;
    if (from == to && start != end) {
        to = from + 360;   /* a whole turn, not nothing */
    }
    dsc.start_angle = from;
    dsc.end_angle = to;
    dsc.color = rgb(color);
    dsc.opa = c->opa;
    lv_draw_arc(&layer, &dsc);
    lv_canvas_finish_layer(obj, &layer);
    return 0;
}

int32_t gadget_cv_triangle(wasm_exec_env_t env, int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           int32_t x3, int32_t y3, int32_t color)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    if (!c || !c->opa) {
        return 0;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.p[0].x = C(x1);
    dsc.p[0].y = C(y1);
    dsc.p[1].x = C(x2);
    dsc.p[1].y = C(y2);
    dsc.p[2].x = C(x3);
    dsc.p[2].y = C(y3);
    dsc.color = rgb(color);
    dsc.opa = c->opa;
    lv_draw_triangle(&layer, &dsc);
    lv_canvas_finish_layer(obj, &layer);
    return 0;
}

int32_t gadget_cv_text(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t font,
                       int32_t color, int32_t align, const char *text)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    const lv_font_t *f = muse_apps_ui_font(font);
    if (!f || w < 1 || align < GADGET_TEXT_ALIGN_AUTO || align > GADGET_TEXT_ALIGN_RIGHT || strlen(text) > 4096) {
        return GADGET_ERR_ARG;
    }
    w = clamp(w, 1, 32767);
    lv_point_t size;
    lv_text_get_size(&size, text, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    if (!c || !c->opa || !*text) {
        return size.y;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(obj, &layer);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.font = f;
    dsc.color = rgb(color);
    dsc.opa = c->opa;
    dsc.align = (lv_text_align_t)align;
    int32_t x0 = C(x), y0 = C(y);
    lv_area_t a = {x0, y0, C(x0 + w - 1), C(y0 + size.y - 1)};
    lv_draw_label(&layer, &dsc, &a);
    lv_canvas_finish_layer(obj, &layer);   /* done with text before this returns */
    return size.y;
}

int32_t gadget_cv_pixels(wasm_exec_env_t env, int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                         const void *data, uint32_t data_len)
{
    (void)env;
    canvas_t *c;
    lv_obj_t *obj = canvas_arg(canvas, &c);
    if (!obj) {
        return GADGET_ERR_HANDLE;
    }
    if (w < 1 || h < 1 || w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * h * 2 > data_len) {
        return GADGET_ERR_ARG;
    }
    if (!c) {
        return 0;
    }
    /* Clipped to the canvas; rows of little-endian RGB565, which is LVGL's
     * RGB565 on these chips. */
    int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + w > c->w ? c->w : (int64_t)x + w, y1 = (int64_t)y + h > c->h ? c->h : (int64_t)y + h;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)c->w, LV_COLOR_FORMAT_RGB565);
    const uint8_t *src = data;
    for (int64_t row = y0; row < y1; row++) {
        memcpy(c->buf + (size_t)row * stride + (size_t)x0 * 2,
               src + ((size_t)(row - y) * (size_t)w + (size_t)(x0 - x)) * 2, (size_t)(x1 - x0) * 2);
    }
    lv_obj_invalidate(obj);
    return 0;
}
