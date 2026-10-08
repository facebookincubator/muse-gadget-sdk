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

// Host SHT4x harness: raw sensor bytes in, sensors.read result out.
// The runner extracts the production protocol code into sht4x_protocol.inc.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#include "sht4x_protocol.inc"

#define S(x) (x * 1000000LL)

// Build a 6-byte measurement with valid CRCs.
static void make_raw(uint16_t raw_t, uint16_t raw_rh, uint8_t out[6]) {
    out[0] = (uint8_t)(raw_t >> 8);
    out[1] = (uint8_t)raw_t;
    out[2] = sht4x_crc(out, 2);
    out[3] = (uint8_t)(raw_rh >> 8);
    out[4] = (uint8_t)raw_rh;
    out[5] = sht4x_crc(out + 3, 2);
}

static void check_crc(void) {
    // Sensirion datasheet vector.
    const uint8_t vec[] = {0xBE, 0xEF};
    assert(sht4x_crc(vec, sizeof(vec)) == 0x92);
    assert(sht4x_crc(vec, 0) == 0xFF);
}

static void check_decode(void) {
    uint8_t raw[6];
    float t, rh;

    // Endpoints of the conversion formulas.
    make_raw(0x0000, 0x0000, raw);
    assert(sht4x_decode(raw, &t, &rh));
    assert(fabsf(t - -45.0f) < 0.01f);
    assert(rh == 0.0f);  // -6% clamps to 0
    make_raw(0xFFFF, 0xFFFF, raw);
    assert(sht4x_decode(raw, &t, &rh));
    assert(fabsf(t - 130.0f) < 0.01f);
    assert(rh == 100.0f);  // 119% clamps to 100

    // A mid-scale reading: 0x8000 -> 42.5 C, 56.5 %RH.
    make_raw(0x8000, 0x8000, raw);
    assert(sht4x_decode(raw, &t, &rh));
    assert(fabsf(t - 42.5f) < 0.02f);
    assert(fabsf(rh - 56.5f) < 0.02f);

    // Corrupted CRCs are rejected.
    make_raw(0x8000, 0x8000, raw);
    raw[2] ^= 0x01;
    assert(!sht4x_decode(raw, &t, &rh));
    make_raw(0x8000, 0x8000, raw);
    raw[5] ^= 0x80;
    assert(!sht4x_decode(raw, &t, &rh));
}

static void check_result(void) {
    reading_t r[SENSOR_COUNT] = {0};

    // Nothing yet: an error, not an all-null payload.
    cJSON *result = readings_result(r, S(10));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON *error = cJSON_GetObjectItem(result, "error");
    assert(!strcmp(cJSON_GetObjectItem(error, "code")->valuestring, "no_readings"));
    assert(!cJSON_GetObjectItem(result, "payload"));
    cJSON_Delete(result);

    // Fresh readings round to one decimal and carry their age.
    r[SENSOR_TEMPERATURE] = (reading_t){true, 23.46f, S(7)};
    r[SENSOR_HUMIDITY] = (reading_t){true, 41.04f, S(9)};
    result = readings_result(r, S(10));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    int keys = 0;
    for (cJSON *item = result->child; item; item = item->next) keys++;
    assert(keys == 2);
    char *json = cJSON_PrintUnformatted(cJSON_GetObjectItem(result, "payload"));
    assert(strstr(json, "\"temperature\":{\"value\":23.5,\"unit\":\"celsius\",\"age_s\":3}"));
    assert(strstr(json, "\"humidity\":{\"value\":41,\"unit\":\"percent_rh\",\"age_s\":1}"));
    cJSON_free(json);
    cJSON_Delete(result);

    // Stale readings are null but the other sensor still reports.
    r[SENSOR_TEMPERATURE].at_us = S(10) - STALE_US - 1;
    result = readings_result(r, S(10));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    assert(cJSON_IsNull(cJSON_GetObjectItem(payload, "temperature")));
    assert(cJSON_IsNumber(cJSON_GetObjectItem(
        cJSON_GetObjectItem(payload, "humidity"), "value")));
    cJSON_Delete(result);
}

int main(void) {
    check_crc(); check_decode(); check_result();
    puts("PASS reterminal sht4x: CRC vector, decode endpoints and clamping, bad CRCs, result envelope, stale handling");
    return 0;
}
