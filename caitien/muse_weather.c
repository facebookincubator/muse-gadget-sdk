/*
 * Copyright (c) 2026 ledienbien-ai
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

#include "muse_weather.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_wifi.h"

static const char *TAG = "muse_weather";

#define MUSE_WEATHER_URL \
    "https://api.open-meteo.com/v1/forecast?" \
    "latitude=" MUSE_WEATHER_LAT "&longitude=" MUSE_WEATHER_LON \
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature," \
    "weather_code,wind_speed_10m,is_day" \
    "&timezone=Asia%2FHo_Chi_Minh"

#define WX_BUF_MAX 2048
#define WX_RETRY_S 60

static SemaphoreHandle_t s_wx_lock;
static muse_weather_t s_wx = {
    .valid = false,
    .stale = false,
    .humidity_pct = -1,
    .wind_kmh = -1.0f,
    .icon = MUSE_WX_UNKNOWN,
    .updated_hhmm = "--:--",
};
static int64_t s_last_ok_us;

/* Bảng ánh xạ mã thời tiết WMO sang tiếng Việt + icon (mục 2.3 DESIGN.md). */
static const char *wmo_describe(int code, bool is_day, muse_wx_icon_t *icon_out)
{
    switch (code) {
    case 0:
        *icon_out = is_day ? MUSE_WX_SUN : MUSE_WX_MOON;
        return "Trời quang";
    case 1:
        *icon_out = is_day ? MUSE_WX_PARTLY : MUSE_WX_CLOUDY;
        return "Ít mây";
    case 2:
        *icon_out = is_day ? MUSE_WX_PARTLY : MUSE_WX_CLOUDY;
        return "Có mây";
    case 3:
        *icon_out = MUSE_WX_CLOUDY;
        return "Nhiều mây";
    case 45:
    case 48:
        *icon_out = MUSE_WX_FOG;
        return "Sương mù";
    case 51:
    case 53:
    case 55:
        *icon_out = MUSE_WX_RAIN;
        return "Mưa phùn";
    case 56:
    case 57:
        *icon_out = MUSE_WX_RAIN;
        return "Mưa phùn lạnh";
    case 61:
    case 63:
    case 65:
        *icon_out = MUSE_WX_RAIN;
        return "Mưa";
    case 66:
    case 67:
        *icon_out = MUSE_WX_RAIN;
        return "Mưa lạnh";
    case 71:
    case 73:
    case 75:
    case 77:
        *icon_out = MUSE_WX_SNOW;
        return "Tuyết";
    case 80:
    case 81:
    case 82:
        *icon_out = MUSE_WX_RAIN;
        return "Mưa rào";
    case 85:
    case 86:
        *icon_out = MUSE_WX_SNOW;
        return "Tuyết rào";
    case 95:
        *icon_out = MUSE_WX_THUNDER;
        return "Dông";
    case 96:
    case 99:
        *icon_out = MUSE_WX_THUNDER;
        return "Dông có mưa đá";
    default:
        *icon_out = MUSE_WX_CLOUDY;
        return "Có mây";
    }
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_sink_t;

static esp_err_t on_http_data(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        http_sink_t *s = (http_sink_t *)evt->user_data;
        size_t copy = (size_t)evt->data_len;
        if (s->len + copy >= s->cap) {
            copy = s->cap - 1 - s->len;
        }
        if (copy > 0) {
            memcpy(s->buf + s->len, evt->data, copy);
            s->len += copy;
            s->buf[s->len] = '\0';
        }
    }
    return ESP_OK;
}

static const char *json_find_key(const char *obj, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static bool json_float(const char *obj, const char *key, float *out)
{
    const char *p = json_find_key(obj, key);
    if (!p) {
        return false;
    }
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

static bool json_int(const char *obj, const char *key, int *out)
{
    const char *p = json_find_key(obj, key);
    if (!p) {
        return false;
    }
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool json_string(const char *obj, const char *key, char *out, size_t cap)
{
    const char *p = json_find_key(obj, key);
    if (!p || *p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"';
}

static bool fetch_weather(muse_weather_t *out)
{
    char *buf = heap_caps_malloc(WX_BUF_MAX, MUSE_BIG_CAPS);
    if (!buf) {
        return false;
    }
    buf[0] = '\0';
    http_sink_t sink = { .buf = buf, .len = 0, .cap = WX_BUF_MAX };

    esp_http_client_config_t cfg = {
        .url = MUSE_WEATHER_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_data,
        .user_data = &sink,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || sink.len == 0) {
        ESP_LOGW(TAG, "Open-Meteo HTTP failed: err=%s status=%d", esp_err_to_name(err), status);
        free(buf);
        return false;
    }

    /* Tìm đối tượng "current": { ... } ở cuối JSON. */
    const char *cur = strstr(buf, "\"current\"");
    if (cur) {
        /* Bỏ qua "current_units" nếu trùng tiền tố. */
        while (cur && strncmp(cur, "\"current_units\"", 15) == 0) {
            cur = strstr(cur + 15, "\"current\"");
        }
    }
    if (!cur) {
        ESP_LOGW(TAG, "Open-Meteo JSON missing 'current'");
        free(buf);
        return false;
    }
    const char *brace = strchr(cur, '{');
    if (!brace) {
        free(buf);
        return false;
    }

    float temp_c = 0.0f, feels_c = 0.0f, wind_kmh = -1.0f;
    int hum = -1, code = 0, is_day = 1;
    if (!json_float(brace, "temperature_2m", &temp_c)) {
        free(buf);
        return false;
    }
    if (!json_float(brace, "apparent_temperature", &feels_c)) {
        feels_c = temp_c;
    }
    json_int(brace, "relative_humidity_2m", &hum);
    json_float(brace, "wind_speed_10m", &wind_kmh);
    json_int(brace, "weather_code", &code);
    json_int(brace, "is_day", &is_day);

    char iso_time[32] = {0};
    json_string(brace, "time", iso_time, sizeof(iso_time));
    free(buf);

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->stale = false;
    out->temp_c = temp_c;
    out->feels_c = feels_c;
    out->humidity_pct = hum;
    out->wind_kmh = wind_kmh;
    out->code = code;
    out->is_day = (is_day != 0);

    const char *desc = wmo_describe(code, out->is_day, &out->icon);
    strlcpy(out->desc, desc, sizeof(out->desc));

    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(out->updated_hhmm, sizeof(out->updated_hhmm), "%H:%M", &tmv);
    } else {
        const char *t_sep = strchr(iso_time, 'T');
        if (t_sep && strlen(t_sep + 1) >= 5) {
            snprintf(out->updated_hhmm, sizeof(out->updated_hhmm), "%.5s", t_sep + 1);
        } else {
            strlcpy(out->updated_hhmm, "--:--", sizeof(out->updated_hhmm));
        }
    }

    ESP_LOGI(TAG, "Đồng Xoài: %.1f°C (cảm giác %.1f°C), %s, ẩm %d%%, gió %.1f km/h [%s]",
             out->temp_c, out->feels_c, out->desc, out->humidity_pct, out->wind_kmh, out->updated_hhmm);
    return true;
}

static void weather_task(void *arg)
{
    (void)arg;
    bool sntp_started = false;

    for (;;) {
        muse_wifi_status_t ws;
        muse_wifi_status(&ws);
        if (ws.state != MUSE_WIFI_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!sntp_started) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_cfg.wait_for_sync = false;
            if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
                sntp_started = true;
                ESP_LOGI(TAG, "SNTP started (pool.ntp.org, TZ=ICT-7)");
            }
        }

        muse_weather_t fresh;
        bool ok = fetch_weather(&fresh);
        if (ok) {
            xSemaphoreTake(s_wx_lock, portMAX_DELAY);
            s_wx = fresh;
            s_last_ok_us = esp_timer_get_time();
            xSemaphoreGive(s_wx_lock);
        }

        int wait_s = ok ? MUSE_WEATHER_REFRESH_S : WX_RETRY_S;
        for (int elapsed = 0; elapsed < wait_s; elapsed += 5) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void muse_weather_start(void)
{
    setenv("TZ", "ICT-7", 1);
    tzset();

    if (!s_wx_lock) {
        s_wx_lock = xSemaphoreCreateMutex();
    }
    if (xTaskCreatePinnedToCoreWithCaps(weather_task, "muse_wx", 6144, NULL, 3,
                                        NULL, 0, MUSE_BIG_CAPS) != pdPASS) {
        xTaskCreate(weather_task, "muse_wx", 6144, NULL, 3, NULL);
    }
}

void muse_weather_get(muse_weather_t *out)
{
    if (!out) {
        return;
    }
    if (!s_wx_lock) {
        *out = s_wx;
        return;
    }
    xSemaphoreTake(s_wx_lock, portMAX_DELAY);
    *out = s_wx;
    if (out->valid && s_last_ok_us > 0) {
        int64_t age_s = (esp_timer_get_time() - s_last_ok_us) / 1000000LL;
        out->stale = (age_s > MUSE_WEATHER_STALE_S);
    }
    xSemaphoreGive(s_wx_lock);
}

int muse_weather_idle_timeout(void)
{
    int w = muse_settings_weather_idle_s();
    if (w <= 0) {
        return 0;
    }
    int slp = muse_settings_sleep_s();
    if (slp > 0 && w > slp - 5) {
        w = slp > 5 ? slp - 5 : 0;
    }
    return w;
}
