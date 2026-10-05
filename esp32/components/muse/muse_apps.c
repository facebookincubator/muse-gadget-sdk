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
 * Apps: pages below the face, built from JSON (muse_apps.h). Each app is a
 * tile of the UI's tileview in the face's column; its widgets are LVGL
 * objects in PSRAM (muse_lv_mem.c), found by id for updates and events.
 */
#include "muse_apps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "rom/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"    /* a replaced picture mustn't be cached */
#include "src/widgets/tileview/lv_tileview_private.h"   /* a tile's scroll directions */

#include "muse_board.h"
#include "muse_hw.h"
#include "muse_input.h"
#if CONFIG_MUSE_HATCH
#include "muse_chat.h"
#endif
#include "muse_state.h"
#include "muse_ui.h"

static const char *TAG = "muse_apps";

#define WIDGETS_MAX 96
#define DEPTH_MAX 8
#define ID_MAX 16
#define LIVE_MS 100            /* a slider or arc with "live": changes while dragging, this often */
#define JPEG_POOL 3100
#define DOT 8

typedef enum {
    W_BOX, W_ROW, W_COLUMN, W_LABEL, W_BUTTON, W_SWITCH, W_CHECKBOX, W_SLIDER, W_ARC, W_BAR, W_SPINNER,
    W_IMAGE, W_CHART, W_LED, W_ROLLER, W_DROPDOWN, W_LINE, W_CANVAS, W_QR, W_TABLE, W_INPUT,
} wtype_t;

static const char *const TYPES[] = {
    "box", "row", "column", "label", "button", "switch", "checkbox", "slider", "arc", "bar", "spinner",
    "image", "chart", "led", "roller", "dropdown", "line", "canvas", "qr", "table", "input",
};

typedef struct app app_t;

typedef struct {
    char id[ID_MAX];
    wtype_t type;
    app_t *app;
    lv_obj_t *obj;
    lv_obj_t *label;              /* a button's text */
    uint16_t *pixels;             /* an image's picture (PSRAM) */
    lv_image_dsc_t dsc;
    lv_draw_buf_t *canvas;        /* a canvas's buffer */
    lv_chart_series_t *series[4];
    int nseries;
    lv_point_precise_t *points;   /* a line's */
    bool live;
    bool click_hooked;
    bool talk_hooked, talking;    /* a "talk" button: held, Muse listens */
    bool talk_photo;              /* ...and its script takes a photo for the question */
    int64_t last_live_us;
} widget_t;

struct app {
    char id[ID_MAX];
    char title[32];
    int order;
    lv_obj_t *tile;
    widget_t *w;                  /* WIDGETS_MAX, PSRAM */
    int nw;
};

static app_t *s_apps[MUSE_APPS_MAX];
static lv_obj_t *s_shown;         /* the tile the last tick saw */
static bool s_dots_stale;         /* relayout moved them: redraw at the next tick */
static bool s_reshow;             /* set from any task: "show" again for the page on screen */
static lv_obj_t *s_dots[MUSE_APPS_MAX + 1];
static lv_obj_t *s_keyboard;
static muse_apps_loader_fn s_loader;

void muse_apps_set_loader(muse_apps_loader_fn fn)
{
    s_loader = fn;
}

/* ---- Values from JSON ---- */

static bool lock(void)
{
    return muse_board && muse_ui_tileview() && muse_board->display_lock(-1);
}

static void unlock(void)
{
    muse_board->display_unlock();
}

static const char *str(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool num(const cJSON *j, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (cJSON_IsNumber(v)) {
        *out = v->valueint;
        return true;
    }
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    return false;
}

static bool flag(const cJSON *j, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (!cJSON_IsBool(v) && !cJSON_IsNumber(v)) {
        return false;
    }
    *out = cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valueint);
    return true;
}

static const struct {
    const char *name;
    uint32_t rgb;
} COLORS[] = {
    { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff3b30 }, { "green", 0x34c759 },
    { "blue", 0x0a84ff }, { "yellow", 0xffd60a }, { "orange", 0xff9f0a }, { "purple", 0xbf5af2 },
    { "pink", 0xff375f }, { "cyan", 0x64d2ff }, { "teal", 0x30b0c7 }, { "gray", 0x8e8e93 },
    { "grey", 0x8e8e93 }, { "muse", 0xa77dff }, { "dark", 0x1c1c1e },
};

static bool color(const cJSON *j, const char *key, lv_color_t *out)
{
    const char *s = str(j, key);
    if (!s) {
        return false;
    }
    for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); i++) {
        if (!strcasecmp(s, COLORS[i].name)) {
            *out = lv_color_hex(COLORS[i].rgb);
            return true;
        }
    }
    if (*s == '#') {
        s++;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end - s == 3) {   /* #rgb */
        v = (v >> 8 & 0xf) * 0x110000 | (v >> 4 & 0xf) * 0x1100 | (v & 0xf) * 0x11;
    } else if (end - s != 6) {
        return false;
    }
    *out = lv_color_hex((uint32_t)v);
    return true;
}

/* 12, 14 .. 48: the nearest font there is; "mono" for the pixel font. */
static const lv_font_t *font_of(const cJSON *j)
{
    const cJSON *f = cJSON_GetObjectItem(j, "font");
    if (cJSON_IsString(f) && !strcmp(f->valuestring, "mono")) {
        return &lv_font_unscii_16;
    }
    if (!cJSON_IsNumber(f)) {
        return NULL;
    }
    static const struct {
        int px;
        const lv_font_t *font;
    } fonts[] = {
        { 8, &lv_font_unscii_8 },
        { 14, &lv_font_montserrat_14 },
        { 16, &lv_font_montserrat_16 },
        { 20, &lv_font_montserrat_20 },
        { 28, &lv_font_montserrat_28 },
#if LV_FONT_MONTSERRAT_36
        { 36, &lv_font_montserrat_36 },
#endif
#if LV_FONT_MONTSERRAT_48
        { 48, &lv_font_montserrat_48 },
#endif
    };
    const lv_font_t *best = fonts[0].font;
    int d = 1000;
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        int e = abs(fonts[i].px - f->valueint);
        if (e < d) {
            d = e;
            best = fonts[i].font;
        }
    }
    return best;
}

/* 120, "50%", "content", "grow" (a flex share). */
static bool size_of(const cJSON *v, int32_t *out, bool *grow)
{
    *grow = false;
    if (cJSON_IsNumber(v)) {
        *out = v->valueint;
        return true;
    }
    if (!cJSON_IsString(v)) {
        return false;
    }
    if (!strcmp(v->valuestring, "content")) {
        *out = LV_SIZE_CONTENT;
        return true;
    }
    if (!strcmp(v->valuestring, "grow")) {
        *grow = true;
        return true;
    }
    int pct = atoi(v->valuestring);
    if (strchr(v->valuestring, '%') && pct > 0 && pct <= 100) {
        *out = lv_pct(pct);
        return true;
    }
    return false;
}

static const struct {
    const char *name;
    const char *sym;
} ICONS[] = {
    { "play", LV_SYMBOL_PLAY }, { "pause", LV_SYMBOL_PAUSE }, { "stop", LV_SYMBOL_STOP },
    { "next", LV_SYMBOL_NEXT }, { "prev", LV_SYMBOL_PREV }, { "home", LV_SYMBOL_HOME },
    { "wifi", LV_SYMBOL_WIFI }, { "bell", LV_SYMBOL_BELL }, { "power", LV_SYMBOL_POWER },
    { "settings", LV_SYMBOL_SETTINGS }, { "ok", LV_SYMBOL_OK }, { "close", LV_SYMBOL_CLOSE },
    { "plus", LV_SYMBOL_PLUS }, { "minus", LV_SYMBOL_MINUS }, { "up", LV_SYMBOL_UP },
    { "down", LV_SYMBOL_DOWN }, { "left", LV_SYMBOL_LEFT }, { "right", LV_SYMBOL_RIGHT },
    { "volume", LV_SYMBOL_VOLUME_MAX }, { "mute", LV_SYMBOL_MUTE }, { "battery", LV_SYMBOL_BATTERY_FULL },
    { "charge", LV_SYMBOL_CHARGE }, { "image", LV_SYMBOL_IMAGE }, { "eye", LV_SYMBOL_EYE_OPEN },
    { "gps", LV_SYMBOL_GPS }, { "refresh", LV_SYMBOL_REFRESH }, { "trash", LV_SYMBOL_TRASH },
    { "edit", LV_SYMBOL_EDIT }, { "warning", LV_SYMBOL_WARNING }, { "bluetooth", LV_SYMBOL_BLUETOOTH },
    { "loop", LV_SYMBOL_LOOP }, { "shuffle", LV_SYMBOL_SHUFFLE }, { "audio", LV_SYMBOL_AUDIO },
    { "video", LV_SYMBOL_VIDEO }, { "list", LV_SYMBOL_LIST }, { "save", LV_SYMBOL_SAVE },
    { "file", LV_SYMBOL_FILE }, { "download", LV_SYMBOL_DOWNLOAD }, { "upload", LV_SYMBOL_UPLOAD },
    { "call", LV_SYMBOL_CALL }, { "cut", LV_SYMBOL_CUT }, { "copy", LV_SYMBOL_COPY },
    { "keyboard", LV_SYMBOL_KEYBOARD }, { "sd", LV_SYMBOL_SD_CARD }, { "usb", LV_SYMBOL_USB },
    { "eject", LV_SYMBOL_EJECT }, { "shift", LV_SYMBOL_NEW_LINE }, { "backspace", LV_SYMBOL_BACKSPACE },
    { "dummy", "" },
};

static const char *icon_of(const char *name)
{
    for (size_t i = 0; name && i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
        if (!strcmp(name, ICONS[i].name)) {
            return ICONS[i].sym;
        }
    }
    return NULL;
}

static lv_flex_align_t flex_align(const char *s)
{
    if (!s) return LV_FLEX_ALIGN_CENTER;
    if (!strcmp(s, "start")) return LV_FLEX_ALIGN_START;
    if (!strcmp(s, "end")) return LV_FLEX_ALIGN_END;
    if (!strcmp(s, "between")) return LV_FLEX_ALIGN_SPACE_BETWEEN;
    if (!strcmp(s, "around")) return LV_FLEX_ALIGN_SPACE_AROUND;
    if (!strcmp(s, "evenly")) return LV_FLEX_ALIGN_SPACE_EVENLY;
    return LV_FLEX_ALIGN_CENTER;
}

static lv_align_t align_of(const char *s)
{
    static const struct {
        const char *name;
        lv_align_t a;
    } A[] = {
        { "center", LV_ALIGN_CENTER }, { "top", LV_ALIGN_TOP_MID }, { "bottom", LV_ALIGN_BOTTOM_MID },
        { "left", LV_ALIGN_LEFT_MID }, { "right", LV_ALIGN_RIGHT_MID }, { "top_left", LV_ALIGN_TOP_LEFT },
        { "top_right", LV_ALIGN_TOP_RIGHT }, { "bottom_left", LV_ALIGN_BOTTOM_LEFT },
        { "bottom_right", LV_ALIGN_BOTTOM_RIGHT },
    };
    for (size_t i = 0; s && i < sizeof(A) / sizeof(A[0]); i++) {
        if (!strcmp(s, A[i].name)) return A[i].a;
    }
    return LV_ALIGN_DEFAULT;
}

/* "a\nb" or ["a", "b"] as roller/dropdown options. */
static char *options_of(const cJSON *v)
{
    if (cJSON_IsString(v)) {
        return strdup(v->valuestring);
    }
    if (!cJSON_IsArray(v)) {
        return NULL;
    }
    size_t len = 1;
    const cJSON *o;
    cJSON_ArrayForEach(o, v) len += cJSON_IsString(o) ? strlen(o->valuestring) + 1 : 0;
    char *s = malloc(len);
    if (!s) return NULL;
    s[0] = '\0';
    cJSON_ArrayForEach(o, v) {
        if (!cJSON_IsString(o)) continue;
        if (s[0]) strcat(s, "\n");
        strcat(s, o->valuestring);
    }
    return s;
}

/* ---- Events ---- */

static void post(widget_t *w, const char *event, int value, const char *text)
{
    muse_state_poke();
    muse_hw_ui(w->app->id, w->id, event, value, text);
}

/* A short click, so a long press isn't also a click when it's let go. */
static void on_click(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    post(w, code == LV_EVENT_LONG_PRESSED ? "long_press" : "click", 0, NULL);
}

/* A "talk" button is a talk key: held, Muse listens, as for the board's own
 * button; its script hears "press" and "release" (to take a photo, say).
 * Anything but a press ends the talk, so a key reset or deleted while held
 * (its page redefined or removed) still lets Muse stop listening. */
static void on_talk(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    bool down = code == LV_EVENT_PRESSED;
    if (down == w->talking) {
        return;   /* a release with no talk going */
    }
    w->talking = down;
    if (code == LV_EVENT_DELETE) {
        muse_input_talk(false);   /* no "release": its page is going away */
        return;
    }
    if (down) {
        muse_ui_keep_page();   /* Muse listens here, where the talk key is */
#if CONFIG_MUSE_HATCH
        if (w->talk_photo) {
            muse_chat_expect_photo();   /* the note waits for it, to send it first */
        }
#endif
    }
    muse_input_talk(down);
    post(w, down ? "press" : "release", 0, NULL);
}

static void on_value(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    int value = 0;
    char text[48] = "";
    switch (w->type) {
    case W_SWITCH:
    case W_CHECKBOX:
        value = lv_obj_has_state(w->obj, LV_STATE_CHECKED);
        break;
    case W_SLIDER:
        value = lv_slider_get_value(w->obj);
        break;
    case W_ARC:
        value = lv_arc_get_value(w->obj);
        break;
    case W_ROLLER:
        value = lv_roller_get_selected(w->obj);
        lv_roller_get_selected_str(w->obj, text, sizeof(text));
        break;
    case W_DROPDOWN:
        value = lv_dropdown_get_selected(w->obj);
        lv_dropdown_get_selected_str(w->obj, text, sizeof(text));
        break;
    default:
        break;
    }
    /* Sliders and arcs report where they're let go; "live" ones while dragged too. */
    if ((w->type == W_SLIDER || w->type == W_ARC) && code == LV_EVENT_VALUE_CHANGED) {
        int64_t now = esp_timer_get_time();
        if (!w->live || now - w->last_live_us < LIVE_MS * 1000) {
            return;
        }
        w->last_live_us = now;
    }
    post(w, "change", value, text);
}

static void keyboard_done(lv_event_t *e);

/* Closes the keyboard and lets go of its input (and of the widget its
 * callbacks point at). With the display lock held. */
static void keyboard_close(void)
{
    if (!s_keyboard) return;
    bool open = !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    while (lv_obj_remove_event_cb(s_keyboard, keyboard_done)) {
    }
    if (open) muse_ui_set_swipe_enabled(true);
}

static void keyboard_done(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        post(w, "submit", 0, lv_textarea_get_text(w->obj));
    }
    lv_obj_remove_state(w->obj, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(s_keyboard, NULL);   /* nothing left pointing at the input (lv_keyboard re-checks) */
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    muse_ui_set_swipe_enabled(true);
}

/* A tapped input: the keyboard, over the lower part of the screen. */
static void on_input(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(s_keyboard, lv_pct(96), lv_pct(46));
        lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
    while (lv_obj_remove_event_cb(s_keyboard, keyboard_done)) {
    }
    lv_obj_add_event_cb(s_keyboard, keyboard_done, LV_EVENT_READY, w);
    lv_obj_add_event_cb(s_keyboard, keyboard_done, LV_EVENT_CANCEL, w);
    lv_keyboard_set_textarea(s_keyboard, w->obj);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    muse_ui_set_swipe_enabled(false);   /* typing mustn't slide the page away */
}

/* ---- Images ---- */

typedef struct {
    const uint8_t *jpeg;
    size_t len, pos;
    uint16_t *out;
    int w, h, dx, dy;
} decode_t;

static UINT dec_in(JDEC *jd, BYTE *buf, UINT len)
{
    decode_t *d = jd->device;
    if (d->pos + len > d->len) len = (UINT)(d->len - d->pos);
    if (buf) memcpy(buf, d->jpeg + d->pos, len);
    d->pos += len;
    return len;
}

static UINT dec_out(JDEC *jd, void *bitmap, JRECT *r)
{
    decode_t *d = jd->device;
    const uint8_t *rgb = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++, rgb += 3) {
            int sx = x + d->dx, sy = y + d->dy;
            if (sx >= 0 && sy >= 0 && sx < d->w && sy < d->h) {
                d->out[sy * d->w + sx] = (rgb[0] & 0xF8) << 8 | (rgb[1] & 0xFC) << 3 | rgb[2] >> 3;
            }
        }
    }
    return 1;
}

/* Decodes into a new buffer the widget's size (or the picture's, shrunk to
 * fit the screen if the widget has none); with the display lock held. */
/* The widget's box: its size, or the screen's if it has none; true if sized.
 * With the lock held. */
static bool image_box(widget_t *w, int *bw, int *bh)
{
    lv_obj_update_layout(w->obj);
    *bw = lv_obj_get_width(w->obj);
    *bh = lv_obj_get_height(w->obj);
    bool sized = *bw >= 8;
    if (*bw < 8 || *bh < 8) {
        *bw = muse_board->width;
        *bh = muse_board->height;
    }
    return sized;
}

/* Decodes into a new PSRAM buffer, bw x bh (or the picture shrunk to fit if
 * the widget has no size). No LVGL, so no lock: a camera frame takes ~100 ms,
 * and holding the lock that long starves touch and scrolling. */
static uint16_t *decode_jpeg(const uint8_t *jpeg, size_t len, int bw, int bh, bool sized, int *ow, int *oh)
{
    void *pool = heap_caps_malloc(JPEG_POOL, MALLOC_CAP_SPIRAM);   /* per call: decodes may overlap */
    if (!pool) return NULL;
    decode_t d = { .jpeg = jpeg, .len = len };
    JDEC jd;
    uint16_t *px = NULL;
    if (jd_prepare(&jd, dec_in, pool, JPEG_POOL, &d) == JDR_OK) {
        uint8_t scale = 0;
        while (scale < 3 && ((int)(jd.width >> scale) > bw || (int)(jd.height >> scale) > bh)) scale++;
        int iw = jd.width >> scale, ih = jd.height >> scale;
        *ow = sized ? bw : iw;
        *oh = sized ? bh : ih;
        px = heap_caps_calloc((size_t)*ow * *oh, 2, MALLOC_CAP_SPIRAM);
        if (px) {
            d.out = px;
            d.w = *ow;
            d.h = *oh;
            d.dx = (*ow - iw) / 2;
            d.dy = (*oh - ih) / 2;
            if (jd_decomp(&jd, dec_out, scale) != JDR_OK) {
                free(px);
                px = NULL;
            }
        }
    }
    free(pool);
    return px;
}

/* Shows a decoded picture; the widget takes the buffer. With the lock held. */
static void image_set(widget_t *w, uint16_t *px, int ow, int oh)
{
    w->dsc = (lv_image_dsc_t){ 0 };
    w->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    w->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    w->dsc.header.w = ow;
    w->dsc.header.h = oh;
    w->dsc.header.stride = ow * 2;
    w->dsc.data_size = (size_t)ow * oh * 2;
    w->dsc.data = (const uint8_t *)px;
    lv_image_set_src(w->obj, &w->dsc);
    lv_image_cache_drop(&w->dsc);
    free(w->pixels);
    w->pixels = px;
    lv_obj_invalidate(w->obj);
}

/* A "b64:" source, from apply(), with the lock held. */
static bool image_from_jpeg(widget_t *w, const uint8_t *jpeg, size_t len)
{
    int bw, bh, ow, oh;
    bool sized = image_box(w, &bw, &bh);
    uint16_t *px = decode_jpeg(jpeg, len, bw, bh, sized, &ow, &oh);
    if (!px) return false;
    image_set(w, px, ow, oh);
    return true;
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* "b64:" inline, "sd:" and "url:" through the loader, "camera" left for camera.preview. */
static void image_src(widget_t *w, const char *src)
{
    if (!strncmp(src, "b64:", 4)) {
        size_t n = strlen(src + 4);
        uint8_t *jpeg = heap_caps_malloc(n / 4 * 3 + 4, MALLOC_CAP_SPIRAM);
        if (!jpeg) return;
        uint32_t acc = 0;
        int bits = 0;
        size_t len = 0;
        for (const char *p = src + 4; *p; p++) {
            int v = b64v(*p);
            if (v < 0) break;
            acc = acc << 6 | (uint32_t)v;
            if ((bits += 6) >= 8) jpeg[len++] = (uint8_t)(acc >> (bits -= 8));
        }
        if (!image_from_jpeg(w, jpeg, len)) ESP_LOGW(TAG, "%s.%s: not a baseline JPEG", w->app->id, w->id);
        free(jpeg);
    } else if ((!strncmp(src, "sd:", 3) || !strncmp(src, "url:", 4)) && s_loader) {
        s_loader(w->app->id, w->id, src);   /* it comes back through muse_apps_set_jpeg */
    }
}

/* ---- Canvas drawing ---- */

static void canvas_draw(widget_t *w, const cJSON *ops, bool clear, lv_color_t bg)
{
    if (!w->canvas) return;
    if (clear) lv_canvas_fill_bg(w->obj, bg, LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(w->obj, &layer);
    const cJSON *op;
    cJSON_ArrayForEach(op, ops) {
        const char *kind = str(op, "op");
        lv_color_t c = lv_color_white();
        color(op, "color", &c);
        int x = 0, y = 0, width = 2, r = 10;
        num(op, "x", &x);
        num(op, "y", &y);
        num(op, "width", &width);
        num(op, "r", &r);
        bool fill = false;
        flag(op, "fill", &fill);
        if (!kind) continue;
        if (!strcmp(kind, "rect") || !strcmp(kind, "circle")) {
            int ww = 20, hh = 20, radius = 0;
            num(op, "w", &ww);
            num(op, "h", &hh);
            num(op, "radius", &radius);
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_color = c;
            d.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
            d.border_color = c;
            d.border_width = fill ? 0 : width;
            d.radius = radius;
            lv_area_t a = { x, y, x + ww - 1, y + hh - 1 };
            if (!strcmp(kind, "circle")) {
                d.radius = LV_RADIUS_CIRCLE;
                a = (lv_area_t){ x - r, y - r, x + r, y + r };
            }
            lv_draw_rect(&layer, &d, &a);
        } else if (!strcmp(kind, "line")) {
            const cJSON *pts = cJSON_GetObjectItem(op, "points");
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.color = c;
            d.width = width;
            d.round_start = d.round_end = 1;
            for (int i = 0; i + 1 < cJSON_GetArraySize(pts); i++) {
                const cJSON *a = cJSON_GetArrayItem(pts, i), *b = cJSON_GetArrayItem(pts, i + 1);
                if (cJSON_GetArraySize(a) < 2 || cJSON_GetArraySize(b) < 2) continue;
                d.p1.x = cJSON_GetArrayItem(a, 0)->valueint;
                d.p1.y = cJSON_GetArrayItem(a, 1)->valueint;
                d.p2.x = cJSON_GetArrayItem(b, 0)->valueint;
                d.p2.y = cJSON_GetArrayItem(b, 1)->valueint;
                lv_draw_line(&layer, &d);
            }
        } else if (!strcmp(kind, "arc")) {
            int start = 0, end = 360;
            num(op, "start", &start);
            num(op, "end", &end);
            lv_draw_arc_dsc_t d;
            lv_draw_arc_dsc_init(&d);
            d.color = c;
            d.width = width;
            d.center.x = x;
            d.center.y = y;
            d.radius = r;
            d.start_angle = start;
            d.end_angle = end;
            lv_draw_arc(&layer, &d);
        } else if (!strcmp(kind, "text")) {
            const char *t = str(op, "text");
            if (!t) continue;
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.color = c;
            const lv_font_t *f = font_of(op);
            if (f) d.font = f;
            d.text = t;   /* the JSON outlives finish_layer below */
            lv_area_t a = { x, y, x + 400, y + 100 };
            lv_draw_label(&layer, &d, &a);
        }
    }
    lv_canvas_finish_layer(w->obj, &layer);
}

/* ---- Widgets ---- */

static widget_t *find_widget(app_t *a, const char *id)
{
    for (int i = 0; id && id[0] && i < a->nw; i++) {
        if (!strcmp(a->w[i].id, id)) return &a->w[i];
    }
    return NULL;
}

static void set_text(widget_t *w, const char *text, const char *icon)
{
    lv_obj_t *l = w->type == W_BUTTON ? w->label : w->obj;
    if (!l) return;
    const char *sym = icon_of(icon);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s%s", sym ? sym : "", sym && text && text[0] ? " " : "", text ? text : "");
    if (w->type == W_CHECKBOX) lv_checkbox_set_text(l, buf);
    else lv_label_set_text(l, buf);
}

/* Everything a description or an update can set, on creation or later. */
static void apply(widget_t *w, const cJSON *j)
{
    lv_obj_t *o = w->obj;
    int32_t sz;
    bool grow;
    if (size_of(cJSON_GetObjectItem(j, "w"), &sz, &grow)) {
        if (grow) lv_obj_set_flex_grow(o, 1);
        else lv_obj_set_width(o, sz);
    }
    if (size_of(cJSON_GetObjectItem(j, "h"), &sz, &grow)) {
        if (grow) lv_obj_set_flex_grow(o, 1);
        else lv_obj_set_height(o, sz);
    }
    int x = 0, y = 0, v;
    bool has_x = num(j, "x", &x), has_y = num(j, "y", &y);
    const char *al = str(j, "align");
    if (al) lv_obj_align(o, align_of(al), x, y);
    else if (has_x || has_y) lv_obj_set_pos(o, x, y);
    lv_color_t c;
    if (color(j, "bg", &c)) {
        lv_obj_set_style_bg_color(o, c, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    }
    if (num(j, "bg_opa", &v)) lv_obj_set_style_bg_opa(o, (lv_opa_t)(v * 255 / 100), 0);
    if (num(j, "opa", &v)) lv_obj_set_style_opa(o, (lv_opa_t)(v * 255 / 100), 0);
    if (num(j, "radius", &v)) lv_obj_set_style_radius(o, v, 0);
    if (num(j, "border", &v)) lv_obj_set_style_border_width(o, v, 0);
    if (color(j, "border_color", &c)) lv_obj_set_style_border_color(o, c, 0);
    if (num(j, "pad", &v)) {
        lv_obj_set_style_pad_all(o, v, 0);
        if (w->type == W_TABLE) lv_obj_set_style_pad_all(o, v, LV_PART_ITEMS);   /* and each cell's */
    }
    if (num(j, "gap", &v)) {
        lv_obj_set_style_pad_row(o, v, 0);
        lv_obj_set_style_pad_column(o, v, 0);
    }
    const lv_font_t *f = font_of(j);
    if (f) lv_obj_set_style_text_font(o, f, 0);
    const char *ta = str(j, "text_align");
    if (ta) {
        lv_obj_set_style_text_align(o, !strcmp(ta, "left") ? LV_TEXT_ALIGN_LEFT
                                       : !strcmp(ta, "right") ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
    }
    bool b;
    if (flag(j, "hidden", &b)) lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, b);
    if (flag(j, "scroll", &b)) lv_obj_set_flag(o, LV_OBJ_FLAG_SCROLLABLE, b);
    if (flag(j, "clickable", &b) && w->type != W_BUTTON) {
        lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, b);
        if (b && !w->click_hooked) {
            lv_obj_add_event_cb(o, on_click, LV_EVENT_SHORT_CLICKED, w);
            lv_obj_add_event_cb(o, on_click, LV_EVENT_LONG_PRESSED, w);
            w->click_hooked = true;
        }
    }
    if (flag(j, "live", &b)) w->live = b;
    if (flag(j, "photo", &b) && w->type == W_BUTTON) w->talk_photo = b;
    if (flag(j, "talk", &b) && b && w->type == W_BUTTON && !w->talk_hooked) {
        /* A drag on it doesn't swipe the page, or a swipe begun here would
         * also talk (and take a photo); the finger stays on it till let go. */
        lv_obj_add_flag(o, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
        static const lv_event_code_t EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                                                  LV_EVENT_INDEV_RESET, LV_EVENT_DELETE };
        for (size_t k = 0; k < sizeof(EVENTS) / sizeof(EVENTS[0]); k++) {
            lv_obj_add_event_cb(o, on_talk, EVENTS[k], w);
        }
        w->talk_hooked = true;
    }
    /* Flex, on containers. */
    const char *just = str(j, "justify"), *items = str(j, "items");
    if (just || items) {
        lv_obj_set_flex_align(o, flex_align(just ? just : "center"), flex_align(items ? items : "center"),
                              flex_align(items ? items : "center"));
    }
    if (flag(j, "wrap", &b) && (w->type == W_ROW || w->type == W_COLUMN)) {
        lv_obj_set_flex_flow(o, w->type == W_ROW ? (b ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW)
                                                 : (b ? LV_FLEX_FLOW_COLUMN_WRAP : LV_FLEX_FLOW_COLUMN));
    }

    /* The widget's own. */
    const char *text = str(j, "text"), *icon = str(j, "icon");
    bool has_color = color(j, "color", &c);
    int min = 0, max = 100;
    bool range = num(j, "min", &min) & num(j, "max", &max);
    switch (w->type) {
    case W_LABEL:
    case W_BUTTON:
    case W_CHECKBOX:
        if (text || icon) set_text(w, text, icon);
        if (has_color) lv_obj_set_style_text_color(w->type == W_BUTTON ? w->label : o, c, 0);
        if (w->type == W_LABEL && str(j, "long")) {
            const char *l = str(j, "long");
            lv_label_set_long_mode(o, !strcmp(l, "scroll") ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                      : !strcmp(l, "dots") ? LV_LABEL_LONG_MODE_DOTS
                                      : !strcmp(l, "clip") ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_WRAP);
        }
        if (w->type == W_CHECKBOX && flag(j, "on", &b)) lv_obj_set_state(o, LV_STATE_CHECKED, b);
        break;
    case W_SWITCH:
        if (flag(j, "on", &b)) lv_obj_set_state(o, LV_STATE_CHECKED, b);
        if (has_color) lv_obj_set_style_bg_color(o, c, LV_PART_INDICATOR | LV_STATE_CHECKED);
        break;
    case W_SLIDER:
        if (range) lv_slider_set_range(o, min, max);
        if (num(j, "value", &v)) lv_slider_set_value(o, v, LV_ANIM_ON);
        if (has_color) {
            lv_obj_set_style_bg_color(o, c, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(o, c, LV_PART_KNOB);
        }
        break;
    case W_ARC: {
        if (range) lv_arc_set_range(o, min, max);
        if (num(j, "value", &v)) lv_arc_set_value(o, v);
        int s, e;
        if (num(j, "start", &s) & num(j, "end", &e)) lv_arc_set_bg_angles(o, s, e);
        if (num(j, "rotation", &v)) lv_arc_set_rotation(o, v);
        if (num(j, "thickness", &v)) {
            lv_obj_set_style_arc_width(o, v, LV_PART_MAIN);
            lv_obj_set_style_arc_width(o, v, LV_PART_INDICATOR);
        }
        if (flag(j, "knob", &b) && !b) {
            lv_obj_remove_style(o, NULL, LV_PART_KNOB);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        }
        if (has_color) lv_obj_set_style_arc_color(o, c, LV_PART_INDICATOR);
        break;
    }
    case W_BAR:
        if (range) lv_bar_set_range(o, min, max);
        if (num(j, "value", &v)) lv_bar_set_value(o, v, LV_ANIM_ON);
        if (has_color) lv_obj_set_style_bg_color(o, c, LV_PART_INDICATOR);
        break;
    case W_SPINNER:
        if (has_color) lv_obj_set_style_arc_color(o, c, LV_PART_INDICATOR);
        break;
    case W_IMAGE: {
        const char *src = str(j, "src");
        if (src) image_src(w, src);
        if (num(j, "zoom", &v)) lv_image_set_scale(o, (uint32_t)(v * 256 / 100));
        if (num(j, "angle", &v)) lv_image_set_rotation(o, v * 10);
        break;
    }
    case W_CHART: {
        const char *kind = str(j, "kind");
        if (kind) lv_chart_set_type(o, !strcmp(kind, "bar") ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
        if (range) lv_chart_set_axis_range(o, LV_CHART_AXIS_PRIMARY_Y, min, max);
        if (num(j, "points", &v) && v > 1 && v <= 200) lv_chart_set_point_count(o, v);
        const cJSON *series = cJSON_GetObjectItem(j, "series"), *s;
        int i = 0;
        cJSON_ArrayForEach(s, series) {   /* [{"color", "values": [...]}] */
            if (i == 4) break;
            lv_color_t sc = lv_palette_main(LV_PALETTE_PURPLE + i);
            color(s, "color", &sc);
            if (i >= w->nseries) {
                w->series[i] = lv_chart_add_series(o, sc, LV_CHART_AXIS_PRIMARY_Y);
                w->nseries = i + 1;
            }
            const cJSON *vals = cJSON_GetObjectItem(s, "values"), *val;
            if (vals) {
                lv_chart_set_all_values(o, w->series[i], LV_CHART_POINT_NONE);
                cJSON_ArrayForEach(val, vals) lv_chart_set_next_value(o, w->series[i], val->valueint);
            }
            i++;
        }
        const cJSON *push = cJSON_GetObjectItem(j, "push");   /* one new value per series */
        if (cJSON_IsNumber(push) && w->nseries) {
            lv_chart_set_next_value(o, w->series[0], push->valueint);
        } else if (cJSON_IsArray(push)) {
            for (int k = 0; k < w->nseries && k < cJSON_GetArraySize(push); k++) {
                lv_chart_set_next_value(o, w->series[k], cJSON_GetArrayItem(push, k)->valueint);
            }
        }
        lv_chart_refresh(o);
        break;
    }
    case W_LED:
        if (has_color) lv_led_set_color(o, c);
        if (num(j, "brightness", &v)) lv_led_set_brightness(o, (uint8_t)v);
        if (flag(j, "on", &b)) {
            if (b) lv_led_on(o);
            else lv_led_off(o);
        }
        break;
    case W_ROLLER:
    case W_DROPDOWN: {
        char *opts = options_of(cJSON_GetObjectItem(j, "options"));
        if (opts) {
            if (w->type == W_ROLLER) lv_roller_set_options(o, opts, LV_ROLLER_MODE_NORMAL);
            else lv_dropdown_set_options(o, opts);
            free(opts);
        }
        if (num(j, "selected", &v)) {
            if (w->type == W_ROLLER) lv_roller_set_selected(o, v, LV_ANIM_ON);
            else lv_dropdown_set_selected(o, v);
        }
        if (w->type == W_ROLLER && num(j, "rows", &v)) lv_roller_set_visible_row_count(o, v);
        break;
    }
    case W_LINE: {
        const cJSON *pts = cJSON_GetObjectItem(j, "points");
        int n = cJSON_GetArraySize(pts);
        if (n >= 2 && n <= 256) {
            lv_point_precise_t *p = heap_caps_malloc(n * sizeof(*p), MALLOC_CAP_SPIRAM);
            if (p) {
                for (int i = 0; i < n; i++) {
                    const cJSON *pt = cJSON_GetArrayItem(pts, i);
                    p[i].x = cJSON_GetArraySize(pt) > 0 ? cJSON_GetArrayItem(pt, 0)->valueint : 0;
                    p[i].y = cJSON_GetArraySize(pt) > 1 ? cJSON_GetArrayItem(pt, 1)->valueint : 0;
                }
                lv_line_set_points(o, p, n);
                free(w->points);
                w->points = p;
            }
        }
        if (has_color) lv_obj_set_style_line_color(o, c, 0);
        if (num(j, "width", &v)) lv_obj_set_style_line_width(o, v, 0);
        break;
    }
    case W_CANVAS: {
        lv_color_t bg = lv_color_black();
        color(j, "fill", &bg);
        const cJSON *draw = cJSON_GetObjectItem(j, "draw");
        bool clear = cJSON_IsTrue(cJSON_GetObjectItem(j, "clear")) || (draw && !cJSON_GetObjectItem(j, "add"));
        if (draw || clear) canvas_draw(w, draw, clear, bg);
        break;
    }
    case W_QR:
#if LV_USE_QRCODE
        if (text) lv_qrcode_update(o, text, strlen(text));
#endif
        break;
    case W_TABLE: {
        const cJSON *rows = cJSON_GetObjectItem(j, "rows"), *row;
        int r = 0;
        cJSON_ArrayForEach(row, rows) {
            int col = 0;
            const cJSON *cell;
            cJSON_ArrayForEach(cell, row) {
                char buf[24];
                if (cJSON_IsString(cell)) lv_table_set_cell_value(o, r, col, cell->valuestring);
                else if (cJSON_IsNumber(cell)) {
                    snprintf(buf, sizeof(buf), "%g", cell->valuedouble);
                    lv_table_set_cell_value(o, r, col, buf);
                }
                col++;
            }
            r++;
        }
        const cJSON *widths = cJSON_GetObjectItem(j, "col_w"), *cw;
        int col = 0;
        cJSON_ArrayForEach(cw, widths) lv_table_set_column_width(o, col++, cw->valueint);
        break;
    }
    case W_INPUT:
        if (text) lv_textarea_set_text(o, text);
        if (str(j, "placeholder")) lv_textarea_set_placeholder_text(o, str(j, "placeholder"));
        break;
    default:
        break;
    }
}

/* A container's defaults: transparent, and a flex layout for rows and columns. */
static void container(lv_obj_t *o, wtype_t t)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, t == W_ROW ? lv_pct(100) : LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(o, 8, 0);
    lv_obj_set_style_pad_column(o, 8, 0);
    if (t != W_BOX) {
        lv_obj_set_flex_flow(o, t == W_ROW ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
}

static bool build(app_t *a, lv_obj_t *parent, const cJSON *j, int depth, char *err, size_t errlen)
{
    if (depth > DEPTH_MAX) {
        snprintf(err, errlen, "widgets nest more than %d deep", DEPTH_MAX);
        return false;
    }
    if (a->nw == WIDGETS_MAX) {
        snprintf(err, errlen, "more than %d widgets", WIDGETS_MAX);
        return false;
    }
    const char *type = str(j, "type");
    int t = -1;
    for (size_t i = 0; type && i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (!strcmp(type, TYPES[i])) t = (int)i;
    }
    if (t < 0) {
        snprintf(err, errlen, "unknown widget type '%s'", type ? type : "(none)");
        return false;
    }
    widget_t *w = &a->w[a->nw++];
    memset(w, 0, sizeof(*w));
    w->type = (wtype_t)t;
    w->app = a;
    const char *id = str(j, "id");
    if (id) strlcpy(w->id, id, sizeof(w->id));
    lv_obj_t *o = NULL;
    switch (w->type) {
    case W_BOX:
    case W_ROW:
    case W_COLUMN:
        o = lv_obj_create(parent);
        container(o, w->type);
        break;
    case W_LABEL:
        o = lv_label_create(parent);
        lv_label_set_text(o, "");
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
        break;
    case W_BUTTON:
        o = lv_button_create(parent);
        lv_obj_set_style_radius(o, 22, 0);
        w->label = lv_label_create(o);
        lv_label_set_text(w->label, "");
        lv_obj_center(w->label);
        lv_obj_add_event_cb(o, on_click, LV_EVENT_SHORT_CLICKED, w);
        lv_obj_add_event_cb(o, on_click, LV_EVENT_LONG_PRESSED, w);
        break;
    case W_SWITCH:
        o = lv_switch_create(parent);
        lv_obj_add_event_cb(o, on_value, LV_EVENT_VALUE_CHANGED, w);
        break;
    case W_CHECKBOX:
        o = lv_checkbox_create(parent);
        lv_checkbox_set_text(o, "");
        lv_obj_add_event_cb(o, on_value, LV_EVENT_VALUE_CHANGED, w);
        break;
    case W_SLIDER:
        o = lv_slider_create(parent);
        lv_obj_set_width(o, lv_pct(80));
        lv_obj_add_event_cb(o, on_value, LV_EVENT_VALUE_CHANGED, w);
        lv_obj_add_event_cb(o, on_value, LV_EVENT_RELEASED, w);
        break;
    case W_ARC:
        o = lv_arc_create(parent);
        lv_obj_set_size(o, 150, 150);
        lv_obj_add_event_cb(o, on_value, LV_EVENT_VALUE_CHANGED, w);
        lv_obj_add_event_cb(o, on_value, LV_EVENT_RELEASED, w);
        break;
    case W_BAR:
        o = lv_bar_create(parent);
        lv_obj_set_size(o, lv_pct(80), 14);
        break;
    case W_SPINNER:
        o = lv_spinner_create(parent);
        lv_obj_set_size(o, 48, 48);
        break;
    case W_IMAGE:
        o = lv_image_create(parent);
        break;
    case W_CHART:
        o = lv_chart_create(parent);
        lv_obj_set_size(o, lv_pct(80), 110);
        lv_chart_set_point_count(o, 30);
        lv_chart_set_update_mode(o, LV_CHART_UPDATE_MODE_SHIFT);
        break;
    case W_LED:
        o = lv_led_create(parent);
        lv_obj_set_size(o, 24, 24);
        break;
    case W_ROLLER:
        o = lv_roller_create(parent);
        lv_obj_add_event_cb(o, on_value, LV_EVENT_VALUE_CHANGED, w);
        break;
    case W_DROPDOWN:
        o = lv_dropdown_create(parent);
        lv_obj_add_event_cb(o, on_value, LV_EVENT_VALUE_CHANGED, w);
        break;
    case W_LINE:
        o = lv_line_create(parent);
        break;
    case W_CANVAS: {
        int cw = 200, ch = 200;
        num(j, "w", &cw);
        num(j, "h", &ch);
        cw = cw < 8 ? 8 : cw > muse_board->width ? muse_board->width : cw;
        ch = ch < 8 ? 8 : ch > muse_board->height ? muse_board->height : ch;
        o = lv_canvas_create(parent);
        w->canvas = lv_draw_buf_create(cw, ch, LV_COLOR_FORMAT_RGB565, 0);
        if (w->canvas) {
            lv_canvas_set_draw_buf(o, w->canvas);
            lv_canvas_fill_bg(o, lv_color_black(), LV_OPA_COVER);
        }
        break;
    }
    case W_QR:
#if LV_USE_QRCODE
        o = lv_qrcode_create(parent);
        {
            int qs = 150;
            num(j, "size", &qs);
            lv_qrcode_set_size(o, qs);
            lv_color_t fg = lv_color_black(), bg = lv_color_white();
            color(j, "color", &fg);
            color(j, "bg", &bg);
            lv_qrcode_set_dark_color(o, fg);
            lv_qrcode_set_light_color(o, bg);
        }
#else
        o = lv_label_create(parent);
        lv_label_set_text(o, "(no QR support)");
#endif
        break;
    case W_TABLE:
        o = lv_table_create(parent);
        break;
    case W_INPUT:
        o = lv_textarea_create(parent);
        lv_textarea_set_one_line(o, true);
        lv_obj_set_width(o, lv_pct(80));
        lv_obj_add_event_cb(o, on_input, LV_EVENT_CLICKED, w);
        break;
    }
    w->obj = o;
    apply(w, j);
    const cJSON *kids = cJSON_GetObjectItem(j, "children"), *k;
    cJSON_ArrayForEach(k, kids) {
        if (!build(a, o, k, depth + 1, err, errlen)) return false;
    }
    return true;
}

/* ---- Pages ---- */

static void free_app(app_t *a)
{
    /* The shared keyboard lets go of this page's input, and of the widget its
     * callbacks point at, before both are freed. */
    lv_obj_t *typing = s_keyboard ? lv_keyboard_get_textarea(s_keyboard) : NULL;
    for (int i = 0; typing && i < a->nw; i++) {
        if (a->w[i].obj == typing) {
            keyboard_close();
            break;
        }
    }
    if (a->tile) lv_obj_delete(a->tile);   /* its widgets with it */
    for (int i = 0; i < a->nw; i++) {
        free(a->w[i].pixels);
        free(a->w[i].points);
        if (a->w[i].canvas) lv_draw_buf_destroy(a->w[i].canvas);
    }
    free(a->w);
    free(a);
}

static int by_order(const void *x, const void *y)
{
    const app_t *a = *(app_t *const *)x, *b = *(app_t *const *)y;
    if (!a || !b) return !a - !b;
    return a->order != b->order ? a->order - b->order : strcmp(a->id, b->id);
}

/* The apps in order below the face, each able to slide to its neighbours;
 * the face slides down to the first. With the display lock held. */
static void relayout(void)
{
    lv_obj_t *tv = muse_ui_tileview(), *face = muse_ui_face_tile();
    qsort(s_apps, MUSE_APPS_MAX, sizeof(s_apps[0]), by_order);
    int n = 0;
    while (n < MUSE_APPS_MAX && s_apps[n]) n++;
    int32_t col = lv_obj_get_x(face), h = lv_obj_get_height(tv);
    lv_tileview_tile_t *ft = (lv_tileview_tile_t *)face;
    ft->dir = (lv_dir_t)((ft->dir & ~LV_DIR_BOTTOM) | (n ? LV_DIR_BOTTOM : 0));
    for (int i = 0; i < n; i++) {
        lv_obj_set_pos(s_apps[i]->tile, col, (i + 1) * h);
        ((lv_tileview_tile_t *)s_apps[i]->tile)->dir = (lv_dir_t)(LV_DIR_TOP | (i + 1 < n ? LV_DIR_BOTTOM : 0));
    }
    /* Dots down the right edge: the face, then each app. */
    lv_obj_t *scr = lv_screen_active();
    for (int i = 0; i <= MUSE_APPS_MAX; i++) {
        if (!s_dots[i]) {
            s_dots[i] = lv_obj_create(scr);
            lv_obj_remove_style_all(s_dots[i]);
            lv_obj_set_size(s_dots[i], DOT, DOT);
            lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE);
        }
        /* Inside the face's ring (its inner edge is 10 px in, and it spins while
         * Muse thinks), and outside the example apps' rings. */
        lv_obj_align(s_dots[i], LV_ALIGN_RIGHT_MID, -28, (i - n / 2) * (DOT + 6) - (n % 2 ? 0 : (DOT + 6) / 2));
        lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* An app added or removed above the one showing moves it: follow it, so
     * the screen doesn't jump to another app. That also gives the tileview the
     * page's directions, which it otherwise takes only when a slide ends. */
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    if (active && lv_obj_is_valid(active)) {   /* not a page just deleted: its caller sets another */
        lv_obj_update_layout(tv);
        lv_tileview_set_tile(tv, active, LV_ANIM_OFF);
    }
    s_dots_stale = true;
}

void muse_apps_tick(void)
{
    lv_obj_t *tv = muse_ui_tileview();
    if (!tv) return;
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    bool reshow = __atomic_exchange_n(&s_reshow, false, __ATOMIC_RELAXED);
    if (active != s_shown) {
        /* The keyboard goes with the page its input is on, but not from an
         * input tapped on the new page before this tick saw it. */
        lv_obj_t *o = s_keyboard ? lv_keyboard_get_textarea(s_keyboard) : NULL;
        while (o && o != active) o = lv_obj_get_parent(o);
        if (!o) keyboard_close();
        /* show / hide events for the pages that came and went. */
        for (int i = 0; i < MUSE_APPS_MAX && s_apps[i]; i++) {
            if (s_apps[i]->tile == s_shown) muse_hw_ui(s_apps[i]->id, "", "hide", 0, NULL);
            if (s_apps[i]->tile == active) muse_hw_ui(s_apps[i]->id, "", "show", 0, NULL);
        }
        s_shown = active;
        s_dots_stale = true;
    } else if (reshow) {
        for (int i = 0; i < MUSE_APPS_MAX && s_apps[i]; i++) {
            if (s_apps[i]->tile == active) muse_hw_ui(s_apps[i]->id, "", "show", 0, NULL);
        }
    }
    if (!s_dots_stale) return;
    s_dots_stale = false;
    int n = 0, at = -1;
    while (n < MUSE_APPS_MAX && s_apps[n]) {
        if (s_apps[n]->tile == active) at = n + 1;
        n++;
    }
    if (active == muse_ui_face_tile()) at = 0;
    for (int i = 0; i <= MUSE_APPS_MAX; i++) {
        if (!s_dots[i]) continue;
        bool show = n && at >= 0 && i <= n;   /* on the face or an app, with apps to show */
        lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, !show);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == at ? 0xa77dff : 0x3a3358), 0);
    }
}

void muse_apps_screen_off(void)
{
    keyboard_close();
    for (int i = 0; s_shown && i < MUSE_APPS_MAX && s_apps[i]; i++) {
        if (s_apps[i]->tile == s_shown) muse_hw_ui(s_apps[i]->id, "", "hide", 0, NULL);
    }
    s_shown = NULL;   /* the first tick awake says "show" again */
}

void muse_apps_reshow(void)
{
    __atomic_store_n(&s_reshow, true, __ATOMIC_RELAXED);
}

bool muse_apps_typing(void)
{
    return s_keyboard && !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static bool valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 1 || n >= ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static int find_app(const char *id)
{
    for (int i = 0; i < MUSE_APPS_MAX; i++) {
        if (s_apps[i] && !strcmp(s_apps[i]->id, id)) return i;
    }
    return -1;
}

bool muse_apps_define(const cJSON *def, char *err, size_t errlen)
{
    const char *id = str(def, "id");
    if (!valid_id(id)) {
        snprintf(err, errlen, "id is 1-15 of a-z, 0-9, _ or -");
        return false;
    }
    if (!strcmp(id, "face") || !strcmp(id, "pet") || !strcmp(id, "settings")) {
        snprintf(err, errlen, "'%s' is a built-in page", id);
        return false;
    }
    app_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_SPIRAM);
    widget_t *ws = a ? heap_caps_calloc(WIDGETS_MAX, sizeof(widget_t), MALLOC_CAP_SPIRAM) : NULL;
    if (!ws) {
        free(a);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    a->w = ws;
    strlcpy(a->id, id, sizeof(a->id));
    const char *title = str(def, "title");
    strlcpy(a->title, title ? title : id, sizeof(a->title));
    num(def, "order", &a->order);
    if (!lock()) {
        free(ws);
        free(a);
        snprintf(err, errlen, "the screen isn't up");
        return false;
    }
    int slot = find_app(id);
    if (slot < 0) {
        for (slot = 0; slot < MUSE_APPS_MAX && s_apps[slot]; slot++) {
        }
    }
    if (slot == MUSE_APPS_MAX) {
        unlock();
        free(ws);
        free(a);
        snprintf(err, errlen, "at most %d apps", MUSE_APPS_MAX);
        return false;
    }
    lv_obj_t *tv = muse_ui_tileview();
    bool was_shown = s_apps[slot] && lv_tileview_get_tile_active(tv) == s_apps[slot]->tile;
    a->tile = lv_tileview_add_tile(tv, 0, MUSE_APPS_MAX + 1, LV_DIR_TOP);   /* placed by relayout */
    lv_obj_set_scrollbar_mode(a->tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(a->tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_color_t bg = lv_color_black();
    color(def, "bg", &bg);
    lv_obj_set_style_bg_color(a->tile, bg, 0);
    lv_obj_set_style_bg_opa(a->tile, LV_OPA_COVER, 0);
    /* The page's root: a centred column inside the circle, unless "layout": "none". */
    widget_t *root = &a->w[a->nw++];
    root->app = a;
    const char *layout = str(def, "layout");
    bool free_layout = layout && !strcmp(layout, "none");
    root->type = free_layout ? W_BOX : W_COLUMN;
    root->obj = lv_obj_create(a->tile);
    container(root->obj, root->type);
    if (free_layout) {
        lv_obj_set_size(root->obj, lv_pct(100), lv_pct(100));
    } else {
        lv_obj_set_width(root->obj, muse_board->round ? muse_board->width * 3 / 4 : muse_board->width - 16);
        lv_obj_center(root->obj);
        lv_obj_set_style_pad_row(root->obj, 10, 0);
    }
    /* A tap on the page's background: widget "". */
    lv_obj_add_flag(a->tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->tile, on_click, LV_EVENT_SHORT_CLICKED, root);
    apply(root, def);
    const cJSON *kids = cJSON_GetObjectItem(def, "children"), *k;
    bool ok = true;
    cJSON_ArrayForEach(k, kids) {
        if (!(ok = build(a, root->obj, k, 1, err, errlen))) break;
    }
    if (!ok) {
        free_app(a);
        unlock();
        return false;
    }
    if (s_apps[slot]) {
        /* On screen, its new widgets need its script to fill them: "show"
         * again, but the keyboard stays unless its input went with the old page. */
        if (s_shown == s_apps[slot]->tile && !was_shown) {
            muse_hw_ui(s_apps[slot]->id, "", "hide", 0, NULL);   /* slid away before the tick saw it */
        }
        if (s_shown == s_apps[slot]->tile) s_shown = was_shown ? a->tile : NULL;
        if (was_shown) muse_apps_reshow();
        free_app(s_apps[slot]);
    }
    s_apps[slot] = a;
    relayout();
    if (was_shown) {
        lv_obj_update_layout(tv);   /* the new tile's place, not where it was made */
        lv_tileview_set_tile(tv, a->tile, LV_ANIM_OFF);
    }
    unlock();
    ESP_LOGI(TAG, "app %s: %d widgets", id, a->nw);
    return true;
}

bool muse_apps_update(const char *app, const cJSON *set, char *err, size_t errlen)
{
    if (!lock()) {
        snprintf(err, errlen, "the screen isn't up");
        return false;
    }
    int i = app ? find_app(app) : -1;
    if (i < 0) {
        unlock();
        snprintf(err, errlen, "no app '%s'", app ? app : "");
        return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, set) {
        widget_t *w = find_widget(s_apps[i], item->string);
        if (w && cJSON_IsObject(item)) apply(w, item);
    }
    unlock();
    return true;
}

bool muse_apps_remove(const char *app)
{
    if (!lock()) return false;
    int i = find_app(app);
    if (i >= 0) {
        lv_obj_t *tv = muse_ui_tileview();
        if (s_shown == s_apps[i]->tile) {
            muse_hw_ui(s_apps[i]->id, "", "hide", 0, NULL);   /* its script stops what it runs for it */
            s_shown = NULL;
        }
        if (lv_tileview_get_tile_active(tv) == s_apps[i]->tile) {
            lv_tileview_set_tile(tv, muse_ui_face_tile(), LV_ANIM_OFF);
        }
        free_app(s_apps[i]);
        s_apps[i] = NULL;
        relayout();
    }
    unlock();
    return i >= 0;
}

/* The pages that are always there, for app.show and app.list. */
static lv_obj_t *builtin_tile(const char *id)
{
    if (!id || !id[0] || !strcmp(id, "face")) return muse_ui_face_tile();
    if (!strcmp(id, "pet")) return muse_ui_pet_tile();
    if (!strcmp(id, "settings")) return muse_ui_settings_tile();
    return NULL;
}

bool muse_apps_show(const char *app)
{
    if (!lock()) return false;
    lv_obj_t *tile = builtin_tile(app);
    int i = tile ? -1 : find_app(app);
    if (!tile && i < 0) {
        unlock();
        return false;
    }
    if (i >= 0) tile = s_apps[i]->tile;
    muse_state_set_asleep(false);
    muse_ui_set_swipe_enabled(true);
    lv_tileview_set_tile(muse_ui_tileview(), tile, LV_ANIM_ON);
    unlock();
    return true;
}

/* Really on screen: the page it thinks is showing, and scrolled all the way there. */
static bool on_screen(lv_obj_t *tv, lv_obj_t *tile)
{
    return tile && tile == lv_tileview_get_tile_active(tv) && lv_obj_get_scroll_x(tv) == lv_obj_get_x(tile)
           && lv_obj_get_scroll_y(tv) == lv_obj_get_y(tile);
}

cJSON *muse_apps_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!lock()) return arr;
    lv_obj_t *active = lv_tileview_get_tile_active(muse_ui_tileview());
    static const char *const BUILTIN[][2] = { { "face", "Muse" }, { "pet", "Pet" }, { "settings", "Settings" } };
    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        lv_obj_t *tile = builtin_tile(BUILTIN[i][0]);
        if (!tile) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", BUILTIN[i][0]);
        cJSON_AddStringToObject(o, "title", BUILTIN[i][1]);
        cJSON_AddBoolToObject(o, "builtin", true);
        cJSON_AddBoolToObject(o, "shown", tile == active);
        cJSON_AddBoolToObject(o, "on_screen", on_screen(muse_ui_tileview(), tile));
        cJSON_AddItemToArray(arr, o);
    }
    for (int i = 0; i < MUSE_APPS_MAX && s_apps[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s_apps[i]->id);
        cJSON_AddStringToObject(o, "title", s_apps[i]->title);
        cJSON_AddNumberToObject(o, "order", s_apps[i]->order);
        cJSON_AddNumberToObject(o, "widgets", s_apps[i]->nw - 1);
        cJSON_AddBoolToObject(o, "shown", s_apps[i]->tile == active);
        cJSON_AddBoolToObject(o, "on_screen", on_screen(muse_ui_tileview(), s_apps[i]->tile));
        cJSON_AddItemToArray(arr, o);
    }
    unlock();
    return arr;
}

bool muse_apps_set_jpeg(const char *app, const char *widget, const uint8_t *jpeg, size_t len)
{
    /* The lock only for the widget's size and the swap: LVGL (touch, a swipe's
     * speed, the slide) can't run while it's held, and a frame's decode is long. */
    if (!lock()) return false;
    int i = app ? find_app(app) : -1;
    widget_t *w = i >= 0 ? find_widget(s_apps[i], widget) : NULL;
    int bw = 0, bh = 0, ow = 0, oh = 0;
    bool ok = w && w->type == W_IMAGE;
    bool sized = ok && image_box(w, &bw, &bh);
    unlock();
    if (!ok) return false;
    uint16_t *px = decode_jpeg(jpeg, len, bw, bh, sized, &ow, &oh);
    if (!px || !lock()) {
        free(px);
        return false;
    }
    i = find_app(app);   /* again: the page may have been rebuilt or removed meanwhile */
    w = i >= 0 ? find_widget(s_apps[i], widget) : NULL;
    ok = w && w->type == W_IMAGE;
    if (ok) {
        image_set(w, px, ow, oh);
    } else {
        free(px);
    }
    unlock();
    return ok;
}
