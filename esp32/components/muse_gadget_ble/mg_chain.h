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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A board's speech chain on the player's output (mg_ble_platform_t.speaker_chain:
 * the ESP-Mosaico's, muse_spk_dsp.h), a stage with a look-ahead: what it was
 * given last comes out only with what follows. So: reply audio goes through
 * it (mg_chain_audio); silence goes through it too while it still holds
 * anything (mg_chain_silence: its tail plays out, then it's quiet: emptied,
 * and silence passes untouched until audio comes back, which starts it
 * afresh); a reply the speaker already went quiet on (a barge-in's clear,
 * then a hold's claim or a flush) empties it unplayed (mg_chain_discard).
 * A barge-in pause doesn't touch it: the state waits with the reply, and a
 * tap's resume writes the rewind (post-chain, as the DMA held it) and then
 * goes on through the chain from the next sample, each sample once.
 * Without a chain every call passes the audio as it comes.
 */
typedef struct {
    void (*run)(int16_t *pcm, size_t n);
    void (*reset)(void);
    bool quiet;                    /* holds nothing audible: silence bypasses it */
} mg_chain_t;

void mg_chain_init(mg_chain_t *c, void (*run)(int16_t *, size_t), void (*reset)(void));
void mg_chain_audio(mg_chain_t *c, int16_t *pcm, size_t n);
/* n samples of silence for the speaker: `silence` itself while the chain is quiet, else zeros run through
 * the chain in scratch (n at least its look-ahead) and the chain quiet once they came out all zero. */
const int16_t *mg_chain_silence(mg_chain_t *c, const int16_t *silence, int16_t *scratch, size_t n);
void mg_chain_discard(mg_chain_t *c);

#ifdef __cplusplus
}
#endif
