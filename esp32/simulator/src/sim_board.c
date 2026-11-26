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

#include "sim_board.h"
#include "sim_platform.h"

#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include <stdlib.h>
#include <string.h>

#if CONFIG_MUSE_LATEST_REPLY
#define WATCHER_RESOLUTION 466
#else
#define WATCHER_RESOLUTION 412
#endif

static lv_display_t *s_display;

/* muse_ui.c reads the selected board through this production global. */
const muse_board_t *muse_board;

static esp_err_t sim_init(void)
{
    return ESP_OK;
}

static lv_display_t *sim_display_start(lv_indev_t **touch)
{
    s_display = lv_sdl_window_create(muse_board->width, muse_board->height);
    if (!s_display) {
        return NULL;
    }
    /* The SDL driver installs SDL_GetTicks. Use the simulator clock instead so
     * scripted runs can advance time without sleeping and render repeatably. */
    lv_tick_set_cb(sim_time_tick_ms);
    lv_sdl_window_set_title(s_display, "Muse Gadget Simulator");
    lv_sdl_window_set_resizeable(s_display, false);
    if (touch) {
        *touch = lv_sdl_mouse_create();
    }
    return s_display;
}

static bool sim_display_lock(int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

static void sim_display_unlock(void)
{
}

static void sim_set_brightness(int pct)
{
    (void)pct;
}

static void sim_panel_sleep(bool sleep)
{
    (void)sleep;
}

/* Never called: the preview plays nothing. Its presence says "has a speaker". */
static esp_err_t sim_audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    (void)spk;
    (void)mic;
    return ESP_FAIL;
}

static esp_err_t sim_power_off(void)
{
    return ESP_FAIL;
}

static const muse_board_t s_sim_board = {
    .name = "SenseCAP Watcher Simulator",
    .width = WATCHER_RESOLUTION,
    .height = WATCHER_RESOLUTION,
    .round = true,
    .touch = true,
    .diagonal_in = 1.45f,
    .panel_w_mm = 36.8,
    .talk_button = "wheel",
    .aux_button = "scroll",
    .talk_hint = { LV_ALIGN_CENTER, 100, -143 },
    .frame_ms = 40,
    .init = sim_init,
    .display_start = sim_display_start,
    .display_lock = sim_display_lock,
    .display_unlock = sim_display_unlock,
    .set_brightness = sim_set_brightness,
    .panel_sleep = sim_panel_sleep,
    .power_off = sim_power_off,
    .audio_init = sim_audio_init,
    .charged = MUSE_CHARGED_DONE,   /* every simulated board's charger reports done: Display shows Sleep when charged */
};

/*
 * MUSE_SIM_BOARD picks the panel: watcher (default, 412 round), stopwatch
 * (466 round, rim keys), w175 (the Waveshare S3 1.75C: 466 round, side
 * keys), mosaico (480 square, 70 px corners) or c6 (368x448
 * rectangle), so the UI screens render at each board's size.
 */
static muse_board_t s_board;

const muse_board_t *sim_board_get(void)
{
    if (s_board.name) {
        return &s_board;
    }
    s_board = s_sim_board;
    const char *which = getenv("MUSE_SIM_BOARD");
    if (which && !strcmp(which, "stopwatch")) {
        s_board.name = "M5Stack StopWatch Simulator";
        s_board.width = s_board.height = 466;
        s_board.diagonal_in = 1.75f;
        s_board.panel_w_mm = 43.8;
        s_board.rim_controls = true;
        s_board.power_button = "red bottom-left";
        s_board.talk_button = "yellow top-left";
        s_board.speaker_button = "blue top-right";
        s_board.aux_button = NULL;
        s_board.talk_hint = (muse_button_hint_t){ LV_ALIGN_CENTER, -144, -110 };
        s_board.speaker_hint = (muse_button_hint_t){ LV_ALIGN_CENTER, 144, -110 };
        s_board.power_hint = (muse_button_hint_t){ LV_ALIGN_CENTER, -134, 134 };
    } else if (which && !strcmp(which, "w175")) {
        s_board.name = "Waveshare S3 1.75C Simulator";
        s_board.width = s_board.height = 466;
        s_board.diagonal_in = 1.75f;
        s_board.panel_w_mm = 0;   /* as the board: no measured width */
        s_board.talk_button = "top";
        s_board.aux_button = "bottom";
        s_board.talk_hint = (muse_button_hint_t){ LV_ALIGN_CENTER, 153, -129 };
        s_board.aux_hint = (muse_button_hint_t){ LV_ALIGN_CENTER, 153, 129 };
    } else if (which && !strcmp(which, "mosaico")) {
        s_board.name = "ESP-Mosaico Simulator";
        s_board.width = s_board.height = 480;
        s_board.round = false;
        s_board.corner_px = 70;
        s_board.no_ring = true;
        s_board.diagonal_in = 2.16f;
        s_board.panel_w_mm = 38.8;
        s_board.talk_button = "ai";
        s_board.aux_button = "boot";
        s_board.talk_hint = (muse_button_hint_t){ LV_ALIGN_TOP_RIGHT, -20, 6 };
    } else if (which && !strcmp(which, "c6")) {
        s_board.name = "Waveshare C6 1.8 Simulator";
        s_board.width = 368;
        s_board.height = 448;
        s_board.round = false;
        s_board.corner_px = 48;   /* as the board's (spec.json wc6 radius 48) */
        s_board.diagonal_in = 1.8f;
        s_board.panel_w_mm = 29.0;
        s_board.talk_button = "boot";
        s_board.aux_button = "pwr";
        s_board.talk_hint = (muse_button_hint_t){ LV_ALIGN_TOP_RIGHT, -12, 40 };
    }
    return &s_board;
}

/* The boards' Muse builds: the StopWatch, Mosaico and Watcher have Wi-Fi
 * builds; the C6's (c6ble) is BLE-only (the home's top row). */
bool muse_home_wifi_build(void)
{
    const char *which = getenv("MUSE_SIM_BOARD");
    return !(which && !strcmp(which, "c6"));
}

/* The firmware's board lookup (for code that asks before muse_board is set). */
const muse_board_t *muse_board_get(void)
{
    return sim_board_get();
}

lv_display_t *sim_board_display(void)
{
    return s_display;
}
