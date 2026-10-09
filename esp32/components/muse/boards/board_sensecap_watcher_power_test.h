/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "muse_watcher_power_test.h"

/* Only the isolated diagnostic may call these, never the production UI.
 * GPIO masks and expander bits come from Seeed's sensecap-watcher BSP;
 * no blanket GPIO sweeps, analog-pad driving, or coprocessor flashing.
 */
esp_err_t muse_watcher_ptest_init(void);
esp_err_t muse_watcher_ptest_power(muse_ptest_readback_t *out);
esp_err_t muse_watcher_ptest_apply(const muse_ptest_state_t *state);
esp_err_t muse_watcher_ptest_readback(muse_ptest_readback_t *out);
esp_err_t muse_watcher_ptest_service(muse_ptest_load_t load);
