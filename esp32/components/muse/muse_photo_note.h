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

/*
 * Photo with voice notes (CONFIG_MUSE_PHOTO_NOTES): with the setting on, a
 * voice note the person starts with a press also takes one photo with the
 * board's camera (camera.h) at the press, and sends it with the note
 * (muse_chat_attach_jpeg). Nothing else takes a photo this way, and the photo
 * isn't kept once it has gone up or been dropped. Voice task only.
 */

/* A note starts. `press`: started by the person's own press and streaming to
 * Muse now. Takes the photo if so and the setting is on. */
void muse_photo_note_begin(bool press);
/* Before the note's end goes up: waits, briefly, until the photo is with the
 * note or has failed. A photo later than that is dropped. */
void muse_photo_note_wait(void);
/* For the note's caption while it records: "TAKING A PHOTO", "PHOTO TAKEN"
 * or "NO PHOTO", or NULL when this note has no photo. */
const char *muse_photo_note_label(void);
