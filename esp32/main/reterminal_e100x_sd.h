// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include "cJSON.h"

// E1002 FAT32 microSD, shared SPI2. Commands are serialized by the module.
bool reterminal_e100x_sd_prepare_bus(void);
bool reterminal_e100x_sd_monitor_init(void);
cJSON *reterminal_e100x_sd_command(const char *command, const cJSON *params);
