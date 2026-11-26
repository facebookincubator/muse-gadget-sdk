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
 * A speech loudness chain for a small speaker (the ESP-Mosaico's, measured
 * with a UMIK-1: almost nothing below ~700 Hz, +12 dB/oct up to 1.5
 * kHz, the NS4150B clipping at DAC -5 dBFS), on every reply's way to the
 * speaker of a board with muse_board_t.speaker_speech_chain (the musegadgets
 * player's stream_audio, push-to-talk and stream_audio replies alike, and the
 * device's own sounds through muse_audio_write; muse_audio.h):
 *   1. high-pass, 4th-order Butterworth (two biquads) at hpf_hz: the 200-800
 *      Hz of speech the speaker can't make only eats the amp's headroom;
 *   2. a compressor: peak detector (attack, release), hard knee at thr_db,
 *      ratio, then makeup_db;
 *   3. a peak limiter at ceil_db: 2 ms look-ahead, instant-ish attack,
 *      lim_release_ms release, and a final clamp, so no sample ever passes
 *      the ceiling (-1 dBFS: DAC -6 dBFS at volume 100, under the amp's clip).
 * Its per-sample math is made cheap for the S31 (log2 / exp2 by polynomial, within
 * 1e-4 dB; the look-ahead's minimum kept incrementally): its output matches
 * a direct float reference within 1 LSB (tests/test_muse_spk_dsp.py).
 * Pure code, float, mono int16 in place. Latency: MUSE_SPK_DSP_LOOKAHEAD samples.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float hpf_hz;           /* 0: no high-pass */
    float thr_db;           /* compressor threshold, dBFS (sample peak re full scale) */
    float ratio;            /* >= 1 (1: no compression) */
    float makeup_db;        /* after the compressor */
    float attack_ms, release_ms;
    float ceil_db;          /* the limiter's ceiling, dBFS (<= 0) */
    float lim_release_ms;
} muse_spk_dsp_cfg_t;

/* The defaults (measured): 700 Hz, -30 dBFS, 4:1, +20 dB, 3 / 120 ms, -1 dBFS, 50 ms. */
void muse_spk_dsp_defaults(muse_spk_dsp_cfg_t *c);

#define MUSE_SPK_DSP_LOOKAHEAD 32   /* samples (2 ms at 16 kHz) */

typedef struct {
    muse_spk_dsp_cfg_t cfg;
    int rate;
    int nbq;                         /* high-pass biquads (0 or 2) */
    float b[2][5];                   /* b0 b1 b2 a1 a2 (a0 = 1) */
    float z[2][2];                   /* transposed direct form II state */
    float att, rel;                  /* compressor detector coefficients */
    float env;                       /* detector level, linear (1 = full scale) */
    float makeup;                    /* linear */
    float slope;                     /* 1 - 1/ratio (0: no compression) */
    float ceil;                      /* linear */
    float latt, lrel;                /* limiter gain coefficients */
    float lg;                        /* limiter gain */
    float dl[MUSE_SPK_DSP_LOOKAHEAD];   /* the look-ahead delay line */
    float req[MUSE_SPK_DSP_LOOKAHEAD];  /* each delayed sample's required gain */
    int di;
    float rmin;                      /* the smallest of req ... */
    int rmin_age;                    /* ... and how many entries were written after it */
    /* Statistics since the last muse_spk_dsp_stats(..., reset): */
    uint32_t n;                      /* samples */
    uint32_t n_speech;               /* samples with the detector above -50 dBFS */
    uint32_t n_limit;                /* samples the limiter reduced (by > 0.1 dB) */
    float comp_gr_sum;               /* dB, over n_speech */
    float comp_gr_max;               /* dB, positive */
    float lg_min;                    /* the limiter's smallest gain */
    float out_peak;                  /* the largest output sample, linear */
} muse_spk_dsp_t;   /* ~390 bytes */

void muse_spk_dsp_init(muse_spk_dsp_t *d, const muse_spk_dsp_cfg_t *c, int rate);
/* Starts afresh with the same settings: filters, detector, gain and look-ahead emptied (what it held is gone). */
void muse_spk_dsp_reset(muse_spk_dsp_t *d);
/* New settings, the state kept (no click: a console change mid-reply); the statistics go on. */
void muse_spk_dsp_retune(muse_spk_dsp_t *d, const muse_spk_dsp_cfg_t *c);
void muse_spk_dsp_process(muse_spk_dsp_t *d, int16_t *pcm, size_t n);

/* The compressor's static curve: its gain change (<= 0 dB) for a detector level in dBFS. */
float muse_spk_dsp_static_db(const muse_spk_dsp_cfg_t *c, float level_db);

typedef struct {
    float comp_gr_avg, comp_gr_max;  /* dB (positive: reduction); avg over speech */
    float lim_gr_max;                /* dB */
    float lim_pct;                   /* % of samples the limiter reduced */
    float out_peak_db;               /* dBFS */
} muse_spk_dsp_stats_t;
void muse_spk_dsp_stats(muse_spk_dsp_t *d, muse_spk_dsp_stats_t *out, bool reset);

/*
 * The chain's settings (>spk.dsp), shared by the console and every chain
 * that runs: set bumps a generation each chain watches to retune. On by
 * default; on/off and the >spk.dsp values persist in NVS ("muse_spk"/"dsp",
 * the blob below; RAM only on a host), read at
 * first use (muse_audio_init asks once on a board with the chain).
 */
void muse_spk_dsp_config(muse_spk_dsp_cfg_t *out, bool *enabled, uint32_t *gen);
/* The >spk.dsp values' ranges: HPF 0 (none) or 50..4000 Hz, THR -60..0 dBFS, RATIO 1..20, MAKEUP 0..30 dB. */
bool muse_spk_dsp_settings_ok(float hpf_hz, float thr_db, float ratio, float makeup_db);
void muse_spk_dsp_set(const muse_spk_dsp_cfg_t *c);
void muse_spk_dsp_enable(bool on);

/*
 * One running chain under the shared settings (the player's, muse_audio's
 * own): run makes it at its first call and follows the settings: a change
 * of values retunes it (the state kept: no click), turned off it passes the
 * audio as it comes, turned back on it starts afresh. reset empties it (what
 * its look-ahead held is gone): a stream's start, a reply the speaker
 * already went quiet on. ~440 bytes.
 */
typedef struct {
    muse_spk_dsp_t d;
    uint32_t gen;
    bool made, on;
} muse_spk_chain_t;
void muse_spk_chain_run(muse_spk_chain_t *c, int16_t *pcm, size_t n, int rate);
void muse_spk_chain_reset(muse_spk_chain_t *c);

/*
 * The NVS blob (version 1, 20 bytes): ver, on, 2 zero bytes, then hpf_hz,
 * thr_db, ratio, makeup_db as little-endian IEEE floats. unpack takes only
 * a blob of exactly that length and version, on 0 or 1, and values that
 * pass muse_spk_dsp_settings_ok; anything else returns false and leaves
 * *c and *on as they were (the defaults).
 */
#define MUSE_SPK_DSP_BLOB_VER 1
#define MUSE_SPK_DSP_BLOB_LEN 20
void muse_spk_dsp_blob_pack(const muse_spk_dsp_cfg_t *c, bool on, uint8_t out[MUSE_SPK_DSP_BLOB_LEN]);
bool muse_spk_dsp_blob_unpack(const uint8_t *blob, size_t len, muse_spk_dsp_cfg_t *c, bool *on);

/*
 * The console word, after its "spk.dsp": "" or "?" shows the settings
 * (a diagnostic); "=on" / "=off"; "=HPF,THR,RATIO,MAKEUP" sets them (and
 * turns the chain on), checked by muse_spk_dsp_settings_ok. v gets the
 * four values for SET.
 */
typedef enum { MUSE_SPK_DSP_WORD_SHOW, MUSE_SPK_DSP_WORD_ON, MUSE_SPK_DSP_WORD_OFF, MUSE_SPK_DSP_WORD_SET,
               MUSE_SPK_DSP_WORD_BAD } muse_spk_dsp_word_t;
muse_spk_dsp_word_t muse_spk_dsp_word(const char *after, float v[4]);
/* "@spk.dsp on: high-pass 700 Hz ..." (the word's answer). */
int muse_spk_dsp_describe(char *out, size_t cap, const muse_spk_dsp_cfg_t *c, bool on);

/*
 * The speaker's level, as esp_codec_dev 1.5 sets the ES8311's DAC volume
 * (register 0x32: 0x00 = -95.5 dB to 0xFF = +32 dB in 0.5 dB steps, 0xBF =
 * 0 dB): a volume 0..100 goes through the board's curve (vol_db: 0 -> -96
 * dB, mute; 1..100 -> top-50..top, 0.5 dB a step), minus the board's
 * hw_gain (20 log10(dac V / PA V) + PA gain), truncated to a register.
 */
double muse_spk_vol_db(int volume, double top_db);
double muse_spk_hw_gain_db(double pa_v, double dac_v, double pa_gain_db);
int muse_spk_dac_reg_of_db(double db);
double muse_spk_dac_db(int reg);

#ifdef __cplusplus
}
#endif
