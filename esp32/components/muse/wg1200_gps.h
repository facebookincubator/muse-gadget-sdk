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
 * WeatherXM WS1001 station GPS: parses the NMEA RMC sentence the outdoor
 * station sends in its WEATHER_GPS_SIGNED (0x04) LoRa packets, e.g.
 *   GNRMC,101554.000,A,3758.84244,N,02342.65014,E,0.00,0.00,140922,,,A
 * Header-only and free of ESP-IDF so the host tests can compile it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* "3758.84244" + 'N' -> 37.98071: degrees, then minutes. False on anything else. */
static inline bool wg1200_gps_nmea_degrees(const char *field, char hemi, double *out)
{
    char *end = NULL;
    double v = strtod(field, &end);
    if (end == field || *end != '\0' || v < 0.0) {
        return false;
    }
    double deg = (double)(long)(v / 100.0);
    double min = v - deg * 100.0;
    if (min >= 60.0) {
        return false;
    }
    double dec = deg + min / 60.0;
    if (hemi == 'S' || hemi == 'W') {
        dec = -dec;
    } else if (hemi != 'N' && hemi != 'E') {
        return false;
    }
    *out = dec;
    return true;
}

/* True with decimal degrees when `s` (len bytes, not NUL-terminated) is an
 * RMC sentence with a valid fix ('A'). */
static inline bool wg1200_gps_parse_rmc(const char *s, size_t len, double *lat, double *lon)
{
    char buf[96];
    if (!s || len == 0 || len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';

    /* Fields: 0 talker+RMC, 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W. */
    char *fields[7] = {0};
    int n = 0;
    char *p = buf;
    while (n < 7) {
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    if (n < 7) {
        return false;
    }
    const char *tag = fields[0][0] == '$' ? fields[0] + 1 : fields[0];
    if (strlen(tag) != 5 || strcmp(tag + 2, "RMC") != 0 || strcmp(fields[2], "A") != 0) {
        return false;
    }
    if (strlen(fields[4]) != 1 || strlen(fields[6]) != 1) {
        return false;
    }
    double la = 0.0, lo = 0.0;
    if (!wg1200_gps_nmea_degrees(fields[3], fields[4][0], &la) ||
        !wg1200_gps_nmea_degrees(fields[5], fields[6][0], &lo) ||
        la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0 || (la == 0.0 && lo == 0.0)) {
        return false;
    }
    *lat = la;
    *lon = lo;
    return true;
}
