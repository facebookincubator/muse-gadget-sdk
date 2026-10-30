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
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "muse_state.h"

/*
 * Minimal AXP2101 access, for boards that have one: power-key edges (for
 * boards where the PWR button reaches only the PMU) and battery/USB status.
 * Rails are left as the BSP set them, but for turning off unused ones.
 */

#define MUSE_PMU_KEY_PRESS   (1u << 0)
#define MUSE_PMU_KEY_RELEASE (1u << 1)
#define MUSE_PMU_KEY_CLICK   (1u << 2)
#define MUSE_PMU_KEY_LONG    (1u << 3)

/* key_irqs: latch power-key edges for muse_pmu_poll_key(); otherwise every
 * PMU interrupt is turned off. */
esp_err_t muse_pmu_init(i2c_master_bus_handle_t bus, bool key_irqs);

/* Turns off every rail but these, by their enable bits: dcdc has DCDC1-5 in
 * bits 0-4; ldo has ALDO1-4, BLDO1-2, CPUSLDO and DLDO1 in bits 0-7 and DLDO2
 * in bit 8. Turns nothing on. */
esp_err_t muse_pmu_keep_rails(uint8_t dcdc, uint16_t ldo);

/* Whether holding PWR powers the board off: past the 10 s hold-off, and after
 * 16 s where the PMU has that on. Boards that talk on PWR turn it on
 * after muse_pmu_init(), off while PWR is held (a talk can outlast both) and on
 * again at release; the 16 s one comes back only if the hold turned it off.
 * Others leave it as the PMU has it. ESP_ERR_INVALID_STATE without a PMU. */
esp_err_t muse_pmu_set_key_shutdown(bool on);

/* Returns and clears latched MUSE_PMU_KEY_* events. */
unsigned muse_pmu_poll_key(void);

esp_err_t muse_pmu_read_power(muse_power_t *out);

/* Soft power-off: the AXP2101 cuts every rail. PWR turns the board back on. */
esp_err_t muse_pmu_power_off(void);
