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
 * Muse on e-paper (CONFIG_MUSE_UI_FREEINK): the face, Settings and the
 * keyboard, drawn with FreeInk instead of LVGL. FreeInk drives the panel
 * from its board profile (CONFIG_MUSE_FREEINK_DEVICE), so an e-paper board
 * supplies only what isn't the display: buttons, touch_read, audio, power.
 * It points display_pause at muse_eink_display_pause.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* muse_board_t.display_pause for FreeInk boards: stops drawing and touch
 * polling while the chip naps. The panel keeps showing what it showed. */
void muse_eink_display_pause(bool pause);

#ifdef __cplusplus
}
#endif
