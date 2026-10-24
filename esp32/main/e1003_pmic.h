// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t status, fault_latched, fault, bus;
    uint8_t input_code, charge_code;
    bool bus_good;
    bool input_present;
    bool charging;
    bool charge_done;
} e1003_pmic_status_t;

// Read-only SY6974B status. No charger registers are changed.
bool e1003_pmic_read(e1003_pmic_status_t *out);
// Status-only read for device.health; leaves the latched fault register intact.
bool e1003_pmic_read_power(bool *charging, bool *input_present);
const char *e1003_pmic_input_name(uint8_t code);
const char *e1003_pmic_charge_name(uint8_t code);
