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

#include "muse_local_press.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/* Only a BLE gadget's firmware update asks for the press (mg_glue.c, over
 * MCUmgr SMP): another build never prompts, so it links the stubs at the end
 * and not the core. */
#if CONFIG_MUSE_GADGET_BLE_AUDIO

static const char *TAG = "link.press";

/* The core's two fields: a press (an input task), an ask (the SMP worker), a link (the host task), a read (the UI). */
static muse_local_press_t s_w = { .prompt_ms = -1, .shown_ms = -1, .bound_ms = -1, .covered_ms = -1, .armed_ms = -1 };
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool (*s_locked)(void);
static void (*s_wake)(bool locked);
static bool (*s_shown)(void);   /* the Muse UI's card on show; NULL: no Muse UI (every prompted press counts) */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool locked(void)
{
    bool (*fn)(void) = s_locked;
    return fn && fn();
}

void muse_local_press_set_shown(bool (*shown)(void))
{
    s_shown = shown;
}

void muse_local_press_set_ui(bool (*locked_fn)(void), void (*wake)(bool locked))
{
    s_locked = locked_fn;
    s_wake = wake;
}

muse_update_answer_t muse_local_press_ask(const muse_update_req_t *req)
{
    int64_t now = now_ms();
    bool lock = locked();
    portENTER_CRITICAL(&s_mux);
    bool was = muse_local_press_core_prompting(&s_w, now);
    muse_update_answer_t a = muse_local_press_core_ask(&s_w, now, lock, req);
    bool on = muse_local_press_core_prompting(&s_w, now);
    portEXIT_CRITICAL(&s_mux);
    if (a == MUSE_UPDATE_ASKING && !was && on) {   /* a prompt that just started (not one the re-arm limit holds) */
        void (*wake)(bool) = s_wake;
        if (wake) {
            wake(lock);   /* the screen up: the confirm card, or the keypad first */
        }
        ESP_LOGW(TAG, "an update over Bluetooth asks for one press on the gadget%s: \"Press to confirm update\" for %d s",
                 lock ? ", after the passcode" : "", MUSE_UPDATE_PROMPT_MS / 1000);
    } else if (a == MUSE_UPDATE_REFUSED) {
        ESP_LOGW(TAG, "an update change refused (rc 11): no image to bind a press to, the prompt ended with no "
                      "press, or not the bound image");
    }
    return a;
}

bool muse_local_press_bound(muse_update_ident_t *id)
{
    portENTER_CRITICAL(&s_mux);
    bool b = muse_local_press_core_bound(&s_w, id);
    portEXIT_CRITICAL(&s_mux);
    return b;
}

void muse_local_press_forget(void)
{
    portENTER_CRITICAL(&s_mux);
    muse_local_press_core_forget(&s_w, now_ms());
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Forget: the update's binding and confirmations end");
}

bool muse_local_press_note(const char *what)
{
    bool (*shown)(void) = s_shown;
    if (shown && !shown()) {
        return false;   /* the prompt isn't on show: the press is the view's, not the update's */
    }
    bool lock = locked();
    portENTER_CRITICAL(&s_mux);
    bool ok = muse_local_press_core_note(&s_w, now_ms(), lock);
    portEXIT_CRITICAL(&s_mux);
    if (ok) {
        ESP_LOGI(TAG, "the update is confirmed (%s): bound to its upload for %d s", what, MUSE_UPDATE_BOUND_MS / 1000);
    }
    return ok;
}

void muse_local_press_cancel(const char *what)
{
    portENTER_CRITICAL(&s_mux);
    bool was = muse_local_press_core_prompting(&s_w, now_ms());
    muse_local_press_core_cancel(&s_w, now_ms());
    portEXIT_CRITICAL(&s_mux);
    if (was) {
        ESP_LOGI(TAG, "the update is cancelled on the gadget (%s)", what);
    }
}

void muse_local_press_link_new(void)
{
    portENTER_CRITICAL(&s_mux);
    bool had = s_w.prompt_ms >= 0 || s_w.cancelled;
    muse_local_press_core_link_new(&s_w);
    portEXIT_CRITICAL(&s_mux);
    if (had) {
        ESP_LOGI(TAG, "a new link: the update prompt ends");
    }
}

bool muse_local_press_prompting(void)
{
    if (locked()) {
        return false;   /* the keypad first (the passcode lock's own screen) */
    }
    portENTER_CRITICAL(&s_mux);
    bool on = muse_local_press_core_prompting(&s_w, now_ms());
    portEXIT_CRITICAL(&s_mux);
    return on;
}

bool muse_local_press_console(const char *line)
{
    if (strcmp(line, "update.arm") != 0) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    muse_local_press_core_arm(&s_w, now_ms());
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "the wired console's update.arm: this link may update for %d s, no press", MUSE_LOCAL_PRESS_MS / 1000);
    printf("@update.arm open %d s\n", MUSE_LOCAL_PRESS_MS / 1000);
    fflush(stdout);
    return true;
}

#endif   /* without CONFIG_MUSE_GADGET_BLE_AUDIO: muse_local_press.h's inline stubs */
