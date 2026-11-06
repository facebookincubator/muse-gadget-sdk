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

// Drives main/ble_coex.c the way the BLE controller and the app do, against a
// fake scheduler that records the status bits it is given.

#include <stdio.h>
#include <stdlib.h>

#include "ble_coex.h"
#include "private/esp_coexist_internal.h"

static uint32_t s_sched[2];  // bits the scheduler holds, per type

void __real_coex_schm_status_bit_set(uint32_t type, uint32_t status) { s_sched[type] |= status; }
void __real_coex_schm_status_bit_clear(uint32_t type, uint32_t status) { s_sched[type] &= ~status; }
void __wrap_coex_schm_status_bit_set(uint32_t type, uint32_t status);
void __wrap_coex_schm_status_bit_clear(uint32_t type, uint32_t status);

static int s_fail;
#define EXPECT(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); s_fail = 1; } } while (0)

int main(void) {
#ifdef TEST_CONNECTED
    EXPECT(ble_coex_withheld() == (BLE_COEX_ADV | BLE_COEX_CONN));
#else
    EXPECT(ble_coex_withheld() == BLE_COEX_ADV);
#endif
    // Setup: the policy is off, everything passes through.
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_BLE, BLE_COEX_ADV);
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_WIFI, 0x1);
    EXPECT(s_sched[1] == BLE_COEX_ADV && s_sched[0] == 0x1);

    // Setup complete: advertising is taken back from the scheduler.
    ble_coex_set_active(true);
    EXPECT(s_sched[1] == 0);
    EXPECT(ble_coex_controller_bits() == BLE_COEX_ADV);

    // The controller re-sets it: still hidden. Wi-Fi bits always pass.
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_BLE, BLE_COEX_ADV);
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_WIFI, 0x2);
    EXPECT(s_sched[1] == 0 && s_sched[0] == 0x3);

    // A scan is never withheld by default.
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_BLE, BLE_COEX_SCAN);
    EXPECT(s_sched[1] == BLE_COEX_SCAN);
    __wrap_coex_schm_status_bit_clear(COEX_SCHM_ST_TYPE_BLE, BLE_COEX_SCAN);
    EXPECT(s_sched[1] == 0);

    // A connection replaces advertising.
    __wrap_coex_schm_status_bit_clear(COEX_SCHM_ST_TYPE_BLE, BLE_COEX_ADV);
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_BLE, BLE_COEX_CONN);
#ifdef TEST_CONNECTED
    EXPECT(s_sched[1] == 0);
#else
    EXPECT(s_sched[1] == BLE_COEX_CONN);
#endif

    // Changing the withheld set hands back what the controller still wants.
    ble_coex_set_withheld(BLE_COEX_ADV);
    EXPECT(s_sched[1] == BLE_COEX_CONN);
    ble_coex_set_withheld(BLE_COEX_ADV | BLE_COEX_CONN | 0x80);
    EXPECT(s_sched[1] == 0);
    EXPECT(ble_coex_withheld() == (BLE_COEX_ADV | BLE_COEX_CONN));

    // A bit this file doesn't know passes through and is reported.
    EXPECT(ble_coex_unknown_bits() == 0);
    __wrap_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_BLE, 0x40);
    EXPECT((s_sched[1] & 0x40) && ble_coex_unknown_bits() == 0x40);
    __wrap_coex_schm_status_bit_clear(COEX_SCHM_ST_TYPE_BLE, 0x40);

    // Back to setup: the scheduler sees exactly what the controller set.
    ble_coex_set_active(false);
    EXPECT(s_sched[1] == BLE_COEX_CONN);
    EXPECT(ble_coex_controller_bits() == BLE_COEX_CONN);

    if (!s_fail) printf("ok\n");
    return s_fail;
}
