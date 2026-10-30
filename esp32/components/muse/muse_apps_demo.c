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
 * Multi-app demo UI driver. The apps (app_manager, the apps directory, and
 * pet_world) draw into one 320x240 RGB565 framebuffer; this binds it to a
 * full-screen LVGL canvas and forwards touch to the app manager. It reads
 * muse_state only (mode, level, caption), so it runs alongside the normal
 * voice/session stack without owning it. muse_ui's frame_tick drives it after
 * its own sleep/brightness/pairing/menu housekeeping.
 */

#include "muse_apps_demo.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "app_manager.h"
#include "apps/pet_world.h"
#include "muse_state.h"

static const char *TAG = "muse_apps_demo";

static lv_obj_t *s_canvas;
static lv_indev_t *s_indev;

/*
 * Pet World (room swipe) and the hardware demo (particle fling) use their own
 * drags. Stop those drags at the canvas so they never chain to the tileview and
 * slide to settings mid-gesture. The launcher and the debugger are tap-only, so
 * they let drags through and swipe-left to settings keeps working there. This
 * is the canvas's own flag, so muse_ui's chrome and show-face code, which turn
 * the tileview's swipe back on, can't undo it.
 */
static void sync_scroll_chain(void)
{
    app_id_t app = app_manager_get_app();
    bool chain = app == APP_LAUNCHER || app == APP_DEBUGGER;
    if (lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_SCROLL_CHAIN) != chain) {
        lv_obj_set_flag(s_canvas, LV_OBJ_FLAG_SCROLL_CHAIN, chain);
    }
}

/* Registered for the four press events only: LVGL also sends draw events to
 * the canvas every frame, which must not count as activity. */
static void on_touch(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        indev = s_indev;
    }
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        app_manager_touch_down(p.x, p.y);
        break;
    case LV_EVENT_PRESSING:
        app_manager_touch_move(p.x, p.y);
        break;
    case LV_EVENT_RELEASED:
        app_manager_touch_up();
        break;
    case LV_EVENT_PRESS_LOST:
        /* The finger left the widget without a clean release: cancel the press
         * rather than committing it as a tap. */
        app_manager_touch_cancel();
        break;
    default:
        return;   /* not a touch */
    }
    muse_state_poke();     /* a touch counts as activity: keep the screen awake */
    sync_scroll_chain();   /* a tap may have switched apps */
}

lv_obj_t *muse_apps_demo_build(lv_obj_t *parent, lv_indev_t *indev)
{
    s_indev = indev;
    app_manager_init();   /* allocates the shared framebuffer in pet_world_init */
    uint16_t *fb = pet_world_get_fb();
    if (!fb) {
        ESP_LOGE(TAG, "no framebuffer; demo UI not started, falling back to avatar");
        return NULL;
    }
    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, fb, WORLD_SCREEN_W, WORLD_SCREEN_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, on_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, on_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, on_touch, LV_EVENT_PRESS_LOST, NULL);
    sync_scroll_chain();
    ESP_LOGI(TAG, "multi-app demo UI up (%dx%d)", WORLD_SCREEN_W, WORLD_SCREEN_H);
    return s_canvas;
}

void muse_apps_demo_tick(void)
{
    if (!s_canvas) {
        return;
    }
    static uint32_t version;
    static char caption[MUSE_CAPTION_MAX];   /* persists: muse_state_caption only
                                                writes it when the caption changes */
    muse_state_caption(caption, sizeof(caption), &version);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    app_manager_update(now_ms, (int)muse_state_mode(NULL), muse_state_level(), caption);
    app_manager_render(pet_world_get_fb(), now_ms);
    lv_obj_invalidate(s_canvas);
}
