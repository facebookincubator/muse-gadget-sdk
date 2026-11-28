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
 * A gadget without a radio, for tools/mg_ble_client.py's tests: the
 * firmware's protocol (mg_proto.c) and device session (mg_session.c), with
 * its playback buffer (mg_play.c), behind a few functions ctypes can call.
 * Its speaker_volume starts at 70.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mg_play.h"
#include "mg_proto.h"
#include "mg_session.h"

typedef void (*fake_cb)(int ch, const uint8_t *data, int len);

static fake_cb s_notify;
static mg_proto_t s_p;
static mg_play_t s_pl;
static uint32_t s_now;
static uint8_t s_volume = 70;

static uint8_t f_vol(void *ctx)
{
    (void)ctx;
    return s_volume;
}

static void f_set_vol(void *ctx, uint8_t v)
{
    (void)ctx;
    s_volume = v;
}

static int s_assistant = -1;

static void f_assistant(void *ctx, uint8_t st)
{
    (void)ctx;
    s_assistant = st;
}

/* The token proof key Link setup left (fake_set_proof_key), and the clears asked for. */
static uint8_t s_k[MG_TOKEN_PROOF_KEY_LEN];
static bool s_have_k;
static int s_clears;
static uint32_t s_rng = 0x2545F491u;

void fake_set_proof_key(const uint8_t *k)
{
    s_have_k = k != NULL;
    if (k) {
        memcpy(s_k, k, sizeof(s_k));
    }
}

int fake_proof_clears(void)
{
    return s_clears;
}

static bool f_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    (void)ctx;
    memcpy(k, s_k, sizeof(s_k));
    return s_have_k;
}

static void f_proof_clear(void *ctx)
{
    (void)ctx;
    s_clears++;
    s_have_k = false;
}

/* Not a CSPRNG: a fake device's nonces only have to differ. */
static bool f_random(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        s_rng ^= s_rng << 13;
        s_rng ^= s_rng >> 17;
        s_rng ^= s_rng << 5;
        out[i] = (uint8_t)s_rng;
    }
    return true;
}

/* The last assistant_state the board got, or -1. */
int fake_assistant(void)
{
    return s_assistant;
}

static bool f_notify(void *ctx, mg_ch_t ch, const uint8_t *d, size_t n)
{
    (void)ctx;
    s_notify((int)ch, d, (int)n);
    return true;
}

static uint32_t f_now(void *ctx)
{
    (void)ctx;
    return s_now;
}

/* The device session (fake_session() before fake_init()): a BLE-only build's,
 * sealed after a token proof (no encrypted link here), its credentials the
 * bench's test pair. */
static bool s_with_session;
static mg_session_config_t s_scfg;
static uint8_t s_sbuf[MG_SESSION_BUF_SIZE(4096)];

void fake_session(int on)
{
    s_with_session = on != 0;
}

/* The session declares mg_session_cap_sealed_clear (before fake_init()). */
static bool s_sealed_clear;

void fake_sealed_clear(int on)
{
    s_sealed_clear = on != 0;
}

static bool f_creds(void *ctx, mg_session_creds_t *c)
{
    (void)ctx;
    if (!s_have_k) return false;
    memset(c, 0, sizeof(*c));
    strcpy(c->node_id, "homelink-a7f5b4");
    strcpy(c->access_token, "bench-access-0123");
    strcpy(c->refresh_token, "bench-refresh-0123");
    return true;
}

static void f_state(void *ctx, uint8_t state, uint16_t epoch)
{
    (void)ctx;
    (void)state;
    (void)epoch;
}

static void f_message(void *ctx)
{
    (void)ctx;
}

void fake_init(int buffer, int mtu, fake_cb notify, fake_cb nus)
{
    s_notify = notify;
    (void)nus;   /* nothing logs on the Nordic UART mirror here */
    mg_play_init(&s_pl, (uint32_t)buffer);
    /* Like the Waveshare C6: a display and a speaker, no vibration motor, no queue. */
    mg_proto_config_t cfg = { .play = &s_pl, .assistant = true };
    mg_proto_ops_t ops = { .notify = f_notify, .now_ms = f_now, .speaker_volume = f_vol,
                           .set_speaker_volume = f_set_vol, .assistant_state = f_assistant,
                           .random = f_random, .proof_key = f_proof_key, .proof_clear = f_proof_clear };
    if (s_with_session) {
        s_scfg = (mg_session_config_t){ .buf = s_sbuf, .max_message = 4096, .window = 4096, .out_max = 1u << 16,
                                        .sealed_clear = s_sealed_clear };
        cfg.session = &s_scfg;
        ops.session_credentials = f_creds;
        ops.session_state = f_state;
        ops.session_message = f_message;
    }
    mg_proto_init(&s_p, &cfg, &ops, NULL);
    mg_proto_connect(&s_p);
    mg_proto_set_mtu(&s_p, (uint16_t)mtu);
    mg_proto_subscribe(&s_p, MG_CH_CONTROL, true);
    mg_proto_subscribe(&s_p, MG_CH_DATA, true);
}

void fake_write(int ch, const uint8_t *d, int n)
{
    mg_proto_write(&s_p, (mg_ch_t)ch, d, (size_t)n);
}

/* Push-to-talk takes the audio path (claim 1) or gives it back (0). */
void fake_claim(int claim)
{
    mg_proto_claim_audio(&s_p, claim != 0);
}
