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

APPS_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "muse_apps_demo.h"
#include "app_manager.h"
#include "muse_chat.h"
#include "muse_state.h"
#include "muse_wifi.h"
#include "esp_chip_info.h"
/* Pet World's state is file-static: build it into this file to read it. */
#include "apps/pet_world.c"

static lv_obj_t canvas = { LV_OBJ_FLAG_SCROLL_CHAIN };
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
void lv_obj_invalidate(lv_obj_t *o) { (void)o; }
static void send(lv_event_code_t code)
{
    lv_event_t e = { code };
    for (int i = 0; i < ncbs; i++) {
        if (cbs[i].filter == LV_EVENT_ALL || cbs[i].filter == code) cbs[i].cb(&e);
    }
}

int64_t fake_now_us = 1000000;
static muse_hatch_state_t link_state = MUSE_HATCH_UNREACHABLE;
static int hatch_tests, chirps;
void muse_state_poke(void) {}
muse_mode_t muse_state_mode(float *t) { if (t) *t = 0; return MUSE_MODE_IDLE; }
float muse_state_level(void) { return 0; }
bool muse_state_caption(char *out, size_t len, uint32_t *v) { (void)v; if (len) out[0] = 0; return false; }
muse_power_t muse_state_power(void) { muse_power_t p; memset(&p, 0, sizeof(p)); p.battery_pct = -1; return p; }
void muse_wifi_status(muse_wifi_status_t *out) { memset(out, 0, sizeof(*out)); out->state = MUSE_WIFI_CONNECTED; }
bool muse_wifi_connected(void) { return true; }
void muse_hatch_status(muse_hatch_status_t *out) { memset(out, 0, sizeof(*out)); out->state = link_state; }
const char *muse_hatch_state_name(muse_hatch_state_t s) { (void)s; return "state"; }
void muse_hatch_test(void) { hatch_tests++; }
void muse_voice_request_chirp(void) { chirps++; }
int muse_settings_mic_gain(void) { return 0; }
void esp_chip_info(esp_chip_info_t *c) { memset(c, 0, sizeof(*c)); c->cores = 2; }
size_t esp_psram_get_size(void) { return 8u << 20; }

static void frame(void) { fake_now_us += 40000; muse_apps_demo_tick(); }
static void press(int x, int y) { point.x = x; point.y = y; send(LV_EVENT_PRESSED); }
static void drag(int x, int y) { point.x = x; point.y = y; send(LV_EVENT_PRESSING); }
static void release(void) { send(LV_EVENT_RELEASED); }
static void tap(int x, int y) { press(x, y); release(); frame(); }
static bool chains(void) { return lv_obj_has_flag(&canvas, LV_OBJ_FLAG_SCROLL_CHAIN); }

int main(void)
{
    assert(muse_apps_demo_build(NULL, &indev) == &canvas);
    frame();
    assert(app_manager_get_app() == APP_LAUNCHER && chains());

    /* Launcher: sliding off a card and releasing opens nothing. */
    press(160, 100); drag(160, 120); drag(160, 230); release(); frame();
    assert(app_manager_get_app() == APP_LAUNCHER);

    /* Debugger: tap-only, so it lets swipes through to settings. */
    tap(160, 100);
    assert(app_manager_get_app() == APP_DEBUGGER && chains());

    /* Its buttons fire on release over them, never after sliding off. */
    press(230, 145); drag(230, 190); release(); frame();
    press(80, 145); drag(80, 200); release(); frame();
    printf("slid off: tests=%d chirps=%d\n", hatch_tests, chirps);
    assert(hatch_tests == 0 && chirps == 0);
    press(230, 145); drag(232, 146); release(); frame();
    tap(80, 145);
    assert(hatch_tests == 1 && chirps == 1);

    /* Back to the launcher only on a release over < APPS. */
    press(30, 14); drag(30, 60); release(); frame();
    assert(app_manager_get_app() == APP_DEBUGGER);
    tap(30, 14);
    assert(app_manager_get_app() == APP_LAUNCHER);

    /* Hardware demo keeps its drags; its back button cancels when slid off. */
    tap(260, 100);
    assert(app_manager_get_app() == APP_DEMO && !chains());
    press(30, 14); drag(120, 120); release(); frame();
    assert(app_manager_get_app() == APP_DEMO && !chains());
    tap(30, 14);
    assert(app_manager_get_app() == APP_LAUNCHER && chains());

    /* Pet World keeps its drags across frames. */
    tap(50, 100);
    assert(app_manager_get_app() == APP_PET);
    for (int i = 0; i < 20; i++) frame();
    assert(!chains());

    /* The status dots follow the real link, not a hard-coded "connected". */
    for (int i = 0; i < 15; i++) frame();
    printf("link down: muse_connected=%d battery=%d\n", s_world.muse_connected, s_world.battery_pct);
    assert(!s_world.muse_connected && s_world.battery_pct == -1);
    link_state = MUSE_HATCH_REACHABLE;
    for (int i = 0; i < 15; i++) frame();
    assert(s_world.muse_connected);

    /* A pat shows the petted animation for a moment, then the pet moves on. */
    s_world.state = PET_ST_IDLE;
    s_world.goal = PET_GOAL_NONE;
    s_world.next_goal_eval = (uint32_t)(fake_now_us / 1000) + 100000;
    int px = (int)(s_world.pet_x - s_world.cam_x), py = (int)s_world.pet_y;
    press(px, py); release();
    assert(s_world.state == PET_ST_PETTED);
    for (int i = 0; i < 10; i++) frame();
    printf("petted after 10 frames: %d\n", s_world.state == PET_ST_PETTED);
    assert(s_world.state == PET_ST_PETTED);
    for (int i = 0; i < 40; i++) frame();
    assert(s_world.state != PET_ST_PETTED);

    /* Needs drift over time, so the hunger and bedtime goals can happen. */
    int hunger = s_world.hunger, energy = s_world.energy;
    for (int i = 0; i < 5; i++) { fake_now_us += 21000000; frame(); }
    printf("needs: hunger %d->%d energy %d->%d\n", hunger, s_world.hunger, energy, s_world.energy);
    assert(s_world.hunger > hunger && s_world.energy < energy);

    /* Pet World's menu icon goes back to the launcher. */
    tap(10, 10);
    assert(app_manager_get_app() == APP_LAUNCHER && chains());

    puts("ok");
    return 0;
}
'''

FAKE_HEADERS = {
    "esp_log.h": "#pragma once\n#define ESP_LOGE(tag, ...) ((void)(tag))\n#define ESP_LOGI(tag, ...) ((void)(tag))\n",
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n",
    "freertos/FreeRTOS.h": "#pragma once\n",
    "freertos/queue.h": "#pragma once\ntypedef void *QueueHandle_t;\n",
    "esp_heap_caps.h": "#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
                       "static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }\n",
    "sdkconfig.h": "#pragma once\n#define CONFIG_ESPTOOLPY_FLASHSIZE \"16MB\"\n",
    "esp_chip_info.h": "#pragma once\nenum { CHIP_ESP32 = 1, CHIP_ESP32S3 = 9, CHIP_ESP32C6 = 13 };\ntypedef struct { int model; int features; int revision; int cores; } esp_chip_info_t;\n"
                       "void esp_chip_info(esp_chip_info_t *c);\n",
    "esp_psram.h": "#pragma once\n#include <stddef.h>\nsize_t esp_psram_get_size(void);\n",
}


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


class Core2Apps(unittest.TestCase):
    """The real apps behind the real app manager and canvas driver."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / "freertos").mkdir()
        (tmp / "lvgl.h").write_text(LVGL)
        for name, text in FAKE_HEADERS.items():
            (tmp / name).write_text(text)
        (tmp / "esp_timer.h").write_text(
            "#pragma once\n#include <stdint.h>\nextern int64_t fake_now_us;\n"
            "static inline int64_t esp_timer_get_time(void) { return fake_now_us; }\n")
        (tmp / "harness.c").write_text(APPS_HARNESS)
        cls.binary = tmp / "harness"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        sources = [MUSE / "muse_apps_demo.c", MUSE / "app_manager.c", MUSE / "apps/app_launcher.c",
                   MUSE / "apps/app_debugger.c", MUSE / "apps/app_demo.c"]
        subprocess.run(
            compiler + ["-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable", "-include", str(ROOT / "tests/host_compat.h"), "-I", str(tmp), "-I", str(MUSE), "-I", str(MUSE / "apps"),
                        str(tmp / "harness.c")] + [str(s) for s in sources] + ["-lm", "-o", str(cls.binary)],
            check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_taps_swipes_indicators_and_pet_state(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
