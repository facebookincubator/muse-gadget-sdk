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

#include "mg_session_gadget_ble.h"

#include <string.h>

#include "mg_proto.h"

static mg_proto_t proto;
static gb_ops_t gops;
static uint8_t buf[MG_SESSION_BUF_SIZE(64 * 1024)];
static mg_session_config_t scfg;
static uint8_t proof_k[MG_TOKEN_PROOF_KEY_LEN], nonce[MG_TOKEN_PROOF_NONCE_LEN];

static bool op_notify(void *ctx, mg_ch_t ch, const uint8_t *d, size_t n)
{
    (void)ctx;
    return ch != MG_CH_CONTROL || gops.notify(d, n);
}
static uint32_t op_now(void *ctx) { (void)ctx; return 0; }
static bool op_random(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    memcpy(out, nonce, n < sizeof(nonce) ? n : sizeof(nonce));
    return true;
}
static bool key_held;   /* the board's proof key: held from each connect, gone at the board's clear */
static bool op_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    (void)ctx;
    memcpy(k, proof_k, sizeof(proof_k));
    return key_held;
}
static void op_proof_clear(void *ctx)
{
    (void)ctx;
    key_held = false;
    if (gops.cleared) gops.cleared();
}
static bool op_credentials(void *ctx, mg_session_creds_t *c) { (void)ctx; return gops.credentials(c); }
static bool op_store(void *ctx, const char *a, const char *r, uint64_t at) { (void)ctx; return gops.store_tokens(a, r, at); }
static void op_state(void *ctx, uint8_t state, uint16_t epoch) { (void)ctx; gops.state(state, epoch); }
static void op_message(void *ctx) { (void)ctx; gops.message(); }
static int refused;
static void op_refused(void *ctx, const char *why, uint32_t len) { (void)ctx; (void)why; (void)len; refused++; }

void gb_connect(const gb_ops_t *ops, uint32_t max_message, uint32_t window, uint16_t mtu, const uint8_t *k,
                const uint8_t *device_nonce, const uint8_t *challenge, size_t challenge_len, const uint8_t *confirm,
                size_t confirm_len)
{
    gops = *ops;
    scfg = (mg_session_config_t){ .buf = buf, .max_message = max_message, .window = window, .out_max = 1u << 20 };
    mg_proto_config_t cfg = { .session = &scfg };
    mg_proto_ops_t pops = {
        .notify = op_notify, .now_ms = op_now, .random = op_random, .proof_key = op_proof_key,
        .proof_clear = op_proof_clear, .session_credentials = op_credentials,
        .session_store_tokens = op_store, .session_state = op_state, .session_message = op_message,
        .session_data_refused = op_refused,
    };
    /* The board inits mg_proto once (mg_ble_init); this re-inits it per
     * connection, so keep what the board keeps across connections: the
     * re-ask marks only ever count up. */
    uint32_t hellos = proto.session.reask_hellos, saved = proto.session.reask_saved;
    mg_proto_init(&proto, &cfg, &pops, NULL);
    proto.session.reask_hellos = hellos;
    proto.session.reask_saved = saved;
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    mg_proto_subscribe(&proto, MG_CH_DATA, true);
    mg_proto_set_mtu(&proto, mtu);
    key_held = true;
    if (k) {
        memcpy(proof_k, k, sizeof(proof_k));
        memcpy(nonce, device_nonce, sizeof(nonce));
        mg_proto_write(&proto, MG_CH_CONTROL, challenge, challenge_len);
        mg_proto_write(&proto, MG_CH_CONTROL, confirm, confirm_len);
    }
}

void gb_set_nonce(const uint8_t *device_nonce)
{
    memcpy(nonce, device_nonce, sizeof(nonce));   /* the device's next proof nonce */
}

void gb_disconnect(void) { mg_proto_disconnect(&proto); }
bool gb_proof_matched(void) { return mg_proto_proof_matched(&proto); }
void gb_write(const uint8_t *d, size_t n) { mg_proto_write(&proto, MG_CH_CONTROL, d, n); }
bool gb_open(void) { return mg_proto_session_open(&proto); }
bool gb_connected(void) { return proto.connected; }
uint32_t gb_id(void) { return mg_proto_session_id(&proto); }
void gb_reask_marks(uint32_t *hellos, uint32_t *saved) { mg_proto_session_reask_marks(&proto, hellos, saved); }
bool gb_send(uint32_t session, const uint8_t *msg, size_t len) { return mg_proto_session_send(&proto, session, msg, len); }
size_t gb_room(void) { return mg_proto_session_room(&proto); }
uint32_t gb_client_max(void) { return mg_proto_session_client_max(&proto); }
size_t gb_take(uint32_t session, uint8_t *out, size_t cap) { return mg_proto_session_take(&proto, session, out, cap); }
void gb_request(bool open) { mg_proto_session_request(&proto, open); }
bool gb_sealed(void) { return mg_proto_session_sealed(&proto); }
int gb_refused(void) { return refused; }

uint32_t gb_rx_credit(void)
{
    return proto.session.rx_credit;
}

void gb_set_counters(long long rx, long long tx)
{
    mg_session_test_set_counters(&proto.session, rx, tx);
}

void gb_set_mtu(uint16_t mtu)
{
    mg_proto_set_mtu(&proto, mtu);
}

void gb_nudge(void)
{
    mg_proto_session_nudge(&proto);   /* the device's keepalive (the vectors' event nudge) */
}

void gb_pump(void)
{
    mg_proto_session_pump(&proto);   /* the stack has room again (the vectors' event buffer), as mg_ble.c's TX done */
}
