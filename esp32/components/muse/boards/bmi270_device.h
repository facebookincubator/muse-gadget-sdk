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
#pragma once

/*
 * A Bosch BMI270 on a board's I2C bus, as the well-known devices accel, gyro
 * and steps (components/capabilities_registry, components/muse_apps/sdk/api/api.json). One per gadget.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t addr;       /* 0x68, or 0x69 with SDO high */
    /* The screen's frame (x right, y up, z out of the screen) in the chip's
     * axes: screen axis i is chip axis axis[i] (0 x, 1 y, 2 z) times sign[i]
     * (1 or -1). Gyro rates follow the same axes. */
    uint8_t axis[3];
    int8_t sign[3];
} bmi270_device_cfg_t;

/*
 * At start-up, once the bus is up (a soft reset just before is fine): checks
 * that a BMI270 answers and registers its three devices. It only reads the
 * chip's id: the chip stays as it was (suspended) until an app opens a device.
 * False, with a log line, if there's no BMI270 there; boot carries on.
 */
bool bmi270_device_register(const bmi270_device_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
