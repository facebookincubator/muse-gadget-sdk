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

/*
 * The board's mic and speaker codecs share one duplex I2S bus, so both run at
 * the same format: 16 kHz, 16-bit, 2 slots. This module hides that and
 * exposes mono buffers. Captured audio is high-passed to remove DC and rumble.
 */

#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 320   /* 20 ms of mono frames */

esp_err_t muse_audio_init(int volume, int mic_gain_db);

/* Off closes the codecs: ADC, DAC, speaker amp and I2S stop. On reopens them
 * with the saved volume and mic gain. Reads and writes need it on. */
void muse_audio_power(bool on);
/* Which muse_audio function holds its lock now, or NULL: a stuck-task report. */
const char *muse_audio_lock_holder(void);

void muse_audio_set_volume(int volume);          /* 0..100 */
void muse_audio_set_mic_gain(int db);            /* 0..MUSE_MIC_GAIN_MAX, applied now */
/* A client's set_mic_gain for its connection's captures (mgcommands.h): db,
 * or -1 to go back to the user's Settings gain (muse_settings_mic_gain, the
 * tuned one by default). RAM only: nothing is saved and the Settings value
 * stays. Doesn't block (any task, the BLE host too): the next read applies it,
 * and every codec open uses it. */
void muse_audio_mic_gain_override(int db);
/* The gain captures use now, dB: the override, else the Settings gain. */
int muse_audio_mic_gain(void);
/* The connection's override (muse_audio_mic_gain_override), dB, or -1: none. */
int muse_audio_mic_gain_override_db(void);

/* Measures the real capture/playback rates and per-mic levels; logs the result. */
void muse_audio_selftest(void);

/* Plays tones while recording, across mic gains; logs levels. For tuning. */
void muse_audio_loopback_test(int volume);

/* Blocking read of `frames` mono samples (mic pair mixed down). */
esp_err_t muse_audio_read(int16_t *mono, size_t frames);

/* Blocking write of `frames` mono samples to the speaker. On a board with
 * the speech chain (muse_board_t.speaker_speech_chain, muse_spk_dsp.h) they
 * go through it first: the device's own sounds and replies (one chain for
 * all of them, on the voice task). */
esp_err_t muse_audio_write(const int16_t *mono, size_t frames);
/* The same for samples that already went through the board's chain or
 * never should: written as they are. The musegadgets player runs its own
 * chain instance, so a barge-in's clear hands its rewind back post-chain
 * (muse_audio_speaker_clear). The same as muse_audio_write on a board
 * without the chain. */
esp_err_t muse_audio_write_chained(const int16_t *mono, size_t frames);
/* A sound is over: on a board with the chain, the last 2 ms its look-ahead
 * still holds play now (a sound followed by silence would lose them); does
 * nothing elsewhere. */
void muse_audio_write_end(void);

/*
 * The speaker's DMA, cleared at a barge-in (muse_audio_tx.h; the barge-in
 * echo rule). A board with a speaker attaches its
 * TX channel in audio_init, after i2s_channel_init_*_mode() and before
 * i2s_channel_enable() (ESP-IDF takes the on_sent callback only then);
 * without it the clear does nothing and the DMA's ~30 ms plays out.
 */
struct i2s_channel_obj_t;
esp_err_t muse_audio_attach_tx(struct i2s_channel_obj_t *tx);
/* muse_audio_write's, around each piece it writes: how many of `frames` to write so the piece stays in one
 * DMA buffer (all of them when nothing is attached), then that they are in the DMA (silence if mute). */
size_t muse_audio_tx_note_begin(size_t frames);
void muse_audio_tx_note_write(const int16_t *mono, size_t frames, bool mute);
/*
 * Silences what the speaker's DMA holds unplayed now (faded over 4 ms, 2 ms
 * after the read position: no click, the I2S bus and the mic's clock never
 * stop) and copies the samples that were there into keep, oldest first (at
 * most max); returns how many. Writing them again plays them as if nothing
 * had happened. 0 when nothing is attached or the speaker isn't running.
 */
size_t muse_audio_speaker_clear(int16_t *keep, size_t max);

/* RMS of a buffer in dBFS (-100 for silence). */
float muse_audio_dbfs(const int16_t *mono, size_t frames);

/* RMS of a buffer mapped to a 0..1 perceptual level. */
float muse_audio_level(const int16_t *mono, size_t frames);

/* Short UI chirp: rising for "go", falling for "done". */
void muse_audio_chirp(int rising);
/* Two-note "ping" for a reply that just arrived. */
void muse_audio_notify(void);
