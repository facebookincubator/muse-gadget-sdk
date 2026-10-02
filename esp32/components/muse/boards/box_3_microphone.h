/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

esp_err_t box_3_microphone_init(void);
esp_codec_dev_handle_t box_3_microphone_create(void);
void box_3_microphone_poll(void);
void box_3_microphone_set_gain(int db);
