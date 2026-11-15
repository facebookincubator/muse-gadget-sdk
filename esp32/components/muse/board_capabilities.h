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

/*
 * Devices any board with the full UI can describe from its muse_board_t,
 * registered in the device registry (components/capabilities_registry) once the board is
 * up: the battery, where the board can read its power. A board's own
 * hardware (a motor, an IMU) is registered by its board file's init().
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Once muse_app_run() has brought the board up (main/muse_glue.c). */
void board_capabilities_init(void);

#ifdef __cplusplus
}
#endif
