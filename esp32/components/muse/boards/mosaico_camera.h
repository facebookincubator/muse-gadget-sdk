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

#include <stdbool.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the Camera Module in the left expansion slot as the camera. bus
 * is the slot's I2C bus where the board shares it (v1.0), or NULL for the
 * slot's own bus (v1.2). */
void mosaico_camera_register(i2c_master_bus_handle_t bus);
/* camera.capture: one JPEG, base64-encoded; *error says why on false. */
bool mosaico_camera_capture(char **jpeg_base64, const char **error);

#ifdef __cplusplus
}
#endif
