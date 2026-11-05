// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available;
    bool pressed;
    uint16_t x, y;
    uint16_t sensor_width, sensor_height;
} e1003_touch_state_t;

bool e1003_touch_init(void);
void e1003_touch_read(e1003_touch_state_t *out);
