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

// The 1.54G's voice extras: the green LED, the reply on the e-paper and the PWR cues.
bool voice_epaper_154g_led_init(void);

// The mic is warm and the start cue has finished: LISTENING turns steady.
void voice_epaper_154g_mic_ready(bool ready);

// Queues one status-band refresh, without waiting for the panel. NULL clears it.
void voice_epaper_154g_show_text(const char *reply);

// A click for a PWR page turn, a lower tone for the rotation hold.
void voice_epaper_154g_button_cue(bool long_hold);
