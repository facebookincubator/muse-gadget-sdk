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

#pragma once

#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tọa độ mặc định: Đồng Xoài, Bình Phước (11.5188°N, 106.9073°E). */
#define MUSE_WEATHER_LAT "11.5188"
#define MUSE_WEATHER_LON "106.9073"

/* Chu kỳ cập nhật: 15 phút (900 giây); quá 60 phút (3600 giây) coi là dữ liệu cũ. */
#define MUSE_WEATHER_REFRESH_S 900
#define MUSE_WEATHER_STALE_S   3600

typedef enum {
    MUSE_WX_UNKNOWN = 0,
    MUSE_WX_SUN,
    MUSE_WX_MOON,
    MUSE_WX_PARTLY,
    MUSE_WX_CLOUDY,
    MUSE_WX_FOG,
    MUSE_WX_RAIN,
    MUSE_WX_THUNDER,
    MUSE_WX_SNOW,
} muse_wx_icon_t;

typedef struct {
    bool valid;             /* đã lấy thành công ít nhất 1 lần từ khi bật máy */
    bool stale;             /* quá 60 phút chưa cập nhật được */
    float temp_c;           /* nhiệt độ hiện tại (°C) */
    float feels_c;          /* nhiệt độ cảm nhận (°C) */
    int humidity_pct;       /* độ ẩm (%), hoặc -1 nếu thiếu */
    float wind_kmh;         /* tốc độ gió (km/h), hoặc -1 nếu thiếu */
    int code;               /* mã WMO weather_code */
    bool is_day;            /* 1 = ban ngày, 0 = ban đêm */
    muse_wx_icon_t icon;    /* icon tương ứng với code + is_day */
    char desc[48];          /* mô tả tiếng Việt ("Có mây", "Mưa rào", ...) */
    char updated_hhmm[16];  /* giờ cập nhật ("14:15" hoặc "--:--") */
} muse_weather_t;

#if CONFIG_MUSE_BOARD_SIMULATOR
static inline void muse_weather_start(void) {}
static inline void muse_weather_get(muse_weather_t *out) { if (out) memset(out, 0, sizeof(*out)); }
static inline int muse_weather_idle_timeout(void) { return 0; }
#else
/* Khởi tạo múi giờ ICT-7, SNTP và tạo task nền lấy thời tiết Open-Meteo. */
void muse_weather_start(void);

/* Đọc bản sao dữ liệu thời tiết hiện tại từ cache (an toàn đa luồng). */
void muse_weather_get(muse_weather_t *out);

/* Thời gian nghỉ hiệu dụng (giây) trước khi hiện màn hình thời tiết,
 * đã kẹp theo auto-sleep (slp - 5s); trả về 0 nếu tắt. */
int muse_weather_idle_timeout(void);
#endif

/* Ẩn overlay thời tiết ngay lập tức (định nghĩa trong muse_ui.c). */
void muse_weather_hide(void);

#ifdef __cplusplus
}
#endif

