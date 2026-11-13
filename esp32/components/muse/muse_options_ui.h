// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "lvgl.h"
void muse_options_ui_start(int width, int height, bool round, const lv_font_t *font);
void muse_options_ui_tick(bool available);
