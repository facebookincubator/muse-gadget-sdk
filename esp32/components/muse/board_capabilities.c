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

/* The devices every board with the full UI can describe (board_capabilities.h). */

#include "board_capabilities.h"

#include "muse_board.h"
#include "capabilities_registry.h"
#include "muse_state.h"

/* The well-known battery, exactly as components/muse_apps/sdk/api/api.json has it. */
static const capability_field_t s_battery_fields[] = {
    {"percent", "%", 0, 100},
    {"volts", "V", 0, 5},
    {"charging", "", 0, 1},
    {"usb", "", 0, 1},
};

/* The input task reads the power chip every 2 s (10 s at rest) and keeps the
 * result (muse_input.c), so this never touches the bus. */
static int battery_read(const capability_t *dev, float *values)
{
    (void)dev;
    muse_power_t p = muse_state_power();
    if (p.battery_pct < 0) {
        return -1;   /* no battery, or not read yet */
    }
    values[0] = (float)p.battery_pct;
    values[1] = p.battery_mv / 1000.0f;   /* 0 where the board can't measure it */
    values[2] = p.charging ? 1.0f : 0.0f;
    values[3] = p.usb ? 1.0f : 0.0f;
    return 4;
}

static const capability_t s_battery = {
    .name = "battery",
    .desc = "The gadget's battery, as its power chip last measured it (every few seconds).",
    .fields = s_battery_fields,
    .n_fields = 4,
    .rate = 0,
    .read = battery_read,
};

void board_capabilities_init(void)
{
    if (muse_board && muse_board->read_power) {
        capabilities_registry_register(&s_battery);
    }
}
