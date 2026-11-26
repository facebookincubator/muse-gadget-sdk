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
 * The live call timeline (mg_lct.h).
 */
#include "mg_lct.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mg.lct";   /* "mg": on the Nordic UART log mirror */

static const char *const LABEL[MG_LCT_COUNT] = {
    [MG_LCT_T0_PRESS] = "T0 press",
    [MG_LCT_T0R_SEND] = "T0r send",
    [MG_LCT_T1C_CONNECTING] = "T1c connecting rx",
    [MG_LCT_T7R_LIVE] = "T7r live rx",
    [MG_LCT_TV_CAPS] = "Tv caps asked+answered",
    [MG_LCT_TS_START] = "Ts start_streaming rx",
    [MG_LCT_T8R_BYTES] = "T8r first video bytes",
    [MG_LCT_T8K_KEYFRAME] = "T8k first keyframe",
    [MG_LCT_T8D_DECODED] = "T8d first frame decoded",
    [MG_LCT_T9_SHOWN] = "T9 first frame shown",
    [MG_LCT_T6S_AUDIO_START] = "T6s reply audio stream_audio rx",
    [MG_LCT_T6R_AUDIO_BYTES] = "T6r first reply audio bytes",
    [MG_LCT_T6P_AUDIO_PLAYED] = "T6p first reply audio played",
    [MG_LCT_TVOL] = "Tvol call-start volume",
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_on;                       /* a call is being timed */
static int64_t s_base_us;               /* T0, or a phone-started call's first step */
static int64_t s_at[MG_LCT_COUNT];      /* 0: not yet */
static unsigned s_call;                 /* calls timed since boot: c=N on every line joins the logs */
static const char *s_by;                /* who started this call (the summary's by=), NULL: not said */
static const char *s_starting_by;       /* ...as mg_lct_begin_by hands it to the T0 mark */

/* The steps that start a timeline when none runs: the user's press, or the
 * phone's answer to a call it started. The rest belong to a call only. */
static bool starts(mg_lct_point_t pt)
{
    return pt == MG_LCT_T0_PRESS || pt == MG_LCT_T1C_CONNECTING || pt == MG_LCT_T7R_LIVE;
}

void mg_lct_begin_by(const char *by)
{
    portENTER_CRITICAL(&s_mux);
    s_starting_by = by;
    portEXIT_CRITICAL(&s_mux);
    mg_lct_mark(MG_LCT_T0_PRESS);
}

void mg_lct_begin(void)
{
    mg_lct_begin_by(NULL);
}

void mg_lct_mark(mg_lct_point_t pt)
{
    if (pt >= MG_LCT_COUNT) {
        return;
    }
    int64_t now = esp_timer_get_time();
    bool print = false, phone = false;
    int64_t base = 0;
    unsigned call = 0;
    portENTER_CRITICAL(&s_mux);
    if (!s_on && starts(pt)) {
        s_on = true;
        s_base_us = now;
        s_call++;
        /* Who started it: the gadget's own start names itself, a timeline the phone's step began is the phone's. */
        s_by = pt == MG_LCT_T0_PRESS ? s_starting_by : "phone";
        for (int i = 0; i < MG_LCT_COUNT; i++) {
            s_at[i] = 0;
        }
    }
    if (pt == MG_LCT_T0_PRESS) {
        s_starting_by = NULL;
    }
    if (s_on && !s_at[pt]) {
        s_at[pt] = now;
        base = s_base_us;
        phone = !s_at[MG_LCT_T0_PRESS];
        call = s_call;
        print = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (print) {
        int64_t d = now - base;
        ESP_LOGI(TAG, "LCT c=%u %s t=%lld +%lld.%lld ms%s", call, LABEL[pt], (long long)now, (long long)(d / 1000),
                 (long long)(d % 1000 / 100), phone ? " (phone-started: since its first step)" : "");
    }
}

void mg_lct_end_why(const char *why)
{
    int64_t at[MG_LCT_COUNT];
    int64_t base;
    bool was;
    unsigned call;
    const char *by;
    portENTER_CRITICAL(&s_mux);
    was = s_on;
    call = s_call;
    by = s_by;
    s_on = false;
    base = s_base_us;
    for (int i = 0; i < MG_LCT_COUNT; i++) {
        at[i] = s_at[i];
    }
    portEXIT_CRITICAL(&s_mux);
    if (!was) {
        return;
    }
    /* One line: who started it, what ended it, each step's ms since the base ("-": didn't happen). */
    char line[384];
    mg_lct_format_summary(line, sizeof(line), at, base, by, why);
    ESP_LOGI(TAG, "LCT c=%u %s", call, line);
}

void mg_lct_end(void)
{
    mg_lct_end_why(NULL);
}
