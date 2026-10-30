# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compile the Core2 multi-app demo's canvas driver against LVGL fakes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

LVGL = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    LV_EVENT_ALL = 0,
    LV_EVENT_PRESSED,
    LV_EVENT_PRESSING,
    LV_EVENT_PRESS_LOST,
    LV_EVENT_RELEASED,
    LV_EVENT_DRAW_MAIN_BEGIN,
    LV_EVENT_DRAW_MAIN,
    LV_EVENT_DRAW_POST_END,
    LV_EVENT_REFR_EXT_DRAW_SIZE,
} lv_event_code_t;
#define LV_OBJ_FLAG_CLICKABLE        (1u << 1)
#define LV_OBJ_FLAG_SCROLLABLE       (1u << 4)
#define LV_OBJ_FLAG_SCROLL_CHAIN_HOR (1u << 8)
#define LV_OBJ_FLAG_SCROLL_CHAIN_VER (1u << 9)
#define LV_OBJ_FLAG_SCROLL_CHAIN (LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER)
#define LV_COLOR_FORMAT_RGB565 0x12
typedef struct { int32_t x, y; } lv_point_t;
typedef struct { int unused; } lv_indev_t;
typedef struct { uint32_t flags; } lv_obj_t;
typedef struct { lv_event_code_t code; } lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *e);
lv_indev_t *lv_indev_active(void);
void lv_indev_get_point(const lv_indev_t *indev, lv_point_t *p);
lv_event_code_t lv_event_get_code(lv_event_t *e);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int32_t w, int32_t h, int cf);
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y);
void lv_obj_add_flag(lv_obj_t *obj, uint32_t f);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t f);
void lv_obj_set_flag(lv_obj_t *obj, uint32_t f, bool v);
bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t f);
void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t filter, void *user);
void lv_obj_invalidate(lv_obj_t *obj);
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "muse_apps_demo.h"
#include "app_manager.h"
#include "muse_state.h"

/* LVGL: one canvas, a registry of (filter, callback), and a dispatcher that
 * delivers like lv_event.c: a callback whose filter is LV_EVENT_ALL gets
 * every event, others only their own code. */
static lv_obj_t canvas = { LV_OBJ_FLAG_SCROLL_CHAIN };  /* children chain by default */
static lv_indev_t indev;
static lv_point_t point;
static struct { lv_event_cb_t cb; lv_event_code_t filter; } cbs[16];
static int ncbs;
lv_indev_t *lv_indev_active(void) { return &indev; }
void lv_indev_get_point(const lv_indev_t *i, lv_point_t *p) { (void)i; *p = point; }
lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }
lv_obj_t *lv_canvas_create(lv_obj_t *parent) { (void)parent; return &canvas; }
void lv_canvas_set_buffer(lv_obj_t *o, void *b, int32_t w, int32_t h, int cf) { (void)o; (void)b; (void)w; (void)h; (void)cf; }
void lv_obj_set_pos(lv_obj_t *o, int32_t x, int32_t y) { (void)o; (void)x; (void)y; }
void lv_obj_add_flag(lv_obj_t *o, uint32_t f) { o->flags |= f; }
void lv_obj_remove_flag(lv_obj_t *o, uint32_t f) { o->flags &= ~f; }
void lv_obj_set_flag(lv_obj_t *o, uint32_t f, bool v) { if (v) o->flags |= f; else o->flags &= ~f; }
bool lv_obj_has_flag(const lv_obj_t *o, uint32_t f) { return (o->flags & f) == f; }
void lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t filter, void *u)
{
    (void)o; (void)u;
    assert(ncbs < 16);
    cbs[ncbs].cb = cb;
    cbs[ncbs].filter = filter;
    ncbs++;
}
static int invalidations;
void lv_obj_invalidate(lv_obj_t *o) { (void)o; invalidations++; }
static void send(lv_event_code_t code)
{
    lv_event_t e = { code };
    for (int i = 0; i < ncbs; i++) {
        if (cbs[i].filter == LV_EVENT_ALL || cbs[i].filter == code) cbs[i].cb(&e);
    }
}

/* muse_state */
static int pokes;
void muse_state_poke(void) { pokes++; }
muse_mode_t muse_state_mode(float *t) { if (t) *t = 0; return (muse_mode_t)0; }
float muse_state_level(void) { return 0; }
bool muse_state_caption(char *out, size_t len, uint32_t *v) { (void)v; if (len) out[0] = 0; return false; }

/* pet_world: only the framebuffer */
static uint16_t fb[320 * 240];
uint16_t *pet_world_get_fb(void) { return fb; }

/* app_manager: a launcher whose tap opens whatever `next_app` names */
static app_id_t app = APP_LAUNCHER, next_app = APP_LAUNCHER;
static int downs, moves, ups, cancels, renders;
void app_manager_init(void) { app = APP_LAUNCHER; }
void app_manager_set_app(app_id_t a) { app = a; }
app_id_t app_manager_get_app(void) { return app; }
void app_manager_open_launcher(void) { app = APP_LAUNCHER; }
void app_manager_update(uint32_t now, int mode, float level, const char *caption) { (void)now; (void)mode; (void)level; (void)caption; }
void app_manager_render(uint16_t *f, uint32_t now) { (void)f; (void)now; renders++; }
void app_manager_touch_down(int x, int y) { (void)x; (void)y; downs++; }
void app_manager_touch_move(int x, int y) { (void)x; (void)y; moves++; }
void app_manager_touch_up(void) { ups++; app = next_app; }
void app_manager_touch_cancel(void) { cancels++; }

static void tap(void) { send(LV_EVENT_PRESSED); send(LV_EVENT_RELEASED); }
static bool chains(void) { return lv_obj_has_flag(&canvas, LV_OBJ_FLAG_SCROLL_CHAIN); }

int main(void)
{
    assert(muse_apps_demo_build(NULL, &indev) == &canvas);
    assert(!(canvas.flags & LV_OBJ_FLAG_SCROLLABLE));
    assert(canvas.flags & LV_OBJ_FLAG_CLICKABLE);

    /* Frames: the canvas is redrawn every tick, and LVGL sends its draw
     * events to the canvas's callbacks. None of that is a touch. */
    for (int i = 0; i < 50; i++) {
        muse_apps_demo_tick();
        send(LV_EVENT_REFR_EXT_DRAW_SIZE);
        send(LV_EVENT_DRAW_MAIN_BEGIN);
        send(LV_EVENT_DRAW_MAIN);
        send(LV_EVENT_DRAW_POST_END);
    }
    assert(renders == 50 && invalidations == 50);
    printf("idle frames: pokes=%d downs=%d ups=%d cancels=%d\n", pokes, downs, ups, cancels);
    assert(pokes == 0 && downs == 0 && moves == 0 && ups == 0 && cancels == 0);

    /* A real touch is activity and reaches the app once per event. */
    point.x = 50; point.y = 100;
    send(LV_EVENT_PRESSED);
    send(LV_EVENT_PRESSING);
    send(LV_EVENT_PRESS_LOST);
    assert(downs == 1 && moves == 1 && cancels == 1 && pokes == 3);

    /* The launcher is tap-only: its drags chain to the tileview (settings). */
    printf("launcher chain=%d\n", chains());
    assert(chains());

    /* Pet World and the hardware demo keep their drags on the canvas, and no
     * later frame turns chaining back on. */
    app_id_t drag_apps[] = { APP_PET, APP_DEMO };
    for (int i = 0; i < 2; i++) {
        next_app = drag_apps[i];
        tap();
        assert(app == drag_apps[i]);
        for (int f = 0; f < 10; f++) muse_apps_demo_tick();
        printf("app %d chain=%d\n", (int)app, chains());
        assert(!chains());
        assert(!(canvas.flags & LV_OBJ_FLAG_SCROLL_CHAIN_HOR));
        /* Back to the launcher (its [< APPS] tap): settings swipe returns. */
        next_app = APP_LAUNCHER;
        tap();
        assert(chains());
    }

    /* The debugger is tap-only like the launcher. */
    next_app = APP_DEBUGGER;
    tap();
    assert(app == APP_DEBUGGER && chains());

    puts("ok");
    return 0;
}
'''


class Core2AppsDemo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / "lvgl.h").write_text(LVGL)
        (tmp / "esp_log.h").write_text(
            "#pragma once\n#define ESP_LOGE(tag, ...) ((void)(tag))\n#define ESP_LOGI(tag, ...) ((void)(tag))\n")
        (tmp / "esp_timer.h").write_text(
            "#pragma once\n#include <stdint.h>\nstatic inline int64_t esp_timer_get_time(void) { return 0; }\n")
        (tmp / "harness.c").write_text(HARNESS)
        cls.binary = tmp / "harness"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(
            compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), "-I", str(MUSE),
                        str(tmp / "harness.c"), str(MUSE / "muse_apps_demo.c"), "-o", str(cls.binary)],
            check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_draw_events_are_not_activity_and_drag_apps_keep_their_drags(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
