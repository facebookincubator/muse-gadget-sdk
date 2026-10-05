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

// Host harness for the ports' and storage's pure parts: hex, the RTC's
// registers and dates (main/muse_hw_io.c), and paths on the SD card
// (main/muse_hw_storage.c). The runner extracts their "host-tested" sections.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOUNT "/sdcard"
#include "io.inc"
#include "storage.inc"

static void test_hex(void) {
    uint8_t b[8];
    assert(parse_hex("0a ff 12", b, 8) == 3 && b[0] == 0x0a && b[1] == 0xff && b[2] == 0x12);
    assert(parse_hex("0AFF12", b, 8) == 3 && b[1] == 0xff);
    assert(parse_hex("0x24,0x00", b, 8) == 2 && b[0] == 0x24 && b[1] == 0);
    assert(parse_hex("", b, 8) == 0);
    assert(parse_hex("0", b, 8) == -1 && parse_hex("zz", b, 8) == -1 && parse_hex("aabbcc", b, 2) == -1);
    char out[16];
    to_hex((const uint8_t *)"\x0a\xff\x12", 3, out);
    assert(!strcmp(out, "0a ff 12"));
    to_hex(b, 0, out);
    assert(!strcmp(out, ""));
}

static void test_clock(void) {
    assert(days_from_civil(1970, 1, 1) == 0);
    assert(days_from_civil(2000, 3, 1) == 11017);
    assert(days_from_civil(2026, 9, 30) == 20726);
    // Round trips across months, leap days and the year, against the host's gmtime.
    const int64_t times[] = { 1735689600, 1740787199, 1740787200, 1790744181, 4102444799, 1709164800 };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        uint8_t r[7];
        rtc_encode(times[i], r);
        assert(rtc_decode(r) == times[i]);
        time_t t = (time_t)times[i];
        struct tm tm;
        gmtime_r(&t, &tm);
        assert(from_bcd(r[3]) == tm.tm_mday && from_bcd(r[5]) == tm.tm_mon + 1);
        assert(from_bcd(r[6]) == tm.tm_year % 100 && r[4] == tm.tm_wday && !(r[0] & 0x80));
    }
    // The voltage-low flag: the time was lost.
    uint8_t lost[7] = { 0x88, 0xa2, 0x84, 0x95, 0x8d, 0x80, 0x0e };
    assert(rtc_decode(lost) == -1);
    uint8_t bad[7] = { 0x00, 0x00, 0x00, 0x32, 0x00, 0x13, 0x26 };   // day 32, month 13
    assert(rtc_decode(bad) == -1);
}

static void test_paths(void) {
    char p[64];
    assert(card_path("photos/a.jpg", p, sizeof(p)) && !strcmp(p, "/sdcard/photos/a.jpg"));
    assert(card_path("/photos/a.jpg", p, sizeof(p)) && !strcmp(p, "/sdcard/photos/a.jpg"));
    assert(card_path("", p, sizeof(p)) && !strcmp(p, "/sdcard"));
    assert(card_path("/", p, sizeof(p)) && !strcmp(p, "/sdcard"));
    assert(!card_path("../etc", p, sizeof(p)) && !card_path("a/../../b", p, sizeof(p)));
    assert(!card_path("a//b", p, sizeof(p)) && !card_path(NULL, p, sizeof(p)));
    assert(card_path("a/..b/c", p, sizeof(p)));   // "..b" is a name, not a parent
    char tiny[12];
    assert(!card_path("a-long-name.txt", tiny, sizeof(tiny)));
}

int main(void) {
    test_hex();
    test_clock();
    test_paths();
    printf("ok\n");
    return 0;
}
