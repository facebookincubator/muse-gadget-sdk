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
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t muse_sticky_ptest_display_init(void);
esp_err_t muse_sticky_ptest_display_start(bool fast);
esp_err_t muse_sticky_ptest_display_service(void);
esp_err_t muse_sticky_ptest_display_stop(void);
uint32_t muse_sticky_ptest_display_frames(void);
#ifdef __cplusplus
}
#endif
