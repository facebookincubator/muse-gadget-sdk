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
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Firmware glue for hands-free wake word ("Hey Muse"): the audio
 * tap in muse_voice's read loop, the wake task that feeds the runner
 * and ticks the session core, synthetic-PTT posts to the muse_input
 * queue, and the runtime opt-in. The pure
 * logic lives in the muse_wakeword component (muse_wakeword.h); this
 * glue lives here, beside muse_voice/muse_state, because the voice
 * loop calls *into* it while it needs muse_voice's seeding and
 * muse_state's captions, and components cannot require each other.
 *
 * Every entry point is safe to call with the feature compiled out
 * (CONFIG_MUSE_WAKEWORD_ENABLE=n): the stubs do nothing and report
 * the runner as "off", so call sites need no conditionals.
 */

/* Once, after muse_voice_start. Synthetic events go to `queue` (the
 * muse_input queue). Failure only loses wake word; voice still runs. */
esp_err_t muse_wakeword_init(QueueHandle_t queue);

/* Tap hook: one chunk just read by muse_voice. Cheap and
 * non-blocking (a ring copy); inference never runs here. */
void muse_wakeword_on_audio(const int16_t *pcm, size_t frames);

/* A physical talk-button press (down=true) or release seen by
 * muse_voice, for the preemption rules. Synthetic events are
 * never reported through here. */
void muse_wakeword_note_ptt(bool down);

/* Runtime opt-in: persists via muse_settings. The setters in this
 * header only publish flags; the wake task applies them at its next
 * tick (disabling closes any hands-free capture in progress). */
void muse_wakeword_set_enabled(bool on);
bool muse_wakeword_enabled(void);
/* Enabled, not suppressed by playback, and not resting: what the
 * listening indicator shows. Resting follows muse_wakeword_enabled(). */
bool muse_wakeword_listening(void);

/* Reply audio (or a chirp/test) is playing: wakes are discarded and
 * pre-roll is cleared, so Muse never triggers on its own voice. */
void muse_wakeword_set_playback(bool on);

/* A physical push-to-talk turn is recording: the runner is paused
 * so the speech being recorded cannot wake a second turn.
 * Ending it clears pre-roll and resets the runner, like
 * muse_voice's own pre_reset() after a turn. */
void muse_wakeword_set_ptt_turn(bool active);

/* muse_voice ended a hands-free turn by itself (its 15 s cap, a failure,
 * or a turn that never started or didn't qualify), not at the glue's own
 * release: the glue closes its capture so the next wake is heard. */
void muse_wakeword_turn_ended(void);

/* "executorch", or "off" (not compiled in): the runner in logs/status. */
const char *muse_wakeword_runner_kind(void);

typedef struct {
    bool enabled;
    bool listening;
    const char *runner;
    int state;   /* muse_ww_state_t, kept as int to stay dependency-free */
    uint32_t wakes, cancels, submits, conversions, suppressed_wakes;
    uint32_t dropped_frames;   /* ring overload evidence */
} muse_wakeword_status_t;

void muse_wakeword_status(muse_wakeword_status_t *out);
