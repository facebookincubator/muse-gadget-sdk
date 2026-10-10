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
 * avatar/pet_world) draw into one 320x240 RGB565 framebuffer; this binds it to a
 * full-screen LVGL canvas, forwards touch to the app manager, and re-renders
 * on the board's frame timer. It reads muse_state only (mode, level, caption),
 * so it runs alongside the normal voice/session stack without owning it.
 */

#include "muse_apps_demo.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "app_manager.h"
#include "avatar/pet_world.h"
#include "muse_state.h"

static const char *TAG = "muse_apps_demo";

static lv_obj_t *s_canvas;
static lv_indev_t *s_indev;

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
    muse_state_poke();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        app_manager_touch_down(p.x, p.y);
        break;
    case LV_EVENT_PRESSING:
        app_manager_touch_move(p.x, p.y);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        app_manager_touch_up();
        break;
    default:
        break;
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    static uint32_t version;
    char caption[MUSE_CAPTION_MAX];
    muse_state_caption(caption, sizeof(caption), &version);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    app_manager_update(now_ms, (int)muse_state_mode(NULL), muse_state_level(), caption);
    app_manager_render(pet_world_get_fb(), now_ms);
    lv_obj_invalidate(s_canvas);
}

void muse_apps_demo_start(lv_indev_t *indev, int frame_ms)
{
    s_indev = indev;
    app_manager_init();   /* allocates the shared framebuffer in pet_world_init */
    uint16_t *fb = pet_world_get_fb();
    if (!fb) {
        ESP_LOGE(TAG, "no framebuffer; demo UI not started");
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, fb, WORLD_SCREEN_W, WORLD_SCREEN_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_touch, LV_EVENT_ALL, NULL);
    lv_timer_create(tick, frame_ms, NULL);
    ESP_LOGI(TAG, "multi-app demo UI up (%dx%d)", WORLD_SCREEN_W, WORLD_SCREEN_H);
}
