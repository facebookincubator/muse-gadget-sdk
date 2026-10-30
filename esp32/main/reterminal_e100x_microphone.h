// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include "cJSON.h"

// Bounded, on-demand PCM capture. Caller serializes hardware operations.
bool reterminal_e100x_microphone_init(void);
cJSON *reterminal_e100x_microphone_record(const cJSON *params);
