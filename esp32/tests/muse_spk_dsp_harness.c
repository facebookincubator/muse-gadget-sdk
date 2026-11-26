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

/*
 * Host harness for muse_spk_dsp.c (tests/test_muse_spk_dsp.py): the chain's
 * ceilings, a settings change mid-signal, its settings (NVS blob, range
 * check, console word) and the speaker's volume map.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* M_PI and clock_gettime: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_spk_dsp.h"

#define RATE 16000
#define FRAME 160   /* 10 ms */

/* ---- signals ---- */

static unsigned s_rng = 1;
static double urand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return ((s_rng >> 8) + 0.5) / 16777216.0;
}

/* The torture mix: full-scale noise, a 3 kHz tone, clicks, a square wave, spikes over a hum, chirps. */
static int16_t torture_sample(long i)
{
    int seg = (int)((i / 4000) % 6);
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    double t = (double)i / RATE, v;
    switch (seg) {
    case 0: v = (int)(s_rng % 65536) - 32768; break;
    case 1: v = 32767 * sin(2 * M_PI * 3000 * t); break;
    case 2: v = (i % 400 < 4) ? 32767 : 0; break;
    case 3: v = 32767 * (i % 16 < 8 ? 1 : -1); break;
    case 4: v = ((s_rng >> 3) % 2000 == 0) ? -32768 : 300; break;
    default: v = 32767 * sin(2 * M_PI * (200 + 3000 * fmod(t, 1.0)) * t); break;
    }
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void cfg_from(muse_spk_dsp_cfg_t *c, char **a)
{
    muse_spk_dsp_defaults(c);
    c->hpf_hz = (float)atof(a[0]);
    c->thr_db = (float)atof(a[1]);
    c->ratio = (float)atof(a[2]);
    c->makeup_db = (float)atof(a[3]);
    c->ceil_db = (float)atof(a[4]);
}

static void stats_line(muse_spk_dsp_t *d)
{
    muse_spk_dsp_stats_t s;
    muse_spk_dsp_stats(d, &s, false);
    printf("stats %.3f %.3f %.3f %.3f %.3f\n", s.comp_gr_avg, s.comp_gr_max, s.lim_gr_max, s.lim_pct, s.out_peak_db);
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "torture") && argc == 8) {  /* torture SEED  HPF THR RATIO MAKEUP CEIL: loud mixed signals, the peak */
        muse_spk_dsp_cfg_t cfg;
        cfg_from(&cfg, argv + 3);
        muse_spk_dsp_t d;
        muse_spk_dsp_init(&d, &cfg, RATE);
        s_rng = (unsigned)atoi(argv[2]) * 2654435761u + 1;
        int peak = 0;
        int16_t buf[FRAME];
        for (long i = 0; i < RATE * 20; i += FRAME) {
            for (int k = 0; k < FRAME; k++) buf[k] = torture_sample(i + k);
            muse_spk_dsp_process(&d, buf, FRAME);
            for (int k = 0; k < FRAME; k++) if (abs(buf[k]) > peak) peak = abs(buf[k]);
        }
        printf("peak %d\n", peak);
        stats_line(&d);
        return 0;
    }
    if (!strcmp(c, "ceiling")) {               /* the defaults under every hostile input: the largest output sample */
        muse_spk_dsp_cfg_t cfg;
        muse_spk_dsp_defaults(&cfg);
        int worst = 0;
        const char *names[] = { "noise", "sweep", "clicks", "torture", "square", "dc_steps" };
        for (int kind = 0; kind < 6; kind++) {
            for (unsigned seed = 1; seed <= 3; seed++) {
                muse_spk_dsp_t d;
                muse_spk_dsp_init(&d, &cfg, RATE);
                s_rng = seed * 2654435761u + 7;
                int peak = 0;
                int16_t buf[FRAME];
                double ph = 0;
                for (long i = 0; i < RATE * 10; i += FRAME) {
                    for (int k = 0; k < FRAME; k++) {
                        long j = i + k;
                        double v;
                        double t = (double)j / RATE;
                        switch (kind) {
                        case 0: v = (urand() * 2 - 1) * 32768; break;                                   /* full-scale noise */
                        case 1: ph += 2 * M_PI * 50 * pow(7000.0 / 50, fmod(t, 2.0) / 2.0) / RATE; v = 32767 * sin(ph); break;
                        case 2: v = (j % (37 + seed * 11)) == 0 ? (seed & 1 ? 32767 : -32768) : 0; break;   /* clicks on silence */
                        case 3: v = torture_sample(j); break;
                        case 4: v = (j / (4 + seed)) % 2 ? 32767 : -32768; break;                        /* full-scale squares */
                        default: v = ((j / 800) % 2 ? 32767 : -32768) * (urand() > 0.5 ? 1 : 0.2); break;
                        }
                        buf[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                    muse_spk_dsp_process(&d, buf, FRAME);
                    for (int k = 0; k < FRAME; k++) if (abs(buf[k]) > peak) peak = abs(buf[k]);
                }
                printf("ceiling %s %u %d\n", names[kind], seed, peak);
                if (peak > worst) worst = peak;
            }
        }
        printf("worst %d\n", worst);
        return 0;
    }
    if (!strcmp(c, "retune")) {                /* a settings change mid-signal: no jump (state kept) */
        muse_spk_dsp_cfg_t cfg, b;
        muse_spk_dsp_defaults(&cfg);
        b = cfg;
        b.thr_db = -24;
        b.ratio = 3;
        b.makeup_db = 12;
        b.hpf_hz = 500;
        muse_spk_dsp_t d;
        muse_spk_dsp_init(&d, &cfg, RATE);
        int16_t prev = 0;
        int worst = 0, worst_at = 0;
        for (int i = 0; i < RATE; i++) {
            if (i == RATE / 2) muse_spk_dsp_retune(&d, &b);
            int16_t s = (int16_t)lround(8000 * sin(2 * M_PI * 1000 * i / RATE));
            muse_spk_dsp_process(&d, &s, 1);
            int st = abs(s - prev);
            if (i > RATE / 4 && st > worst) { worst = st; worst_at = i; }
            prev = s;
        }
        /* a 1 kHz sine's largest sample-to-sample step at the output's level: 2 sin(pi/16) x amplitude */
        printf("retune largest step %d at %d\n", worst, worst_at);
        return 0;
    }
    if (!strcmp(c, "config")) {                /* the shared settings: defaults, set, enable, the range check */
        muse_spk_dsp_cfg_t cfg;
        bool on;
        uint32_t g0, g1, g2;
        muse_spk_dsp_config(&cfg, &on, &g0);
        printf("default %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f on=%d\n", cfg.hpf_hz, cfg.thr_db, cfg.ratio, cfg.makeup_db,
               cfg.attack_ms, cfg.release_ms, cfg.ceil_db, cfg.lim_release_ms, on);
        cfg.hpf_hz = 500;
        muse_spk_dsp_set(&cfg);
        muse_spk_dsp_config(&cfg, &on, &g1);
        muse_spk_dsp_enable(false);
        muse_spk_dsp_config(NULL, &on, &g2);
        printf("set %.1f gen_changed=%d off=%d gen_changed2=%d even=%d\n", cfg.hpf_hz, g1 != g0, !on, g2 != g1,
               (g1 % 2 == 0) && (g2 % 2 == 0));
        printf("ok %d %d %d %d %d %d %d\n", muse_spk_dsp_settings_ok(700, -30, 4, 20), muse_spk_dsp_settings_ok(0, -30, 4, 20),
               muse_spk_dsp_settings_ok(40, -30, 4, 20), muse_spk_dsp_settings_ok(700, 1, 4, 20),
               muse_spk_dsp_settings_ok(700, -30, 0.5f, 20), muse_spk_dsp_settings_ok(700, -30, 4, 31),
               muse_spk_dsp_settings_ok(4000, -60, 20, 0));
        printf("ok_nan %d\n", muse_spk_dsp_settings_ok(NAN, -30, 4, 20) || muse_spk_dsp_settings_ok(700, NAN, 4, 20) ||
                                  muse_spk_dsp_settings_ok(700, -30, NAN, 20) || muse_spk_dsp_settings_ok(700, -30, 4, NAN));
        return 0;
    }
    if (!strcmp(c, "blob")) {                  /* blob HEX...: unpack each (over the defaults); "pack" packs the defaults */
        for (int i = 2; i < argc; i++) {
            muse_spk_dsp_cfg_t cfg;
            muse_spk_dsp_defaults(&cfg);
            bool on = true;
            if (!strcmp(argv[i], "pack")) {
                uint8_t b[MUSE_SPK_DSP_BLOB_LEN];
                cfg.hpf_hz = 500;
                cfg.makeup_db = 12.5f;
                muse_spk_dsp_blob_pack(&cfg, false, b);
                printf("pack ");
                for (int k = 0; k < MUSE_SPK_DSP_BLOB_LEN; k++) printf("%02x", b[k]);
                printf("\n");
                continue;
            }
            uint8_t b[64];
            size_t n = strlen(argv[i]) / 2;
            for (size_t k = 0; k < n && k < sizeof(b); k++) {
                unsigned v;
                sscanf(argv[i] + 2 * k, "%2x", &v);
                b[k] = (uint8_t)v;
            }
            bool ok = muse_spk_dsp_blob_unpack(b, n, &cfg, &on);
            printf("unpack %d %.2f %.2f %.2f %.2f on=%d\n", ok, cfg.hpf_hz, cfg.thr_db, cfg.ratio, cfg.makeup_db, on);
        }
        return 0;
    }
    if (!strcmp(c, "word")) {                  /* word ARG...: each parsed as what follows ">spk.dsp" */
        for (int i = 2; i < argc; i++) {
            float v[4] = { 0 };
            const char *a = strcmp(argv[i], "EMPTY") ? argv[i] : "";
            muse_spk_dsp_word_t w = muse_spk_dsp_word(a, v);
            static const char *kinds[] = { "show", "on", "off", "set", "bad" };
            printf("word %s %s", argv[i], kinds[w]);
            if (w == MUSE_SPK_DSP_WORD_SET) printf(" %.2f %.2f %.2f %.2f", v[0], v[1], v[2], v[3]);
            printf("\n");
        }
        muse_spk_dsp_cfg_t cfg;
        muse_spk_dsp_defaults(&cfg);
        char line[256];
        muse_spk_dsp_describe(line, sizeof(line), &cfg, true);
        printf("describe %s\n", line);
        return 0;
    }
    if (!strcmp(c, "volmap") && argc == 6) {   /* volmap TOP PA_V DAC_V PA_GAIN: volume -> curve dB -> register */
        double top = atof(argv[2]), hw = muse_spk_hw_gain_db(atof(argv[3]), atof(argv[4]), atof(argv[5]));
        printf("hw %.4f\n", hw);
        for (int v = 0; v <= 100; v++) {
            double db = muse_spk_vol_db(v, top);
            int reg = muse_spk_dac_reg_of_db(db - hw);
            printf("vol %d %.2f %d %.1f\n", v, db, reg, muse_spk_dac_db(reg));
        }
        return 0;
    }
    return 2;
}
