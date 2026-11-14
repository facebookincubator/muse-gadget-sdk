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

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The wake word (CONFIG_MUSE_WAKE_WORD): while the gadget is idle, the mic
 * audio the voice task already captures for its pre-roll (muse_voice's
 * idle_capture) is also fed to Espressif's on-device speech recognition
 * (esp-sr). Nothing is recorded or sent anywhere: the audio never leaves
 * the chip. When the wake word is heard, muse_wake posts the same input
 * gesture as a tap on the talk button (muse_input_wake_word), so a live
 * call starts exactly as if the button had been tapped.
 *
 * Which word, and which engine, is a build-time choice
 * (CONFIG_MUSE_WAKE_WORD_ENGINE): MultiNet speech commands hearing
 * "Hi Muse", or the stock WakeNet9 word "Jarvis". The models live in the
 * "model" partition (partitions_muse.csv), packed by esp-sr's build from
 * its own model selection (SR_MN_EN_MULTINET7_QUANT, or
 * SR_WN_WN9_JARVIS_TTS); the board overlay selects both.
 *
 * Listening is a Sound setting (muse_settings_wake_word), on by default
 * when the feature is built in. Off, the detector task parks and the mic
 * audio fed here is dropped.
 */

/* Creates the detector task. The engines load inside it, so a missing or
 * mismatched model partition only costs the log line, never the boot. */
esp_err_t muse_wake_start(void);

/* One chunk of idle mic audio (mono, MUSE_AUDIO_RATE), from the voice
 * task's idle capture: the only audio the detector ever sees, so it hears
 * nothing while a note records, a turn runs or a call is up. Cheap and
 * non-blocking; chunks are dropped when the detector is off, busy or not
 * started. */
void muse_wake_feed(const int16_t *mono, size_t frames);
