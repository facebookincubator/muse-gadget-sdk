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
#include "muse_clock.h"

#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "muse_clock";

#define NVS_NS "muse_clock"
#define SYNCED_MAGIC 0x434C4B53u   /* "CLKS" */

/* Survives a software restart, as the system time does. */
static RTC_NOINIT_ATTR uint32_t s_synced;
static int16_t s_offset;
static uint8_t s_flags;
static int16_t s_saved_offset;   /* what NVS holds */
static uint8_t s_saved_flags;
static bool s_loaded;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int16_t off;
        uint8_t fl;
        if (nvs_get_i16(h, "off", &off) == ESP_OK) {
            s_offset = s_saved_offset = off;
        }
        if (nvs_get_u8(h, "flags", &fl) == ESP_OK) {
            s_flags = s_saved_flags = fl;
        }
        nvs_close(h);
    }
}

void muse_clock_set(int64_t unix_ms, bool has_offset, int16_t offset_min, bool has_flags, uint8_t flags, bool store)
{
    load();
    struct timeval tv = { .tv_sec = (time_t)(unix_ms / 1000), .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000) };
    if (tv.tv_usec < 0) {
        tv.tv_sec -= 1;
        tv.tv_usec += 1000000;
    }
    settimeofday(&tv, NULL);
    portENTER_CRITICAL(&s_mux);
    if (has_offset) {
        s_offset = offset_min;
    }
    if (has_flags) {
        s_flags = flags;
    }
    bool first = s_synced != SYNCED_MAGIC;
    s_synced = SYNCED_MAGIC;
    int16_t off = s_offset;
    uint8_t fl = s_flags;
    portEXIT_CRITICAL(&s_mux);
    if (store && (off != s_saved_offset || fl != s_saved_flags)) {
        /* Only when they change: no flash write on every connection. */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i16(h, "off", off);
            nvs_set_u8(h, "flags", fl);
            if (nvs_commit(h) == ESP_OK) {
                s_saved_offset = off;
                s_saved_flags = fl;
            }
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "set from the phone%s: UTC offset %+d min, %s clock%s", first ? " (first since power-on)" : "",
             off, fl & 1 ? "24-hour" : "12-hour", store ? "" : " (not stored: no token proof)");
}

void muse_clock_get(int64_t *unix_ms, int16_t *offset_min, uint8_t *flags)
{
    load();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *unix_ms = s_synced == SYNCED_MAGIC ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : INT64_MIN;
    *offset_min = s_offset;
    *flags = s_flags;
}

bool muse_clock_local(struct tm *out)
{
    int64_t ms;
    int16_t off;
    uint8_t fl;
    muse_clock_get(&ms, &off, &fl);
    if (ms == INT64_MIN) {
        return false;
    }
    time_t t = (time_t)(ms / 1000) + (time_t)off * 60;
    gmtime_r(&t, out);
    return true;
}

bool muse_clock_24h(void)
{
    load();
    return s_flags & 1;
}
