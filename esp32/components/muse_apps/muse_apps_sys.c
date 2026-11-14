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

/* The API's sys_* and muse_* groups. */

#include <string.h>
#include <sys/time.h>

#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "muse_app_view.h"
#include "muse_apps_priv.h"
#include "muse_board.h"
#include "muse_link.h"
#include "muse_state.h"
#include "muse_ui.h"

int muse_apps_muse_mode(void)
{
    switch (muse_state_mode(NULL)) {
    case MUSE_MODE_LISTENING: return GADGET_MUSE_LISTENING;
    case MUSE_MODE_THINKING: return GADGET_MUSE_THINKING;
    case MUSE_MODE_SPEAKING: return GADGET_MUSE_SPEAKING;
    case MUSE_MODE_ERROR: return GADGET_MUSE_ERROR;
    default: return GADGET_MUSE_IDLE;
    }
}

int32_t gadget_sys_millis(wasm_exec_env_t env)
{
    (void)env;
    return (int32_t)(esp_timer_get_time() / 1000);
}

/* Nothing else on the gadget sets the clock, so the first app that asks for
 * the time starts SNTP; until it has synced (or something else set the
 * clock), the answer is 0. */
int64_t gadget_sys_time(wasm_exec_env_t env)
{
    (void)env;
    static bool sntp_started;
    if (!sntp_started) {
        sntp_started = true;
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.start = true;
        if (esp_netif_sntp_init(&cfg) != ESP_OK) {
            muse_apps_log("sys_time: SNTP didn't start");
        }
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return ms > 1704067200000LL ? ms : 0;   /* before 2024: not set */
}

void gadget_sys_log(wasm_exec_env_t env, const char *text)
{
    (void)env;
    muse_apps_log("%.200s", text ? text : "");
}

int32_t gadget_sys_info(wasm_exec_env_t env, int32_t key)
{
    (void)env;
    int w, h;
    bool round;
    muse_app_view_size(&w, &h, &round);
    switch (key) {
    case GADGET_INFO_API_VERSION: return MUSE_APPS_API_MAJOR * 100 + MUSE_APPS_API_MINOR;
    case GADGET_INFO_SCREEN_WIDTH: return w;
    case GADGET_INFO_SCREEN_HEIGHT: return h;
    case GADGET_INFO_ROUND: return round;
    case GADGET_INFO_SAFE_INSET: return muse_apps_safe_inset();
    case GADGET_INFO_TOUCH: return muse_board && muse_board->touch;
    case GADGET_INFO_BUTTONS: return (int32_t)muse_app_view_buttons_present();
    case GADGET_INFO_KNOB: return 0;
    case GADGET_INFO_MEM_BUDGET: return (int32_t)muse_apps_mem_budget();
    case GADGET_INFO_MEM_USED: return (int32_t)muse_apps_mem_used();
    case GADGET_INFO_CONNECTED: return muse_link_state() == MUSE_LINK_ONLINE;
    default: return GADGET_ERR_ARG;
    }
}

int32_t gadget_sys_random(wasm_exec_env_t env)
{
    (void)env;
    return (int32_t)esp_random();
}

int32_t gadget_sys_timer(wasm_exec_env_t env, int32_t id, int32_t ms, int32_t repeat)
{
    (void)env;
    return muse_apps_timer_set(id, ms, repeat != 0);
}

int32_t gadget_sys_timer_stop(wasm_exec_env_t env, int32_t id)
{
    (void)env;
    return muse_apps_timer_stop(id);
}

int32_t gadget_sys_frame_rate(wasm_exec_env_t env, int32_t fps)
{
    (void)env;
    return muse_apps_frame_rate(fps);
}

void gadget_sys_keep_awake(wasm_exec_env_t env, int32_t on)
{
    (void)env;
    muse_apps_keep_awake(on != 0);
}

int32_t gadget_sys_brightness(wasm_exec_env_t env, int32_t percent)
{
    (void)env;
    if (!muse_board || !muse_board->set_brightness) {
        return GADGET_ERR_UNSUPPORTED;
    }
    if (percent == -1) {
        muse_ui_app_brightness(0);
        return 0;
    }
    if (percent < 1 || percent > 100) {
        return GADGET_ERR_ARG;
    }
    muse_ui_app_brightness(percent);
    return 0;
}

void gadget_sys_exit(wasm_exec_env_t env)
{
    (void)env;
    muse_apps_exit();
}

int32_t gadget_muse_mode(wasm_exec_env_t env)
{
    (void)env;
    return muse_apps_muse_mode();
}

int32_t gadget_muse_caption(wasm_exec_env_t env, void *buf, uint32_t buf_size)
{
    (void)env;
    char caption[MUSE_CAPTION_MAX] = "";
    uint32_t version = 0;
    muse_state_caption(caption, sizeof(caption), &version);
    size_t n = strlen(caption);
    if (buf_size) {
        size_t k = n < buf_size - 1 ? n : buf_size - 1;
        memcpy(buf, caption, k);
        ((char *)buf)[k] = '\0';
    }
    return (int32_t)n;
}

void gadget_muse_talk_key(wasm_exec_env_t env, int32_t on)
{
    (void)env;
    muse_app_view_talk_to_muse(on != 0);
}

void muse_apps_sys_stop(void)
{
    muse_app_view_talk_to_muse(false);
}
