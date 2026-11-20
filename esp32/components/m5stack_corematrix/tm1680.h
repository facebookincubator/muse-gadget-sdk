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

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_lcd_types.h"

// A TM1680 driving a 16x16 matrix as the CoreMatrix wires it, as an esp_lcd
// panel (see corematrix_new_matrix()).
esp_err_t tm1680_new_panel(i2c_master_bus_handle_t bus, uint16_t address,
                           esp_lcd_panel_handle_t *panel);
