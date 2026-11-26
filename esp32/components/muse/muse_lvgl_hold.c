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

#include "muse_lvgl_hold.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * esp_lvgl_adapter is pulled in only for the boards that run LVGL through it
 * (components/muse/idf_component.yml); the others (their BSP's esp_lvgl_port,
 * or LVGL on a task of their own: the ESP32-S3-BOX-3, CoreS3, Core2, C6 2.06,
 * JC3248W535, S3 LCD 7) have no adapter to pause, and nothing calls this on
 * them (no video, and their screen-off paths stop LVGL their own way). There
 * the holders are counted all the same and nothing is paused: a board that
 * starts calling it without the adapter must bring its own pause.
 */
#if __has_include("esp_lv_adapter.h")
#include "esp_lv_adapter.h"
#define MUSE_LVGL_ADAPTER 1
#else
#define MUSE_LVGL_ADAPTER 0
#endif

__attribute__((unused)) static const char *HOLD_TAG = "muse_ui";   /* only with the adapter */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_holders;
static bool s_stopped;              /* the adapter acknowledged our pause */
static unsigned s_gave_up;          /* pauses given up (both tries timed out) */

/*
 * One bounded pause: MUSE_LVGL_PAUSE_MS for the worker's acknowledgement,
 * once more on a timeout, then give up. A timed-out pause leaves the adapter
 * running, its worker woken (cmake/esp_lvgl_adapter_fix.cmake), so giving up
 * means LVGL keeps drawing: the holder's own redraw (video's at its end, the
 * screen's on wake) repaints whatever the two drew over each other, and the
 * next hold or release tries again. Never a wait without an end: a lost
 * acknowledgement once hung the paused path until the task watchdog reset
 * the chip.
 */
static bool pause_bounded(void)
{
#if MUSE_LVGL_ADAPTER
    for (int attempt = 1; attempt <= 2; attempt++) {
        esp_err_t err = esp_lv_adapter_pause(MUSE_LVGL_PAUSE_MS);
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW(HOLD_TAG, "LVGL paused on the second try");
            }
            return true;
        }
        ESP_LOGW(HOLD_TAG, "LVGL's worker didn't acknowledge the pause in %d ms (try %d, err %d)",
                 MUSE_LVGL_PAUSE_MS, attempt, (int)err);
    }
    s_gave_up++;
    ESP_LOGE(HOLD_TAG, "gave up pausing LVGL (%u so far): it keeps running, the holder redraws", s_gave_up);
    return false;
#else
    return false;   /* no adapter: nothing to pause on this board (above) */
#endif
}

bool muse_lvgl_hold(muse_lvgl_holder_t who, bool hold)
{
    /* Created once, whichever of its tasks (the input task's screen-off,
     * video's show task) comes first. */
    portENTER_CRITICAL(&s_lock_mux);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    portEXIT_CRITICAL(&s_lock_mux);
    /* Bounded by what it guards: two pauses of MUSE_LVGL_PAUSE_MS at most,
     * or a resume (which doesn't wait). */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_holders = hold ? s_holders | (unsigned)who : s_holders & ~(unsigned)who;
    if (s_holders && !s_stopped) {
        s_stopped = pause_bounded();   /* also retries one given up earlier */
    } else if (!s_holders && s_stopped) {
#if MUSE_LVGL_ADAPTER
        esp_lv_adapter_resume();
#endif
        s_stopped = false;
    }
    bool held = s_holders != 0;
    xSemaphoreGive(s_lock);
    return held;
}

bool muse_lvgl_hold_stopped(void)
{
    return s_stopped;
}

unsigned muse_lvgl_hold_gave_up(void)
{
    return s_gave_up;
}
