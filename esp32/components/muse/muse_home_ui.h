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
 * The v1.0 home (muse_home.c), its icon set (muse_icons.c) and the passcode
 * lock (muse_passcode*.c): the Muse Gadgets UI, in builds with BLE gadget
 * audio. Without it a touch board draws the face as boards without the home
 * do, Settings rows lead with the font's symbols, and there is no passcode
 * (the headers' stubs). The simulator and the host tests build all of it.
 */
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#define MUSE_HOME_UI 1
#else
#define MUSE_HOME_UI 0
#endif
#else
#define MUSE_HOME_UI 1
#endif
