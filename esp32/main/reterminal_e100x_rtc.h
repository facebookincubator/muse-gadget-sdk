// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>

typedef struct {
    int year, month, day, hour, minute, second;
    bool valid;
} reterminal_e100x_rtc_time_t;

bool reterminal_e100x_rtc_read(reterminal_e100x_rtc_time_t *out);
bool reterminal_e100x_rtc_set(const reterminal_e100x_rtc_time_t *time);
