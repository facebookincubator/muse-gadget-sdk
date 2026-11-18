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
#include "esp_err.h"
#include "lvgl.h"

/* Called once with the display lock held. */
lv_indev_t *muse_mirror_start(lv_display_t *disp);   /* its pointer, or NULL */
bool muse_mirror_connected(void);
/* The console reader owns RX until this blocking session returns. */
/* baud: the speed a UART console runs at for this session (0 keeps its own). */
esp_err_t muse_mirror_serial_session(int baud);
