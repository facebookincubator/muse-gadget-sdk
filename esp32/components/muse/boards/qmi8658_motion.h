/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */
#pragma once
#include "driver/i2c_master.h"
#include "muse_motion_core.h"

esp_err_t muse_qmi8658_enable(i2c_master_bus_handle_t bus, bool enable);
esp_err_t muse_qmi8658_read(muse_motion_sample_t *out);
