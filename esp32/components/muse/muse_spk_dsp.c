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

#include "muse_spk_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "nvs.h"   /* the settings persist (>spk.dsp) */
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void muse_spk_dsp_defaults(muse_spk_dsp_cfg_t *c)
{
    c->hpf_hz = 700.0f;
    /* On the Mosaico, speech-shaped noise
     * at -20 dBFS RMS, volume 100: 64.7 dBA clean (GR 7 dB, limiter on 10 % of
     * samples, peak -1.0), and 63.2 dBA from -26 dBFS (raw 51.8; -24/3/+12
     * gave 61.5). */
    c->thr_db = -30.0f;
    c->ratio = 4.0f;
    c->makeup_db = 20.0f;
    c->attack_ms = 3.0f;
    c->release_ms = 120.0f;
    c->ceil_db = -1.0f;
    c->lim_release_ms = 50.0f;
}

static float coef(float ms, int rate)
{
    return ms > 0 ? expf(-1000.0f / (ms * (float)rate)) : 0.0f;
}

/* RBJ cookbook high-pass; Q 0.5412 and 1.3066 make a 4th-order Butterworth. */
static void hpf_biquad(float *b, double f0, double q, int rate)
{
    double w0 = 2.0 * M_PI * f0 / rate, cw = cos(w0), alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
    b[0] = (float)((1.0 + cw) / 2.0 / a0);
    b[1] = (float)(-(1.0 + cw) / a0);
    b[2] = b[0];
    b[3] = (float)(-2.0 * cw / a0);
    b[4] = (float)((1.0 - alpha) / a0);
}

/* The limiter's required gain for a sample of magnitude av. */
static inline float required(const muse_spk_dsp_t *d, float av)
{
    return av > d->ceil ? d->ceil / av : 1.0f;
}

/* The look-ahead's smallest required gain, found again, and how many entries came after it (ties: the newest). */
static void rmin_scan(muse_spk_dsp_t *d)
{
    float m = 2.0f;
    int age = 0;
    for (int k = 0; k < MUSE_SPK_DSP_LOOKAHEAD; k++) {   /* oldest (di) to newest (di - 1) */
        int j = (d->di + k) % MUSE_SPK_DSP_LOOKAHEAD;
        if (d->req[j] <= m) {
            m = d->req[j];
            age = MUSE_SPK_DSP_LOOKAHEAD - 1 - k;
        }
    }
    d->rmin = m;
    d->rmin_age = age;
}

/* Everything the settings decide; the state is left alone. */
static void tune(muse_spk_dsp_t *d, const muse_spk_dsp_cfg_t *c)
{
    d->cfg = *c;
    d->nbq = 0;
    if (c->hpf_hz > 0 && c->hpf_hz < d->rate * 0.45f) {
        hpf_biquad(d->b[0], c->hpf_hz, 0.54119610, d->rate);
        hpf_biquad(d->b[1], c->hpf_hz, 1.30656296, d->rate);
        d->nbq = 2;
    }
    d->att = coef(c->attack_ms, d->rate);
    d->rel = coef(c->release_ms, d->rate);
    d->makeup = powf(10.0f, c->makeup_db / 20.0f);
    d->slope = c->ratio > 1.0f ? 1.0f - 1.0f / c->ratio : 0.0f;
    d->ceil = powf(10.0f, (c->ceil_db > 0 ? 0 : c->ceil_db) / 20.0f);
    /* The limiter's gain gets within 0.7 % of a new peak's in the look-ahead
     * (time constant a fifth of it); the final clamp takes the rest. */
    d->latt = expf(-5.0f / MUSE_SPK_DSP_LOOKAHEAD);
    d->lrel = coef(c->lim_release_ms, d->rate);
}

void muse_spk_dsp_init(muse_spk_dsp_t *d, const muse_spk_dsp_cfg_t *c, int rate)
{
    memset(d, 0, sizeof(*d));
    d->rate = rate > 0 ? rate : 16000;
    tune(d, c);
    d->lg = 1.0f;
    d->lg_min = 1.0f;
    for (int i = 0; i < MUSE_SPK_DSP_LOOKAHEAD; i++) {
        d->req[i] = 1.0f;
    }
    d->rmin = 1.0f;
    d->rmin_age = 0;
}

void muse_spk_dsp_reset(muse_spk_dsp_t *d)
{
    muse_spk_dsp_cfg_t c = d->cfg;
    muse_spk_dsp_init(d, &c, d->rate);
}

void muse_spk_dsp_retune(muse_spk_dsp_t *d, const muse_spk_dsp_cfg_t *c)
{
    tune(d, c);
    for (int i = 0; i < MUSE_SPK_DSP_LOOKAHEAD; i++) {
        d->req[i] = required(d, fabsf(d->dl[i]));   /* (a new ceiling) */
    }
    rmin_scan(d);
}

float muse_spk_dsp_static_db(const muse_spk_dsp_cfg_t *c, float level_db)
{
    if (c->ratio <= 1.0f || level_db <= c->thr_db) {
        return 0.0f;
    }
    return -(level_db - c->thr_db) * (1.0f - 1.0f / c->ratio);
}

/* log2(x) for a normal x > 0: the exponent, and the mantissa's log by its atanh series around 1 (|t| <= 0.172,
 * error under 2e-8). No libm call per sample. */
static inline float fast_log2(float x)
{
    union {
        float f;
        uint32_t u;
    } v = { x };
    int e = (int)((v.u >> 23) & 0xff) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;   /* [1, 2) */
    float m = v.f;
    if (m > 1.41421356f) {
        m *= 0.5f;
        e++;
    }
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    return (float)e + t * (2.88539008f + t2 * (0.961796694f + t2 * (0.577078016f + t2 * 0.412198583f)));
}

/* 2^x for x in about [-120, 120]: the integer part in the exponent bits, the rest (|f| <= 0.5) by its series
 * (error under 2e-7). */
static inline float fast_exp2(float x)
{
    x = x < -120.0f ? -120.0f : x > 120.0f ? 120.0f : x;
    int i = (int)(x + 128.5f) - 128;   /* round to nearest (the argument is positive: the cast floors) */
    float f = (x - (float)i) * 0.693147181f;
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.166666667f + f * (0.0416666667f + f * (0.00833333333f +
                                                                                     f * 0.00138888889f)))));
    union {
        float f;
        uint32_t u;
    } v = { .u = (uint32_t)(i + 127) << 23 };
    return p * v.f;
}

#define DB_PER_OCTAVE 6.02059991f   /* 20 log10(2) */

void muse_spk_dsp_process(muse_spk_dsp_t *d, int16_t *pcm, size_t n)
{
    const float k = 1.0f / 32768.0f;
    const float speech = 0.00316f;   /* -50 dBFS: speech, for the average */
    const float lthr = d->cfg.thr_db / DB_PER_OCTAVE;   /* the threshold, in octaves (log2) */
    for (size_t i = 0; i < n; i++) {
        float x = (float)pcm[i] * k;
        /* 1. high-pass */
        for (int s = 0; s < d->nbq; s++) {
            const float *b = d->b[s];
            float *z = d->z[s];
            float y = b[0] * x + z[0];
            z[0] = b[1] * x - b[3] * y + z[1];
            z[1] = b[2] * x - b[4] * y;
            x = y;
        }
        /* 2. compressor: gain reduction (lvl - thr) x (1 - 1/ratio) dB over the threshold, in log2 */
        float ax = fabsf(x);
        d->env = ax > d->env ? d->att * d->env + (1.0f - d->att) * ax : d->rel * d->env + (1.0f - d->rel) * ax;
        float g = d->makeup;
        if (d->env > speech) {
            float over = fast_log2(d->env) - lthr;   /* octaves over the threshold */
            float gr = over > 0 ? over * d->slope * DB_PER_OCTAVE : 0.0f;   /* dB, >= 0 */
            if (gr > 0) {
                g *= fast_exp2(-over * d->slope);
            }
            d->comp_gr_sum += gr;
            d->n_speech++;
            if (gr > d->comp_gr_max) {
                d->comp_gr_max = gr;
            }
        }
        float v = x * g;
        /* 3. limiter: the sample leaving the look-ahead, at the smallest gain any sample still in it needs */
        float out = d->dl[d->di];
        float r = required(d, fabsf(v));
        d->dl[d->di] = v;
        d->req[d->di] = r;
        if (++d->di == MUSE_SPK_DSP_LOOKAHEAD) {
            d->di = 0;
        }
        if (r <= d->rmin) {
            d->rmin = r;
            d->rmin_age = 0;
        } else if (++d->rmin_age >= MUSE_SPK_DSP_LOOKAHEAD) {
            rmin_scan(d);   /* the smallest just left (written over): find the next (rare) */
        }
        float target = d->rmin;
        d->lg = target < d->lg ? d->latt * d->lg + (1.0f - d->latt) * target
                               : d->lrel * d->lg + (1.0f - d->lrel) * target;
        if (d->lg > 1.0f) {
            d->lg = 1.0f;
        }
        out *= d->lg;
        if (out > d->ceil) {
            out = d->ceil;
        } else if (out < -d->ceil) {
            out = -d->ceil;
        }
        if (d->lg < 0.98855309f) {   /* more than 0.1 dB */
            d->n_limit++;
        }
        if (d->lg < d->lg_min) {
            d->lg_min = d->lg;   /* (in dB at muse_spk_dsp_stats: no log per sample) */
        }
        float ao = fabsf(out);
        if (ao > d->out_peak) {
            d->out_peak = ao;
        }
        d->n++;
        float y = out * 32768.0f;
        int s = (int)(y + (y >= 0 ? 0.5f : -0.5f));   /* lroundf's rounding, without the call */
        pcm[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    /* Silence decays the filters into subnormals, slow on some FPUs: flush what can't reach a sample. */
    for (int s = 0; s < d->nbq; s++) {
        for (int j = 0; j < 2; j++) {
            if (fabsf(d->z[s][j]) < 1e-12f) {
                d->z[s][j] = 0.0f;
            }
        }
    }
    if (d->env < 1e-12f) {
        d->env = 0.0f;
    }
}

void muse_spk_dsp_stats(muse_spk_dsp_t *d, muse_spk_dsp_stats_t *o, bool reset)
{
    o->comp_gr_avg = d->n_speech ? d->comp_gr_sum / (float)d->n_speech : 0.0f;
    o->comp_gr_max = d->comp_gr_max;
    o->lim_gr_max = d->lg_min < 1.0f && d->lg_min > 0.0f ? -20.0f * log10f(d->lg_min) : 0.0f;
    o->lim_pct = d->n ? 100.0f * (float)d->n_limit / (float)d->n : 0.0f;
    o->out_peak_db = d->out_peak > 0 ? 20.0f * log10f(d->out_peak) : -120.0f;
    if (reset) {
        d->n = d->n_speech = d->n_limit = 0;
        d->comp_gr_sum = d->comp_gr_max = d->out_peak = 0.0f;
        d->lg_min = 1.0f;
    }
}

/* ---- the settings ---- */

/* The settings, their switch and generation under one spinlock: a short
 * copy each way, so the audio task never waits on a preempted setter. */
static muse_spk_dsp_cfg_t s_cfg;
static volatile bool s_cfg_init, s_enabled = true;
static uint32_t s_gen = 2;
#ifdef ESP_PLATFORM
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
#define CFG_LOCK() portENTER_CRITICAL(&s_cfg_mux)
#define CFG_UNLOCK() portEXIT_CRITICAL(&s_cfg_mux)
#else
#define CFG_LOCK() ((void)0)
#define CFG_UNLOCK() ((void)0)
#endif

bool muse_spk_dsp_settings_ok(float hpf_hz, float thr_db, float ratio, float makeup_db)
{
    return (hpf_hz == 0 || (hpf_hz >= 50 && hpf_hz <= 4000)) && thr_db >= -60 && thr_db <= 0 && ratio >= 1 &&
           ratio <= 20 && makeup_db >= 0 && makeup_db <= 30;
}

static void put_f32(uint8_t *p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(u >> (8 * i));
    }
}

static float get_f32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void muse_spk_dsp_blob_pack(const muse_spk_dsp_cfg_t *c, bool on, uint8_t out[MUSE_SPK_DSP_BLOB_LEN])
{
    memset(out, 0, MUSE_SPK_DSP_BLOB_LEN);
    out[0] = MUSE_SPK_DSP_BLOB_VER;
    out[1] = on ? 1 : 0;
    put_f32(out + 4, c->hpf_hz);
    put_f32(out + 8, c->thr_db);
    put_f32(out + 12, c->ratio);
    put_f32(out + 16, c->makeup_db);
}

bool muse_spk_dsp_blob_unpack(const uint8_t *blob, size_t len, muse_spk_dsp_cfg_t *c, bool *on)
{
    if (!blob || len != MUSE_SPK_DSP_BLOB_LEN || blob[0] != MUSE_SPK_DSP_BLOB_VER || blob[1] > 1) {
        return false;
    }
    float hpf = get_f32(blob + 4), thr = get_f32(blob + 8), ratio = get_f32(blob + 12), makeup = get_f32(blob + 16);
    if (!muse_spk_dsp_settings_ok(hpf, thr, ratio, makeup)) {   /* (a NaN fails every comparison) */
        return false;
    }
    c->hpf_hz = hpf;
    c->thr_db = thr;
    c->ratio = ratio;
    c->makeup_db = makeup;
    *on = blob[1] != 0;
    return true;
}

static void load(muse_spk_dsp_cfg_t *c, bool *on)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("muse_spk", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t blob[MUSE_SPK_DSP_BLOB_LEN + 4];
    size_t len = sizeof(blob);
    muse_spk_dsp_cfg_t got = *c;
    bool got_on = *on;
    if (nvs_get_blob(h, "dsp", blob, &len) == ESP_OK && muse_spk_dsp_blob_unpack(blob, len, &got, &got_on)) {
        *c = got;
        *on = got_on;
    }
    nvs_close(h);
#else
    (void)c;
    (void)on;
#endif
}

static void save(void)
{
#ifdef ESP_PLATFORM
    uint8_t blob[MUSE_SPK_DSP_BLOB_LEN];
    CFG_LOCK();
    muse_spk_dsp_cfg_t cfg = s_cfg;
    bool on = s_enabled;
    CFG_UNLOCK();
    muse_spk_dsp_blob_pack(&cfg, on, blob);
    nvs_handle_t h;
    if (nvs_open("muse_spk", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "dsp", blob, sizeof(blob)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
#endif
}

static void cfg_ready(void)
{
    if (!s_cfg_init) {
        muse_spk_dsp_cfg_t c;
        bool on = true;
        muse_spk_dsp_defaults(&c);
        load(&c, &on);   /* outside the lock: NVS */
        CFG_LOCK();
        if (!s_cfg_init) {
            s_cfg = c;
            s_enabled = on;
            s_cfg_init = true;
        }
        CFG_UNLOCK();
    }
}

void muse_spk_dsp_config(muse_spk_dsp_cfg_t *out, bool *enabled, uint32_t *gen)
{
    cfg_ready();
    CFG_LOCK();
    if (out) {
        *out = s_cfg;
    }
    bool on = s_enabled;
    uint32_t g = s_gen;
    CFG_UNLOCK();
    if (enabled) {
        *enabled = on;
    }
    if (gen) {
        *gen = g;
    }
}

void muse_spk_dsp_set(const muse_spk_dsp_cfg_t *c)
{
    cfg_ready();
    CFG_LOCK();
    s_cfg = *c;
    s_gen += 2;
    CFG_UNLOCK();
    save();
}

void muse_spk_dsp_enable(bool on)
{
    cfg_ready();
    CFG_LOCK();
    s_enabled = on;
    s_gen += 2;
    CFG_UNLOCK();
    save();
}

void muse_spk_chain_run(muse_spk_chain_t *c, int16_t *pcm, size_t n, int rate)
{
    muse_spk_dsp_cfg_t cfg;
    bool on;
    uint32_t gen;
    muse_spk_dsp_config(&cfg, &on, &gen);
    if (!c->made || (on && !c->on)) {
        muse_spk_dsp_init(&c->d, &cfg, rate);   /* the first, or on again: afresh */
        c->made = true;
    } else if (gen != c->gen && on) {
        muse_spk_dsp_retune(&c->d, &cfg);
    }
    c->gen = gen;
    c->on = on;
    if (on) {
        muse_spk_dsp_process(&c->d, pcm, n);
    }
}

void muse_spk_chain_reset(muse_spk_chain_t *c)
{
    if (c->made) {
        muse_spk_dsp_reset(&c->d);
    }
}

muse_spk_dsp_word_t muse_spk_dsp_word(const char *w, float v[4])
{
    if (!strcmp(w, "") || !strcmp(w, "?")) {
        return MUSE_SPK_DSP_WORD_SHOW;
    }
    if (w[0] != '=') {
        return MUSE_SPK_DSP_WORD_BAD;
    }
    w++;
    if (!strcmp(w, "on")) {
        return MUSE_SPK_DSP_WORD_ON;
    }
    if (!strcmp(w, "off")) {
        return MUSE_SPK_DSP_WORD_OFF;
    }
    /* Four numbers, commas between, nothing else. */
    const char *p = w;
    for (int i = 0; i < 4; i++) {
        char *end;
        double x = strtod(p, &end);
        if (end == p || x != x) {
            return MUSE_SPK_DSP_WORD_BAD;
        }
        v[i] = (float)x;
        p = end;
        if (i < 3) {
            if (*p != ',') {
                return MUSE_SPK_DSP_WORD_BAD;
            }
            p++;
        }
    }
    if (*p || !muse_spk_dsp_settings_ok(v[0], v[1], v[2], v[3])) {
        return MUSE_SPK_DSP_WORD_BAD;
    }
    return MUSE_SPK_DSP_WORD_SET;
}

int muse_spk_dsp_describe(char *out, size_t cap, const muse_spk_dsp_cfg_t *c, bool on)
{
    return snprintf(out, cap,
                    "@spk.dsp %s: high-pass %.0f Hz (4th-order Butterworth%s), compressor %.1f dBFS %.1f:1 attack %.0f "
                    "ms release %.0f ms, makeup %+.1f dB, limiter %.1f dBFS (2 ms look-ahead, release %.0f ms)",
                    on ? "on" : "off", (double)c->hpf_hz, c->hpf_hz > 0 ? "" : ": none", (double)c->thr_db,
                    (double)c->ratio, (double)c->attack_ms, (double)c->release_ms, (double)c->makeup_db,
                    (double)c->ceil_db, (double)c->lim_release_ms);
}

/* ---- the speaker's level (esp_codec_dev 1.5's arithmetic) ---- */

double muse_spk_vol_db(int volume, double top_db)
{
    if (volume <= 0) {
        return -96.0;
    }
    if (volume >= 100) {
        return top_db;
    }
    return top_db - 50.0 + volume * 0.5;
}

double muse_spk_hw_gain_db(double pa_v, double dac_v, double pa_gain_db)
{
    if (pa_v == 0.0) {
        pa_v = 5.0;
    }
    if (dac_v == 0.0) {
        dac_v = 3.3;
    }
    return 20.0 * log10(dac_v / pa_v) + pa_gain_db;
}

int muse_spk_dac_reg_of_db(double db)
{
    /* esp_codec_dev_vol_calc_reg over the ES8311's range: float math, truncated */
    float f = (float)db;
    if (f >= 32.0f) {
        return 0xFF;
    }
    if (f <= -95.5f) {
        return 0x00;
    }
    return (int)((f - -95.5f) * (255.0f / 127.5f));
}

double muse_spk_dac_db(int reg)
{
    return -95.5 + 0.5 * reg;
}
