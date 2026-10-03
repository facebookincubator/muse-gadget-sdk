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

// Push-to-talk voice chat with the agent. Hold the button and speak; on
// release the recording goes to the agent's chat as a voice note, which the
// VM transcribes, over the voice session borrowed from Muse (muse_chat.h).
// The reply is text and shows up in the Muse app; a TTS API of your own can
// speak it (start_tts in muse_chat_session.cpp).

#pragma once

#include "cJSON.h"

// Start the voice task. It brings up the audio hardware and then takes over
// the button whenever a turn can run.
void voice_init(void);

// voice.configure: sets the speaker volume (0-100), kept in NVS. The dial on
// top sets it too.
cJSON *voice_configure_command(cJSON *params);
