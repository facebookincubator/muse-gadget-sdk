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
 * The device session's BLE side (mg_session.c through mg_proto.c) on the
 * host, beside the exchanges of protocols/test-vectors/mg-session-v1.json
 * that mg_session_gadget_harness.cpp replays through the link code: "unit"
 * covers what the vectors don't reach, "creds" and "sealed" the two session
 * layers.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_proto.h"
#include "mg_session.h"
#include "mg_session_seal.h"
#include "mg_seal_e2e.h"
#include "mg_session_vectors.h"
#include "mg_token_proof.h"
#include "mgcommands.h"

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

/* ---- the fake BLE side and Link's config ---------------------------------- */

#define MAX_NOTES 256
typedef struct {
    mg_ch_t ch;
    size_t len;
    uint8_t d[MG_SESSION_FRAGMENT_MAX];
} note_t;

static note_t notes[MAX_NOTES];
static int nnotes;
static int notify_budget = -1;   /* >= 0: notifications the "stack" takes before it's full */

static bool f_notify(void *ctx, mg_ch_t ch, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (notify_budget == 0) {
        return false;
    }
    if (notify_budget > 0) {
        notify_budget--;
    }
    if (nnotes < MAX_NOTES && n <= sizeof(notes[0].d)) {
        notes[nnotes].ch = ch;
        notes[nnotes].len = n;
        memcpy(notes[nnotes].d, d, n);
        nnotes++;
    }
    return true;
}

static uint32_t now_ms;   /* the session's clock (layer 2's 30 s deferral) */
static uint32_t f_now(void *ctx) { (void)ctx; return now_ms; }

static uint8_t proof_k[MG_TOKEN_PROOF_KEY_LEN];
static uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN];
static int proof_clears;
static bool have_key = true;   /* the gadget holds a proof key (set up; the board's clear takes it) */
static bool f_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    (void)ctx;
    memcpy(k, proof_k, sizeof(proof_k));
    return have_key;
}
static bool set_up_marker;   /* Link setup's marker (ops.set_up): set up with no key fails closed */
static bool f_set_up(void *ctx) { (void)ctx; return have_key || set_up_marker; }
static bool holds_token;
static void f_proof_clear(void *ctx) { (void)ctx; proof_clears++; holds_token = false; have_key = false; }   /* the tokens and K go */
/* A board whose erase runs on its own task (mg_ble's): started here, its result given by mg_proto_clear_done(). */
static bool async_clear;   /* the next setup's ops.proof_clear_async */
static int async_starts;
static bool f_proof_clear_async(void *ctx) { (void)ctx; async_starts++; return true; }
static bool f_random(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    memcpy(out, device_nonce, n < sizeof(device_nonce) ? n : sizeof(device_nonce));
    return true;
}

/* The credential Link setup stored, as the glue reads it. */
static bool encrypted, store_fails;
static char access_token[MG_SESSION_TOKEN_MAX], refresh_token[MG_SESSION_TOKEN_MAX];
static const char *node_id = SV_NODE_ID, *device_key = SV_DEVICE_KEY, *key_id = SV_KEY_ID;
/* The VM the key is bound to (test_credentials_bound): none unless an
 * exchange's device has vm_binding. */
static const char *vm_id = "", *vm_kind = "";
static int stores, states, messages;
static uint8_t last_state;
static uint16_t last_epoch;

static bool resetup_busy;   /* a re-setup committing (app_gadget_session_busy) */
static bool f_busy(void *ctx) { (void)ctx; return resetup_busy; }
static int no_wifi = -1;   /* the next setup: -1 no client_refreshes op; 0/1 the gadget holds Wi-Fi / none */
static bool f_no_wifi(void *ctx) { (void)ctx; return no_wifi == 1; }
static uint64_t stored_expires;   /* the pair's access_expires_at, as the device keeps it (0: none) */
static bool f_credentials(void *ctx, mg_session_creds_t *c)
{
    (void)ctx;
    if (!holds_token) {
        return false;
    }
    snprintf(c->node_id, sizeof(c->node_id), "%s", node_id);
    snprintf(c->access_token, sizeof(c->access_token), "%s", access_token);
    snprintf(c->refresh_token, sizeof(c->refresh_token), "%s", refresh_token);
    snprintf(c->device_key, sizeof(c->device_key), "%s", device_key);
    snprintf(c->key_id, sizeof(c->key_id), "%s", key_id);
    snprintf(c->vm_id, sizeof(c->vm_id), "%s", vm_id);
    snprintf(c->vm_kind, sizeof(c->vm_kind), "%s", vm_kind);
    c->access_expires_at = stored_expires;   /* handed back as stored */
    return true;
}
static bool f_store(void *ctx, const char *a, const char *r, uint64_t at)
{
    (void)ctx;
    stores++;
    if (store_fails) {
        return false;
    }
    snprintf(access_token, sizeof(access_token), "%s", a);
    snprintf(refresh_token, sizeof(refresh_token), "%s", r);
    stored_expires = at;
    return true;
}
static void f_state(void *ctx, uint8_t state, uint16_t epoch)
{
    (void)ctx;
    states++;
    last_state = state;
    last_epoch = epoch;
}
static void f_message(void *ctx) { (void)ctx; messages++; }

static size_t hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static mg_proto_t proto;
static uint8_t session_buf[MG_SESSION_BUF_SIZE(SV_MAX_MESSAGE)];
static mg_session_config_t scfg;

static bool store_later;   /* the next setup's mg_session_config_t.store_later */
static bool client_refreshes;   /* the next setup's mg_session_config_t.client_refreshes */
static bool sealed_clear;       /* the next setup's mg_session_config_t.sealed_clear (CONFIG_MUSE_GADGET_SEALED_CLEAR) */
static bool lazy_ring;          /* the next setup: the ring taken per session (alloc/release), as mg_ble.c does */
static int ring_allocs, ring_frees;
static bool ring_alloc_fails;
static void *f_ring_alloc(size_t n)
{
    if (ring_alloc_fails) {
        return NULL;
    }
    ring_allocs++;
    return malloc(n);
}
static void f_ring_release(void *p)
{
    ring_frees++;
    free(p);
}
static uint8_t recovery;   /* the next setup's mg_proto_config_t.recovery_mode */
static bool creds_only;   /* the next setup: no VM session, the credentials-only one (a Wi-Fi gadget) */

static bool csealed;
static void setup(uint16_t mtu, bool offered, uint32_t max_message, uint32_t window, size_t out_max)
{
    bool async = async_clear;
    now_ms = 0;
    csealed = false;
    have_key = true;
    scfg = (mg_session_config_t){ .buf = lazy_ring ? NULL : session_buf, .alloc = f_ring_alloc,
                                  .release = f_ring_release, .max_message = max_message, .window = window,
                                  .out_max = out_max, .store_later = store_later,
                                  .client_refreshes = client_refreshes, .sealed_clear = sealed_clear };
    lazy_ring = false;
    ring_allocs = ring_frees = 0;
    ring_alloc_fails = false;
    store_later = client_refreshes = sealed_clear = async_clear = false;
    async_starts = 0;
    mg_proto_config_t cfg = { .session = offered && !creds_only ? &scfg : NULL, .recovery_mode = recovery,
                              .creds_session = creds_only };
    recovery = 0;
    creds_only = false;
    mg_proto_ops_t ops = {
        .notify = f_notify, .now_ms = f_now, .random = f_random,
        .proof_key = f_proof_key, .set_up = f_set_up, .proof_clear = f_proof_clear,
        .proof_clear_async = async ? f_proof_clear_async : NULL,
        .session_credentials = f_credentials, .session_store_tokens = f_store, .session_busy = f_busy,
        .client_refreshes = no_wifi >= 0 ? f_no_wifi : NULL,
        .session_state = f_state, .session_message = f_message,
    };
    mg_proto_init(&proto, &cfg, &ops, NULL);
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    mg_proto_subscribe(&proto, MG_CH_DATA, true);
    mg_proto_set_mtu(&proto, mtu);
    snprintf(access_token, sizeof(access_token), "%s", SV_ACCESS);
    snprintf(refresh_token, sizeof(refresh_token), "%s", SV_REFRESH);
    holds_token = encrypted = true;
    vm_id = vm_kind = "";
    store_fails = false;
    notify_budget = -1;
    stores = states = messages = 0;
    stored_expires = 0;
    resetup_busy = false;
    nnotes = 0;
}

/* The open session's id, as the session code above holds it. */
static uint32_t sid(void)
{
    return mg_proto_session_id(&proto);
}

static void write_bytes(const uint8_t *d, size_t n)
{
    mg_proto_write(&proto, MG_CH_CONTROL, d, n);
}

static void write_hex(const char *h)
{
    static uint8_t b[4096];
    write_bytes(b, hex(h, b, sizeof(b)));
}

/* The client nonce of the last proof (with proof_k, device_nonce and
 * node_id: the sealed session's keys, for checking what it sealed), and the
 * device -> client counter the next sealed credentials_saved must carry. */
static uint8_t cur_cn[MG_TOKEN_PROOF_NONCE_LEN];
static uint32_t dc_next;

/* The token proof of mg-token-proof-v1.json's first vector: result 1. */
static void prove(void)
{
    hex(PV_K, proof_k, sizeof(proof_k));
    hex(PV_DEVICE_NONCE, device_nonce, sizeof(device_nonce));
    {
        uint8_t ch[2 + MG_TOKEN_PROOF_NONCE_LEN];
        CHECK(hex(PV_CHALLENGE, ch, sizeof(ch)) == sizeof(ch));
        memcpy(cur_cn, ch + 2, sizeof(cur_cn));
    }
    write_hex(PV_CHALLENGE);
    write_hex(PV_CONFIRM);
    CHECK(mg_proto_proof_matched(&proto));
    nnotes = 0;
}

/* The proof on a link whose MTU is then 23 again: its 50-byte answer waits
 * whole for an MTU that carries it (never cut short), so it's
 * made at MG_MIN_ATT_MTU; the session's fragments are then cut to 23. */
static void prove_at_floor(void)
{
    uint16_t mtu = proto.mtu;
    mg_proto_set_mtu(&proto, MG_MIN_ATT_MTU);
    prove();
    mg_proto_set_mtu(&proto, mtu);
}

/* ---- what the vectors don't reach ------------------------------------------ */

static void put32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

static void hello(uint32_t max_message, uint32_t window)
{
    uint8_t m[11] = { mg_command_session, mg_session_hello, MG_SESSION_VERSION };
    put32(m + 3, max_message);
    put32(m + 7, window);
    write_bytes(m, sizeof(m));
}

static void state(uint8_t st, uint16_t epoch)
{
    uint8_t m[5] = { mg_command_session, mg_session_state, st, (uint8_t)epoch, (uint8_t)(epoch >> 8) };
    write_bytes(m, sizeof(m));
}

/* The client's side: one message from the VM, fragment by fragment, from
 * *off while fewer than upto bytes have gone. */
static void client_send_some(const uint8_t *msg, size_t len, uint16_t mtu, size_t *pos, size_t upto)
{
    size_t off = *pos;
    while (off < len && off < upto) {
        bool first = off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = mtu - 3 - hdr < len - off ? mtu - 3 - hdr : len - off;
        uint8_t f[MG_SESSION_FRAGMENT_MAX];
        f[0] = mg_command_session;
        f[1] = mg_session_data;
        f[2] = (uint8_t)((first ? mg_session_flag_first : 0) | (off + n == len ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, (uint32_t)len);
        }
        memcpy(f + hdr, msg + off, n);
        write_bytes(f, hdr + n);
        off += n;
    }
    *pos = off;
}

static void client_send(const uint8_t *msg, size_t len, uint16_t mtu)
{
    size_t off = 0;
    client_send_some(msg, len, mtu, &off, len);
}

/* Reassembles the device's data notifications (from note `from`) into msg; the bytes. */
static size_t reassemble(int from, uint8_t sub, uint8_t *msg, size_t cap, int *count)
{
    size_t len = 0;
    *count = 0;
    for (int i = from; i < nnotes; i++) {
        const note_t *nt = &notes[i];
        if (nt->len < 3 || nt->d[0] != mg_command_session || nt->d[1] != sub) {
            continue;
        }
        size_t hdr = nt->d[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        if (len + nt->len - hdr <= cap) {
            memcpy(msg + len, nt->d + hdr, nt->len - hdr);
        }
        len += nt->len - hdr;
        (*count)++;
    }
    return len;
}

static bool is_error(int i, uint16_t code)
{
    return i < nnotes && notes[i].len == 5 && notes[i].d[0] == mg_command_error && notes[i].d[1] == mg_command_session
           && notes[i].d[2] == mg_error_data_code && notes[i].d[3] == (uint8_t)code && notes[i].d[4] == code >> 8;
}

static uint32_t credit_in(int from)
{
    uint32_t total = 0;
    for (int i = from; i < nnotes; i++) {
        if (notes[i].len == 6 && notes[i].d[0] == mg_command_session && notes[i].d[1] == mg_session_credit) {
            total += notes[i].d[2] | notes[i].d[3] << 8 | notes[i].d[4] << 16 | (uint32_t)notes[i].d[5] << 24;
        }
    }
    return total;
}

/* ---- the client's sealed session (the only one) ------------------------ */

/* A sealed hello under the connection's latest proof: the client's keys from K, that proof's nonces and the
 * node id; its counters from 0, the device's data after its credentials' 0. No sealed_saved: answers to an
 * update are the one-byte credentials_saved. */
static mgseal_keys_t ckeys;
static uint32_t cctr;
static void s_hello(uint32_t max_message, uint32_t window)
{
    uint8_t m[12] = { mg_command_session, mg_session_hello, MG_SESSION_VERSION };
    put32(m + 3, max_message);
    put32(m + 7, window);
    m[11] = mg_session_hello_flag_sealed;
    write_bytes(m, sizeof(m));
    CHECK(mgseal_keys(&ckeys, proof_k, cur_cn, device_nonce, node_id, strlen(node_id)) == MGSEAL_OK);
    csealed = true;
    cctr = 0;
    dc_next = 1;
}

/* One message from the VM, sealed under the client's next counter (its sealed bytes held from *pos == 0),
 * fragment by fragment from *pos while fewer than upto of its sealed bytes have gone. */
static void s_send_some(const uint8_t *msg, size_t len, uint16_t mtu, size_t *pos, size_t upto)
{
    static uint8_t sb[SV_MAX_MESSAGE + MGSEAL_OVERHEAD + 8];
    static size_t sn;
    if (*pos == 0) {
        CHECK(len + MGSEAL_OVERHEAD <= sizeof(sb));
        memcpy(sb + 4, msg, len);
        CHECK(mgseal_seal(&ckeys, MGSEAL_DIR_CLIENT, cctr++, sb, len) == MGSEAL_OK);
        sn = len + MGSEAL_OVERHEAD;
    }
    size_t off = *pos;
    while (off < sn && off < upto) {
        bool first = off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = mtu - 3 - hdr < sn - off ? mtu - 3 - hdr : sn - off;
        uint8_t f[MG_SESSION_FRAGMENT_MAX];
        f[0] = mg_command_session;
        f[1] = mg_session_data;
        f[2] = (uint8_t)(mg_session_flag_sealed | (first ? mg_session_flag_first : 0)
                         | (off + n == sn ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, (uint32_t)sn);
        }
        memcpy(f + hdr, sb + off, n);
        write_bytes(f, hdr + n);
        off += n;
    }
    *pos = off;
}

static void s_send(const uint8_t *msg, size_t len, uint16_t mtu)
{
    size_t off = 0;
    s_send_some(msg, len, mtu, &off, SIZE_MAX);
}

/* The device's sealed messages of sub (from note `from`), opened under the counter they carry, which must be
 * the next one; the plaintext of the last into msg. Returns its length (0 on a bad tag). */
static size_t s_open(int from, uint8_t sub, uint8_t *msg, size_t cap, int *count)
{
    static uint8_t buf[SV_MAX_MESSAGE + MGSEAL_OVERHEAD + 8];
    size_t len = reassemble(from, sub, buf, sizeof(buf), count), pt = 0;
    if (len < MGSEAL_OVERHEAD || len > sizeof(buf)) {
        return 0;
    }
    uint32_t ctr = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
    if (mgseal_open(&ckeys, MGSEAL_DIR_DEVICE, ctr, buf, len, &pt) != MGSEAL_OK || pt > cap) {
        return 0;
    }
    if (sub == mg_session_data) {
        CHECK(ctr == dc_next);
        dc_next++;
    }
    memcpy(msg, buf + 4, pt);
    return pt;
}

/* A credentials_update, sealed under the client's next counter (it shares data's), in fragments at mtu. */
static void s_update(const char *pair, uint16_t mtu)
{
    static uint8_t sb[MG_SESSION_MAX_CREDENTIALS + MGSEAL_OVERHEAD + 8];
    size_t len = strlen(pair), sn = len + MGSEAL_OVERHEAD;
    memcpy(sb + 4, pair, len);
    CHECK(mgseal_seal(&ckeys, MGSEAL_DIR_CLIENT, cctr++, sb, len) == MGSEAL_OK);
    for (size_t off = 0; off < sn;) {
        bool first = off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = mtu - 3 - hdr < sn - off ? mtu - 3 - hdr : sn - off;
        uint8_t f[MG_SESSION_FRAGMENT_MAX];
        f[0] = mg_command_session;
        f[1] = mg_session_credentials_update;
        f[2] = (uint8_t)(mg_session_flag_sealed | (first ? mg_session_flag_first : 0)
                         | (off + n == sn ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, (uint32_t)sn);
        }
        memcpy(f + hdr, sb + off, n);
        write_bytes(f, hdr + n);
        off += n;
    }
}

static bool features_list_session(void)
{
    nnotes = 0;
    uint8_t rs = mg_command_request_status;
    write_bytes(&rs, 1);
    for (int i = 0; i < nnotes; i++) {
        if (notes[i].d[0] == mg_command_supported_features && notes[i].len > 1
            && notes[i].d[1] != mg_command_sub_feature) {
            return memchr(notes[i].d + 1, mg_command_session, notes[i].len - 1) != NULL;
        }
    }
    return false;
}

static void test_offered(void)
{
    setup(247, false, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    CHECK(!features_list_session());
    nnotes = 0;
    hello(65536, 65536);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_unsupported));
    CHECK(!mg_proto_session_offered(&proto));
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    CHECK(features_list_session());
    CHECK(mg_proto_session_offered(&proto));
}

static bool is_saved(int i, uint8_t value)
{
    return i < nnotes && notes[i].len == 3 && notes[i].d[0] == mg_command_session
           && notes[i].d[1] == mg_session_credentials_saved && notes[i].d[2] == value;
}

/* A sealed hello's answer (its flags byte echoed): the only hello a device answers. */
static bool is_hello(int i)
{
    return i < nnotes && notes[i].len == 12 && notes[i].d[0] == mg_command_session && notes[i].d[1] == mg_session_hello;
}

static const char PAIR[] = "{\"access_token\":\"A2\",\"refresh_token\":\"R2\"}";

static void send_update(const char *pair)
{
    uint8_t f[128] = { mg_command_session, mg_session_credentials_update, mg_session_flag_first | mg_session_flag_last };
    put32(f + 3, (uint32_t)strlen(pair));
    memcpy(f + 7, pair, strlen(pair));
    write_bytes(f, 7 + strlen(pair));
}

/* ---- the VM data machinery, in sealed sessions ---- */

#define SO MGSEAL_OVERHEAD   /* a sealed message's counter and tag: on the wire, in the window, in credit */

/* Makes m[0..n) a ServiceResponseEnvelope (client data must be one): field 1 over the rest,
 * its length a two-byte varint (n - 3 < 16384), the payload bytes unchanged from m[3] on. */
static void as_envelope(uint8_t *m, size_t n)
{
    size_t l = n - 3;
    m[0] = 0x0a;
    m[1] = (uint8_t)(0x80 | (l & 0x7f));
    m[2] = (uint8_t)(l >> 7);
}

/* A full BLE stack: credit, the credentials and fragments wait and go in order. */
static void test_full_stack(void)
{
    setup(100, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    notify_budget = 2;   /* hello and the credentials' first fragment */
    s_hello(65536, 65536);
    CHECK(nnotes == 2);
    CHECK(mg_proto_session_pump(&proto));   /* still held back */
    notify_budget = -1;
    CHECK(!mg_proto_session_pump(&proto));
    uint8_t creds[MG_SESSION_MAX_CREDENTIALS];
    int count;
    size_t n = s_open(0, mg_session_credentials, creds, sizeof(creds), &count);
    char want[MG_SESSION_MAX_CREDENTIALS];
    mg_session_creds_t c = { 0 };
    snprintf(c.node_id, sizeof(c.node_id), "%s", SV_NODE_ID);
    snprintf(c.access_token, sizeof(c.access_token), "%s", SV_ACCESS);
    snprintf(c.refresh_token, sizeof(c.refresh_token), "%s", SV_REFRESH);
    snprintf(c.device_key, sizeof(c.device_key), "%s", SV_DEVICE_KEY);
    snprintf(c.key_id, sizeof(c.key_id), "%s", SV_KEY_ID);
    size_t wn = mg_session_creds_json(&c, want, sizeof(want));
    CHECK(count > 2 && n == wn && memcmp(creds, want, wn) == 0);

    state(mg_session_state_open, 7);
    CHECK(states == 1 && last_state == mg_session_state_open && last_epoch == 7);
    CHECK(mg_proto_session_open(&proto));
    /* A 600-byte message to the VM through a stack that takes one notification at a time: it arrives whole,
     * in order, sealed (620 bytes in 7 fragments at MTU 100). */
    uint8_t big[600];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7 + 3);
    }
    nnotes = 0;
    notify_budget = 1;
    CHECK(mg_proto_session_send(&proto, sid(), big, sizeof(big)));
    for (int i = 0; i < 40 && mg_proto_session_pump(&proto); i++) {
        notify_budget = 1;
    }
    notify_budget = -1;
    uint8_t got[sizeof(big)];
    CHECK(s_open(0, mg_session_data, got, sizeof(got), &count) == sizeof(big) && memcmp(got, big, sizeof(big)) == 0
          && count == 7);

    /* A message from the VM: its credit (its sealed bytes) waits for room in the stack too. */
    uint8_t vm[300];
    memset(vm, 0x5a, sizeof(vm));
    as_envelope(vm, sizeof(vm));
    nnotes = 0;
    s_send(vm, sizeof(vm), 100);
    CHECK(messages == 1 && nnotes == 0);
    notify_budget = 0;
    uint8_t taken[SV_MAX_MESSAGE];
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == sizeof(vm) && taken[299] == 0x5a);
    CHECK(nnotes == 0 && mg_proto_session_pump(&proto));
    notify_budget = -1;
    mg_proto_session_pump(&proto);
    CHECK(credit_in(0) == sizeof(vm) + SO);
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == 0);
}

/* mg_proto_session_nudge: on an open session one [session, credit, 0] (or the credit owed, with it); none before the
 * session opens or after it closes (the keepalive). */
static void test_nudge(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    nnotes = 0;
    mg_proto_session_nudge(&proto);   /* not open yet */
    CHECK(nnotes == 0);
    state(mg_session_state_open, 1);
    nnotes = 0;
    mg_proto_session_nudge(&proto);
    CHECK(nnotes == 1 && notes[0].len == 6 && notes[0].d[0] == mg_command_session
          && notes[0].d[1] == mg_session_credit && credit_in(0) == 0);
    nnotes = 0;
    mg_proto_session_nudge(&proto);   /* again: one more, nothing held */
    CHECK(nnotes == 1 && credit_in(0) == 0);
    CHECK(!mg_proto_session_pump(&proto));
    state(mg_session_state_closed, 1);
    nnotes = 0;
    mg_proto_session_nudge(&proto);   /* closed */
    CHECK(nnotes == 0);
}


/* The window: messages the session code hasn't taken use the buffer; more than it holds is refused (and
 * credited back), counted in sealed bytes. */
static void test_window(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    uint8_t m[3000];
    memset(m, 1, sizeof(m));
    as_envelope(m, sizeof(m));
    nnotes = 0;
    s_send(m, sizeof(m), 247);
    CHECK(nnotes == 0 && messages == 1);
    s_send(m, sizeof(m), 247);   /* past the window: the second doesn't fit */
    CHECK(is_error(0, mg_error_code_invalid_value));
    /* What fit of it, the fragment that didn't, and the rest (no message open): every byte comes back. */
    CHECK(credit_in(0) == sizeof(m) + SO);
    uint8_t taken[4096];
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == sizeof(m));
    nnotes = 0;
    s_send(m, sizeof(m), 247);
    CHECK(nnotes == 0 && messages == 2);
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == sizeof(m));

    /* A message larger than the client's max_message (sealed) is never queued; nor more than out_max. */
    setup(247, true, 4096, 4096, 5000);
    encrypted = false;
    prove();
    s_hello(4096, 4096);
    state(mg_session_state_open, 1);
    static uint8_t out[8192];
    CHECK(!mg_proto_session_send(&proto, sid(), out, 4097 - SO));
    CHECK(mg_proto_session_send(&proto, sid(), out, 4096 - SO));   /* the whole window: all sent */
    CHECK(mg_proto_session_room(&proto) == 5000 - SO);              /* the queue holds sealed bytes */
    CHECK(mg_proto_session_send(&proto, sid(), out, 4096 - SO));   /* no credit left: it waits */
    CHECK(mg_proto_session_room(&proto) == 5000 - 4096 - SO);
    CHECK(!mg_proto_session_send(&proto, sid(), out, 1000));
    CHECK(!mg_proto_session_send(&proto, sid(), out, 0));
    nnotes = 0;
    uint8_t credit[6] = { mg_command_session, mg_session_credit };
    put32(credit + 2, 4096);
    write_bytes(credit, sizeof(credit));
    CHECK(mg_proto_session_room(&proto) == 5000 - SO && nnotes == 18);
}

/* A window smaller than max_message (mgcommands.h, Device session credit): once a message still arriving fills
 * the window, the device gives its bytes back, so a client message larger than the window completes. Window
 * 4096, max_message 16384, a 6 KB message sent strictly within credit. */
static void test_window_under_max(void)
{
    setup(247, true, 16384, 4096, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    static uint8_t m[6144], sb[sizeof(m) + MGSEAL_OVERHEAD];
    for (size_t i = 0; i < sizeof(m); i++) {
        m[i] = (uint8_t)(i * 7);
    }
    as_envelope(m, sizeof(m));
    memcpy(sb + 4, m, sizeof(m));
    CHECK(mgseal_seal(&ckeys, MGSEAL_DIR_CLIENT, cctr++, sb, sizeof(m)) == MGSEAL_OK);
    size_t sn = sizeof(sb), off = 0;
    uint32_t credit = 4096, back = 0;
    nnotes = 0;
    while (off < sn) {
        if (!credit) {
            uint32_t more = credit_in(0) - back;   /* what the device gave back since */
            CHECK(more > 0);
            if (!more) {
                break;   /* stuck: the deadlock */
            }
            back += more;
            credit += more;
        }
        bool first = off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = 247 - 3 - hdr;
        n = n < sn - off ? n : sn - off;
        n = n < credit ? n : credit;
        uint8_t f[MG_SESSION_FRAGMENT_MAX];
        f[0] = mg_command_session;
        f[1] = mg_session_data;
        f[2] = (uint8_t)(mg_session_flag_sealed | (first ? mg_session_flag_first : 0)
                         | (off + n == sn ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, (uint32_t)sn);
        }
        memcpy(f + hdr, sb + off, n);
        write_bytes(f, hdr + n);
        off += n;
        credit -= (uint32_t)n;
    }
    CHECK(off == sn && messages == 1 && !is_error(0, mg_error_code_invalid_value));
    static uint8_t taken[sizeof(sb)];   /* room for the sealed bytes: it opens in place */
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == sizeof(m) && !memcmp(taken, m, sizeof(m)));
    CHECK(credit_in(0) == sn);   /* every byte back once: early, then the rest at the take */
    if (!failures) {
        printf("PASS window under max_message: a 6 KB message completes through a 4 KB window\n");
    }
}

/* A client that takes max_message 4 KB, the protocol's minimum (MG_SESSION_MIN_MESSAGE):
 * a sealed message of max_message bytes arrives whole; one byte more is refused (invalid_value) and none of it
 * reaches the gadget's code, never truncated; the session goes on. Sealed credentials (at most
 * MG_SESSION_MAX_CREDENTIALS) don't go through the message ring and are saved whole at any max_message. */
static void test_small_max_message_at(uint32_t max)
{
    setup(247, true, max, max, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    static uint8_t m[8193], taken[8192];
    for (size_t i = 0; i < sizeof(m); i++) m[i] = (uint8_t)(i * 7);
    int before = messages;
    nnotes = 0;
    as_envelope(m, max - SO);
    s_send(m, max - SO, 247);
    CHECK(nnotes == 0 && messages == before + 1);
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == (size_t)max - SO && !memcmp(taken, m, max - SO));
    nnotes = 0;
    s_send(m, max - SO + 1, 247);
    CHECK(is_error(0, mg_error_code_invalid_value));
    cctr--;   /* refused at its first fragment (past max_message): its counter not used */
    CHECK(messages == before + 1);   /* nothing delivered */
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == 0);
    nnotes = 0;
    as_envelope(m, 100);
    s_send(m, 100, 247);   /* the session goes on */
    CHECK(nnotes == 0 && messages == before + 2);
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == 100 && !memcmp(taken, m, 100));
    /* The largest pair the next hello's credentials (beside the node id and the device key) can still carry,
     * sealed and fragmented at the MTU: saved whole, and the next hello's credentials carry it. One byte
     * more is refused, nothing stored, the old pair kept: stored, it would fail every later hello. */
    static mg_session_creds_t c;
    static char js[MG_SESSION_MAX_CREDENTIALS];
    CHECK(f_credentials(NULL, &c));
    snprintf(c.refresh_token, sizeof(c.refresh_token), "R9");
    c.access_expires_at = 0;
    size_t fill = MG_SESSION_MAX_CREDENTIALS;
    do {
        fill--;
        memset(c.access_token, 'a', fill);
        c.access_token[fill] = '\0';
    } while (!mg_session_creds_json(&c, js, sizeof(js)));
    static char pair[MG_SESSION_MAX_CREDENTIALS + 1];
    const char *head = "{\"access_token\":\"", *mid = "\",\"refresh_token\":\"R9\"}";
    for (int more = 1; more >= 0; more--) {
        size_t n = fill + (size_t)more;
        snprintf(pair, sizeof(pair), "%s", head);
        memset(pair + strlen(head), 'a', n);
        snprintf(pair + strlen(head) + n, sizeof(pair) - strlen(head) - n, "%s", mid);
        CHECK(strlen(pair) <= MG_SESSION_MAX_CREDENTIALS);
        char before[MG_SESSION_TOKEN_MAX];
        snprintf(before, sizeof(before), "%s", access_token);
        int stored = stores;
        nnotes = 0;
        s_update(pair, 247);
        if (more) {
            CHECK(is_error(0, mg_error_code_invalid_value) && stores == stored && !strcmp(access_token, before));
        } else {
            CHECK(stores == stored + 1 && strlen(access_token) == fill && !strcmp(refresh_token, "R9"));
        }
    }
    CHECK(f_credentials(NULL, &c) && mg_session_creds_json(&c, js, sizeof(js)));   /* the next hello's, whole */
}

static void test_small_max_message(void)
{
    test_small_max_message_at(MG_SESSION_MIN_MESSAGE);
    test_small_max_message_at(8192);
}

/* Only bytes are bounded, as the client's credit counts them (sealed bytes): any number of messages not yet
 * taken, and a message started while others are held, its rest sent as credit comes back. */
static void test_burst(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    nnotes = 0;
    for (int i = 0; i < 160; i++) {   /* 160 x 25 = 4000 sealed bytes: within the window */
        uint8_t m[5] = { 0x0a, 0x03, (uint8_t)i, (uint8_t)(i >> 8), (uint8_t)~i };   /* an envelope */
        s_send(m, sizeof(m), 247);
    }
    CHECK(nnotes == 0 && messages == 160);
    bool same = true;
    uint32_t credited = 0;
    for (int i = 0; i < 160; i++) {
        uint8_t got[32];   /* the take needs the sealed length: it opens in place */
        nnotes = 0;
        size_t n = mg_proto_session_take(&proto, sid(), got, sizeof(got));
        same = same && n == 5 && got[2] == (uint8_t)i && got[3] == (uint8_t)(i >> 8) && got[4] == (uint8_t)~i;
        credited += credit_in(0);
    }
    CHECK(same && credited == 4000);
    CHECK(mg_proto_session_take(&proto, sid(), (uint8_t[32]){ 0 }, 32) == 0);
    /* A fragment past the window is refused, and credited back. */
    static uint8_t m[4096];
    memset(m, 3, sizeof(m));
    as_envelope(m, 4000 - SO);
    s_send(m, 4000 - SO, 247);
    nnotes = 0;
    s_send(m, 200, 247);
    CHECK(nnotes == 2 && is_error(0, mg_error_code_invalid_value) && credit_in(0) == 200 + SO);
    uint8_t taken[4096];
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == 4000 - SO);
    CHECK(mg_proto_session_take(&proto, sid(), taken, sizeof(taken)) == 0);

    /* 10000 sealed bytes held and the rest of the window left: a 12000-byte message starts with what credit
     * allows and ends once the first is taken, across the ring's end. */
    setup(247, true, SV_MAX_MESSAGE, SV_MAX_MESSAGE, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    static uint8_t a[10000 - SO], b[12000 - SO], got[SV_MAX_MESSAGE];
    for (size_t i = 0; i < sizeof(b); i++) {
        b[i] = (uint8_t)(i * 13 + 1);
    }
    memset(a, 9, sizeof(a));
    as_envelope(a, sizeof(a));
    as_envelope(b, sizeof(b));
    nnotes = 0;
    s_send(a, sizeof(a), 247);
    size_t off = 0;
    s_send_some(b, sizeof(b), 247, &off, SV_MAX_MESSAGE - 10000 - 241);
    CHECK(nnotes == 0 && messages == 1 && off > 6000);
    CHECK(mg_proto_session_take(&proto, sid(), got, sizeof(got)) == sizeof(a));
    s_send_some(b, sizeof(b), 247, &off, SIZE_MAX);
    CHECK(messages == 2 && !is_error(0, mg_error_code_invalid_value));
    CHECK(mg_proto_session_take(&proto, sid(), got, sizeof(got)) == sizeof(b) && memcmp(got, b, sizeof(b)) == 0);
    CHECK(credit_in(0) == 10000 + 12000);
}

/* The session lives on the connection and the token proof's key. */
static void test_endings(void)
{
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 3);
    CHECK(mg_proto_session_open(&proto));
    /* A new proof and its hello end the open session, as state closed does (the board resets the sealed
     * session at the challenge: one closed). */
    states = 0;
    prove();
    s_hello(65536, 65536);
    CHECK(states == 1 && last_state == mg_session_state_closed && !mg_proto_session_open(&proto));
    state(mg_session_state_open, 4);
    /* The client's reason is passed on, each state once: a closed after another end too (the ref model and
     * the Zephyr side take every state). */
    states = 0;
    state(mg_session_state_no_network, 4);
    CHECK(states == 1 && last_state == mg_session_state_no_network);
    state(mg_session_state_closed, 4);
    CHECK(states == 2 && last_state == mg_session_state_closed);
    /* A failed open (mg-session-v1's failed open): connecting, then closed or no_network with no session ever
     * open, is reported, never dropped. */
    states = 0;
    state(mg_session_state_connecting, 4);
    CHECK(states == 1 && last_state == mg_session_state_connecting);
    state(mg_session_state_closed, 4);
    CHECK(states == 2 && last_state == mg_session_state_closed && !mg_proto_session_open(&proto));
    state(mg_session_state_connecting, 4);
    state(mg_session_state_no_network, 4);
    CHECK(states == 4 && last_state == mg_session_state_no_network);
    state(mg_session_state_open, 5);
    /* The connection ends it. */
    states = 0;
    mg_proto_disconnect(&proto);
    CHECK(states == 1 && last_state == mg_session_state_closed);
    mg_proto_connect(&proto);
    mg_proto_set_mtu(&proto, 247);   /* a sealed hello needs MG_MIN_ATT_MTU */
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    nnotes = 0;
    state(mg_session_state_open, 6);   /* no hello on this connection */
    CHECK(is_error(0, mg_error_code_invalid_value));
    /* The token proof's clear erases the credential: the session goes. */
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 7);
    states = 0;
    uint8_t clear[2] = { mg_command_token_proof, mg_token_proof_clear };
    write_bytes(clear, sizeof(clear));
    CHECK(proof_clears == 1 && states == 1 && !mg_proto_session_open(&proto));
    /* A new setup's commit (mg_proto_proof_reset from mg_ble_setup_changed) replaces the credential: the
     * session and its hello go with the proof. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 7);
    states = 0;
    mg_proto_proof_reset(&proto);
    CHECK(states == 1 && last_state == mg_session_state_closed && !mg_proto_session_open(&proto));
    nnotes = 0;
    state(mg_session_state_open, 8);   /* that hello no longer counts */
    CHECK(is_error(0, mg_error_code_invalid_value));
    nnotes = 0;
    s_hello(65536, 65536);             /* nor does the old proof */
    CHECK(is_error(0, mg_error_code_proof_required));
    /* With the new proof, the new credentials go out at the connection's MTU, as before the commit (not the
     * 23 of a new connection). */
    prove();
    nnotes = 0;
    s_hello(65536, 65536);
    uint8_t again[MG_SESSION_MAX_CREDENTIALS + MGSEAL_OVERHEAD];
    int frags;
    size_t got_len = reassemble(0, mg_session_credentials, again, sizeof(again), &frags);
    CHECK(got_len > 20 && frags == (int)((got_len + 4 + 3 + 243) / 244));
    /* Each session has its own id: nothing of one goes into the next. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    CHECK(sid() == 0);
    state(mg_session_state_open, 8);
    uint32_t first = sid();
    uint8_t vm[20] = { 0x0a, 0x12, 1, 2, 3 }, got[64];   /* an envelope */
    CHECK(first != 0);
    state(mg_session_state_closed, 8);
    CHECK(sid() == 0 && !mg_proto_session_send(&proto, first, vm, sizeof(vm)));
    state(mg_session_state_open, 9);
    CHECK(sid() != 0 && sid() != first);
    s_send(vm, sizeof(vm), 247);
    CHECK(!mg_proto_session_send(&proto, first, vm, sizeof(vm)));
    CHECK(mg_proto_session_take(&proto, first, got, sizeof(got)) == 0);
    CHECK(mg_proto_session_take(&proto, sid(), got, sizeof(got)) == sizeof(vm));
    CHECK(!mg_proto_session_send(&proto, 0, vm, sizeof(vm)) && mg_proto_session_send(&proto, sid(), vm, sizeof(vm)));
    /* open_request goes out ahead of anything else. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    nnotes = 0;
    mg_proto_session_request(&proto, true);
    CHECK(nnotes == 1 && notes[0].len == 3 && notes[0].d[1] == mg_session_open_request && notes[0].d[2] == 1);
}

/* mgcommands.h: a sealed message's first fragment carries at least its whole 4-byte counter.
 * A sealed hello's window under 4 is invalid_value; the device holding 1-3 bytes of credit doesn't start a sealed
 * message (it waits); a client's sealed first fragment with fewer than 4 bytes is refused (invalid_length), its
 * counter not used and its bytes credited back. */
static void test_sealed_first_fragment(void)
{
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    /* A sealed hello's window is at least 4 (mgcommands.h): 3 is invalid_value, the proof not spent. */
    nnotes = 0;
    s_hello(65536, 3);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value) && !proto.session.hello);
    s_hello(65536, 28);   /* the client's window: one 5-byte message (25 sealed), then 3 bytes of credit */
    state(mg_session_state_open, 1);
    uint8_t msg[10] = { 0x0a, 0x08, 3, 4, 5, 6, 7, 8, 9, 10 };   /* an envelope */
    CHECK(mg_proto_session_send(&proto, sid(), msg, 5));
    int count;
    uint8_t got[64];
    CHECK(s_open(0, mg_session_data, got, sizeof(got), &count) == 5);
    nnotes = 0;
    CHECK(mg_proto_session_send(&proto, sid(), msg, sizeof(msg)));
    CHECK(nnotes == 0);   /* 3 of credit: nothing starts */
    uint8_t credit[6] = { mg_command_session, mg_session_credit };
    put32(credit + 2, 1);
    write_bytes(credit, sizeof(credit));   /* 4: the first fragment, exactly the counter */
    CHECK(nnotes == 1 && notes[0].d[1] == mg_session_data && notes[0].len == MG_SESSION_FIRST_HEADER + 4
          && (notes[0].d[2] & mg_session_flag_first) && !(notes[0].d[2] & mg_session_flag_last));
    put32(credit + 2, 100);
    write_bytes(credit, sizeof(credit));
    CHECK(s_open(0, mg_session_data, got, sizeof(got), &count) == sizeof(msg) && !memcmp(got, msg, sizeof(msg))
          && count == 2);
    /* A client's sealed first fragment of 1, 2 or 3 bytes: refused (invalid_length), credited back, its
     * counter not used: the next message on that counter goes through. */
    for (size_t k = 1; k <= 3; k++) {
        uint8_t f[MG_SESSION_FIRST_HEADER + 3] = { mg_command_session, mg_session_data,
                                                    mg_session_flag_first | mg_session_flag_sealed };
        put32(f + 3, 40);
        memset(f + MG_SESSION_FIRST_HEADER, 0, k);
        nnotes = 0;
        write_bytes(f, MG_SESSION_FIRST_HEADER + k);
        CHECK(nnotes == 2 && is_error(0, mg_error_code_invalid_length) && credit_in(0) == k);
    }
    nnotes = 0;
    s_send(msg, sizeof(msg), 247);
    CHECK(nnotes == 0 && messages == 1);
    CHECK(mg_proto_session_take(&proto, sid(), got, sizeof(got)) == sizeof(msg) && !memcmp(got, msg, sizeof(msg)));
}

static void pump_all(void);

/* The sealed clear (mg_session_clear): a Forget in a sealed session. counter || pt || tag
 * under the client's next counter (pt 0 is the clear), one fragment, outside credit; bad_tag flips its last byte. */
static void s_clear(uint8_t pt, bool bad_tag)
{
    uint8_t f[MG_SESSION_FIRST_HEADER + MG_SESSION_CLEAR_SEALED] = {
        mg_command_session, mg_session_clear, mg_session_flag_first | mg_session_flag_last | mg_session_flag_sealed };
    put32(f + 3, MG_SESSION_CLEAR_SEALED);
    uint8_t *sb = f + MG_SESSION_FIRST_HEADER;
    sb[4] = pt;
    CHECK(mgseal_seal(&ckeys, MGSEAL_DIR_CLIENT, cctr++, sb, 1) == MGSEAL_OK);
    if (bad_tag) {
        sb[MG_SESSION_CLEAR_SEALED - 1] ^= 1;
    }
    write_bytes(f, sizeof(f));
}

/* Note i is [error, token_proof, data_code, code]. */
static bool is_proof_error(int i, uint16_t code)
{
    return i < nnotes && notes[i].len == 5 && notes[i].d[0] == mg_command_error
           && notes[i].d[1] == mg_command_token_proof && notes[i].d[3] == (uint8_t)code;
}

/* The sealed clear's answer in notes[from...]: sealed [v] under device counter dc_next. */
static bool is_clear_answer(int from, uint8_t v)
{
    int count;
    uint8_t got[4];
    return s_open(from, mg_session_clear, got, sizeof(got), &count) == 1 && got[0] == v && count >= 1;
}

/* The [10 FE 89 caps] request_status answers; -1 if none. */
static int advertised_caps(void)
{
    CHECK(features_list_session());
    for (int i = 0; i < nnotes; i++) {
        if (notes[i].len == 4 && notes[i].d[0] == mg_command_supported_features
            && notes[i].d[1] == mg_command_sub_feature && notes[i].d[2] == mg_command_session) {
            return notes[i].d[3];
        }
    }
    return -1;
}

/* A gadget set up with no proof key has no owner: the owner-only commands are refused; an unset one is
 * open (the recovery design); with a key, only a matched proof is the owner. */
static void test_owner_fails_closed(void)
{
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    have_key = false;
    set_up_marker = false;
    CHECK(mg_proto_owner_ok(&proto));            /* unset: open */
    set_up_marker = true;
    CHECK(!mg_proto_owner_ok(&proto));           /* set up, no key: nobody */
    have_key = true;
    set_up_marker = false;
    CHECK(!mg_proto_owner_ok(&proto));           /* a key, no proof yet */
    prove();
    CHECK(mg_proto_owner_ok(&proto));            /* the proven owner */
    set_up_marker = false;
    if (!failures) {
        printf("PASS owner fails closed: set up with no key is nobody, unset is open, a key needs its proof\n");
    }
}

static void test_sealed_clear(void)
{
    static const uint8_t TP_CLEAR[2] = { mg_command_token_proof, mg_token_proof_clear };
    /* CONFIG_MUSE_GADGET_SEALED_CLEAR on (sealed_clear): the cap bit; an unsealed token_proof clear refused in the
     * hello's order: not_found with no key, proof_required before a match, insecure after one; nothing
     * erased. */
    sealed_clear = true;
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    have_key = false;
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));
    CHECK(nnotes == 1 && is_proof_error(0, mg_error_code_not_found) && proof_clears == 0);
    have_key = true;
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));
    CHECK(nnotes == 1 && is_proof_error(0, mg_error_code_proof_required) && proof_clears == 0);
    prove();
    CHECK(advertised_caps() >= 0 && (advertised_caps() & mg_session_cap_sealed_clear));
    s_hello(65536, 65536);
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));
    CHECK(nnotes == 1 && is_proof_error(0, mg_error_code_insecure) && proof_clears == 0 && holds_token
          && proto.session.hello);
    /* Refusals, none erasing: an unsealed clear (insecure), a wrong tag (invalid_value, its counter given back),
     * a plaintext that isn't 0 (invalid_value, its counter used), a first fragment without the whole counter
     * (invalid_length). */
    uint8_t plain[8] = { mg_command_session, mg_session_clear, mg_session_flag_first | mg_session_flag_last,
                         1, 0, 0, 0, 0 };
    nnotes = 0;
    write_bytes(plain, sizeof(plain));
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure));
    nnotes = 0;
    s_clear(0, true);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
    cctr--;
    nnotes = 0;
    s_clear(1, false);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
    uint8_t shortf[10] = { mg_command_session, mg_session_clear,
                           mg_session_flag_first | mg_session_flag_sealed, MG_SESSION_CLEAR_SEALED, 0, 0, 0, 0, 0, 0 };
    nnotes = 0;
    write_bytes(shortf, sizeof(shortf));
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_length));
    CHECK(proof_clears == 0 && holds_token && proto.session.hello);
    /* The sealed clear: erased, the answer sealed [1] on the next device counter, the session ended. */
    nnotes = 0;
    s_clear(0, false);
    CHECK(proof_clears == 1 && !holds_token && !proto.session.hello && is_clear_answer(0, 1));
    nnotes = 0;
    s_update(PAIR, 247);   /* no session now */
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value) && stores == 0);

    /* Credentials a full stack still holds, none of them out, never go: the clear drops them and erases at once,
     * and its answer takes their device counter 0, the next one the client takes (mgcommands.h, Session clear;
     * mg-session-v1.json, "sealed clear while the credentials wait at MTU 247"). */
    sealed_clear = true;
    setup(100, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    notify_budget = 0;
    s_hello(65536, 65536);
    s_clear(0, false);
    CHECK(proof_clears == 1 && !proto.session.clr_pending && !proto.session.cred_out && nnotes == 0);
    notify_budget = -1;
    pump_all();
    int count;
    uint8_t creds[MG_SESSION_MAX_CREDENTIALS];
    CHECK(is_hello(0) && s_open(1, mg_session_credentials, creds, sizeof(creds), &count) == 0);
    CHECK(!proto.session.hello && nnotes == 2 && notes[1].d[1] == mg_session_clear && is_clear_answer(1, 1));
    CHECK(notes[1].len > 10 && !notes[1].d[7] && !notes[1].d[8] && !notes[1].d[9] && !notes[1].d[10]);   /* counter 0 */
    /* Forget's [1] holds: a credentials_update after the clear is taken, while its erase
     * waits for what's going out, is refused (invalid_value) and stores nothing; only the held message and the
     * clear's [1] go out, no credentials_saved. (The credentials part way out at ATT MTU 100: the erase waits
     * for them.) */
    sealed_clear = true;
    setup(100, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    notify_budget = 2;   /* the hello's answer and the credentials' first fragment */
    s_hello(65536, 65536);
    CHECK(proto.session.cred_out && proto.session.cred_out_off > 0);
    notify_budget = 0;
    s_clear(0, false);
    CHECK(proto.session.clr_pending && proof_clears == 0);
    int before = stores;
    s_update(PAIR, 100);
    notify_budget = -1;
    pump_all();
    CHECK(stores == before && proof_clears == 1 && !holds_token && !proto.session.hello);
    bool refused = false, saved = false;
    for (int i = 0; i < nnotes; i++) {
        refused = refused || is_error(i, mg_error_code_invalid_value);
        saved = saved || (notes[i].d[0] == mg_command_session && notes[i].d[1] == mg_session_credentials_saved);
    }
    CHECK(refused && !saved);
    /* Forget, then set up again: a new connection and hello, and its credentials_update is stored as ever. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    before = stores;
    nnotes = 0;
    s_update(PAIR, 247);
    CHECK(stores == before + 1);
    for (int i = 0; i < nnotes; i++) CHECK(!is_error(i, mg_error_code_invalid_value));
    /* A clear refused (a wrong tag) starts no refusal: an update in the same session is stored. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    nnotes = 0;
    s_clear(0, true);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value) && proof_clears == 0);
    cctr--;
    before = stores;
    s_update(PAIR, 247);
    CHECK(stores == before + 1 && !proto.session.clr_pending);
    /* A link that goes after the clear's tag checked, its erase waiting behind the credentials part way out:
     * still erased. */
    sealed_clear = true;
    setup(100, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    notify_budget = 2;
    s_hello(65536, 65536);
    CHECK(proto.session.cred_out && proto.session.cred_out_off > 0);
    notify_budget = 0;
    s_clear(0, false);
    CHECK(proof_clears == 0 && proto.session.clr_pending);
    notify_budget = -1;
    mg_proto_disconnect(&proto);
    CHECK(proof_clears == 1 && !holds_token);
    /* a data message part way out with the stack starved: the erase runs at once; only the answer waits
     * for the data, then goes. */
    sealed_clear = true;
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    static uint8_t big[1200];
    memset(big, 0x5c, sizeof(big));
    as_envelope(big, sizeof(big));
    notify_budget = 1;
    CHECK(mg_proto_session_send(&proto, sid(), big, sizeof(big)));
    (void)mg_proto_session_pump(&proto);
    CHECK(proto.session.out_off != 0);   /* part way out */
    notify_budget = 0;
    s_clear(0, false);
    CHECK(proof_clears == 1 && proto.session.clr_due && proto.session.hello);   /* erased; the answer waits */
    notify_budget = -1;
    nnotes = 0;
    pump_all();
    CHECK(!proto.session.hello && !proto.session.clr_due && is_clear_answer(0, 1));

    /* A release image (sealed_clear off): no cap bit, the token_proof clear as before; the sealed clear taken too. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    CHECK(advertised_caps() >= 0 && !(advertised_caps() & mg_session_cap_sealed_clear));
    s_hello(65536, 65536);
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));
    CHECK(proof_clears == 1 && !holds_token && !is_proof_error(0, mg_error_code_insecure));
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    nnotes = 0;
    s_clear(0, false);
    CHECK(proof_clears == 1 && !holds_token && is_clear_answer(0, 1));

    /* A recovery mode: the token proof refused as recovery, the sealed clear never reached. */
    sealed_clear = true;
    recovery = mg_device_mode_recovery;
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    proof_clears = 0;
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));
    CHECK(proof_clears == 0 && nnotes >= 1 && notes[nnotes - 1].d[0] == mg_command_error);
    if (!failures) {
        printf("PASS sealed clear: erased once taken, every refusal erases nothing, credentials not yet out dropped "
               "(the answer on their counter), on a lost link; token_proof clear refused where declared\n");
        printf("PASS sealed clear and updates: none stored once taken, stored after a new hello and after a refused "
               "clear\n");
    }
}

/* A board whose erase runs on its own task (mg_ble's: it waits for the setup operation gate, wipes, then restarts
 * in 500 ms): the sealed clear's answer is the erase's own result, sent when it ends ([1] erased, [0] not), and
 * nothing meanwhile; other session frames get invalid_value until then. A link lost while it runs: nothing
 * answered, the erase goes on. */
static void test_sealed_clear_async(void)
{
    for (int erased = 1; erased >= 0; erased--) {
        async_clear = true;
        setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
        encrypted = false;
        prove();
        s_hello(65536, 65536);
        nnotes = 0;
        s_clear(0, false);
        CHECK(async_starts == 1 && proof_clears == 0 && nnotes == 0 && proto.session.hello);
        uint8_t st[4] = { mg_command_session, mg_session_state, mg_session_state_open, 1 };
        write_bytes(st, sizeof(st));
        CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
        nnotes = 0;
        /* Credit still counts while the erase runs (the answer may wait for it): no error, and it's added. */
        uint32_t credit_before = proto.session.send_credit;
        uint8_t cr[6] = { mg_command_session, mg_session_credit, 0, 0, 0x10, 0 };
        write_bytes(cr, sizeof(cr));
        CHECK(nnotes == 0 && proto.session.send_credit > credit_before);
        CHECK(mg_proto_clear_owed(&proto));   /* the platform's restart waits for the answer */
        mg_proto_clear_done(&proto, erased);
        CHECK(is_clear_answer(0, (uint8_t)erased) && !proto.session.hello);
        CHECK(!mg_proto_clear_owed(&proto));   /* out: the restart may go */
        mg_proto_clear_done(&proto, erased);   /* a second result: nothing */
    }
    async_clear = true;
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    s_clear(0, false);
    CHECK(async_starts == 1);
    mg_proto_disconnect(&proto);
    nnotes = 0;
    mg_proto_clear_done(&proto, true);
    CHECK(nnotes == 0);
    if (!failures) {
        printf("PASS sealed clear async: answered from the erase's result, nothing before it, nothing after the link\n");
    }
}

/* A new hello drops every partial sealed message (mgcommands.h, Sealed data): a clear part way in under the old
 * session's keys never completes under the new one's counters, and a sealed message of the old session replayed
 * in the new one is refused, the new session's counters untouched. */
static void prove_with(const char *cnh, const char *dnh);

static void test_new_hello_drops_partials(void)
{
    static uint8_t frames[2][4][MG_SESSION_FRAGMENT_MAX];
    static size_t flen[2][4];
    int nf[2] = { 0, 0 };
    /* Seals one data message under the client's keys now, on counter ctr, into frames[which] (20-byte writes). */
#define SEAL_FRAMES(which, ctr) do { \
        uint8_t m_[8] = { 1, 2, 3, 4, 5, 6, 7, (uint8_t)(which) }, d_[8 + MGSEAL_OVERHEAD]; \
        memcpy(d_ + 4, m_, sizeof(m_)); \
        CHECK(mgseal_seal(&ckeys, MGSEAL_DIR_CLIENT, (ctr), d_, sizeof(m_)) == MGSEAL_OK); \
        size_t off_ = 0, sn_ = sizeof(d_); \
        while (off_ < sn_) { \
            bool first_ = off_ == 0; \
            size_t hdr_ = first_ ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER; \
            size_t n_ = 20 - hdr_ < sn_ - off_ ? 20 - hdr_ : sn_ - off_; \
            uint8_t *f_ = frames[which][nf[which]]; \
            f_[0] = mg_command_session; \
            f_[1] = mg_session_data; \
            f_[2] = (uint8_t)(mg_session_flag_sealed | (first_ ? mg_session_flag_first : 0) \
                              | (off_ + n_ == sn_ ? mg_session_flag_last : 0)); \
            if (first_) { \
                put32(f_ + 3, (uint32_t)sn_); \
            } \
            memcpy(f_ + hdr_, d_ + off_, n_); \
            flen[which][nf[which]++] = hdr_ + n_; \
            off_ += n_; \
        } \
    } while (0)
#define WRITE_FRAMES(which) do { for (int k_ = 0; k_ < nf[which]; k_++) write_bytes(frames[which][k_], flen[which][k_]); } while (0)
#define PUMP() do { for (int k_ = 0; k_ < 50 && mg_proto_session_pump(&proto); k_++) { } } while (0)

    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    PUMP();
    /* The old session: a sealed clear on counter 0, only its first fragment (the client may fragment it at any
     * size), and a data message on counter 1, kept to replay. */
    uint8_t sb[MG_SESSION_CLEAR_SEALED];
    sb[4] = 0;
    CHECK(mgseal_seal(&ckeys, MGSEAL_DIR_CLIENT, 0, sb, 1) == MGSEAL_OK);
    uint8_t f1[20] = { mg_command_session, mg_session_clear, mg_session_flag_first | mg_session_flag_sealed };
    put32(f1 + 3, MG_SESSION_CLEAR_SEALED);
    memcpy(f1 + 7, sb, 13);
    write_bytes(f1, sizeof(f1));
    CHECK(proto.session.clr_in && proof_clears == 0);
    SEAL_FRAMES(0, 1);

    /* A new proof (new nonces: new keys). Its challenge resets the session (proof_challenge), which drops the
     * clear today; the hello must drop it by itself too (a later path to a hello without that reset), so the
     * partial clear is put back as if it had survived the challenge. */
    uint8_t keep[MG_SESSION_CLEAR_SEALED];
    memcpy(keep, proto.session.clr, sizeof(keep));
    uint8_t got = proto.session.clr_got;
    prove_with("00112233445566778899aabbccddeeff", "ffeeddccbbaa99887766554433221100");
    CHECK(!proto.session.clr_in);
    memcpy(proto.session.clr, keep, sizeof(keep));
    proto.session.clr_got = got;
    proto.session.clr_in = true;
    s_hello(65536, 65536);
    state(mg_session_state_open, 1);
    PUMP();
    CHECK(!proto.session.clr_in && !proto.session.clr_pending && proof_clears == 0);
    /* The new session's first message, counter 0: taken once. */
    int before = messages;
    SEAL_FRAMES(1, 0);
    WRITE_FRAMES(1);
    CHECK(messages == before + 1);
    /* The old clear's last fragment: refused, nothing erased, and it gives back no counter of the new session
     * (kept, it would fail its tag under the new keys and roll counter 0 back). */
    nnotes = 0;
    uint8_t f2[3 + MG_SESSION_CLEAR_SEALED - 13] = { mg_command_session, mg_session_clear,
                                                     mg_session_flag_last | mg_session_flag_sealed };
    memcpy(f2 + 3, sb + 13, MG_SESSION_CLEAR_SEALED - 13);
    write_bytes(f2, sizeof(f2));
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value) && proof_clears == 0 && holds_token);
    /* The new session's message replayed: refused (counter 0 is used). The old session's message replayed:
     * refused too. Neither reaches the gadget's code. */
    nnotes = 0;
    WRITE_FRAMES(1);
    CHECK(messages == before + 1 && is_error(0, mg_error_code_invalid_value));
    nnotes = 0;
    WRITE_FRAMES(0);
    CHECK(messages == before + 1 && is_error(0, mg_error_code_invalid_value));
#undef SEAL_FRAMES
#undef WRITE_FRAMES
#undef PUMP
    if (!failures) {
        printf("PASS new hello drops partials: a clear part way in never completes, an old message replayed is refused\n");
    }
}

/* credentials_update, sealed: stored in one call; a failed store is credentials_saved 0. */
static void test_credentials_update(void)
{
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    const char *pair = "{\"access_token\":\"A2\",\"refresh_token\":\"R2\"}";
    store_fails = true;
    nnotes = 0;
    s_update(pair, 247);
    CHECK(stores == 1 && nnotes == 1 && is_saved(0, 0));
    CHECK(!strcmp(access_token, SV_ACCESS));
    store_fails = false;
    nnotes = 0;
    s_update(pair, 247);
    CHECK(stores == 2 && is_saved(0, 1) && !strcmp(access_token, "A2") && !strcmp(refresh_token, "R2"));
    /* No hello on this connection yet: refused. */
    mg_proto_disconnect(&proto);
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    nnotes = 0;
    s_update(pair, 247);
    CHECK(is_error(0, mg_error_code_invalid_value) && stores == 2);
}

/* store_later: the sealed pair waits for mg_proto_session_commit() (the BLE worker), the fragments having
 * come on the host task. */
static void test_store_later(void)
{
    store_later = true;
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    s_hello(65536, 65536);
    nnotes = 0;
    s_update(PAIR, 247);
    CHECK(stores == 0 && nnotes == 0);
    CHECK(mg_proto_session_commit(&proto));
    CHECK(stores == 1 && nnotes == 1 && is_saved(0, 1) && !strcmp(access_token, "A2"));
    CHECK(!mg_proto_session_commit(&proto));
    /* A second pair before the first is stored supersedes it (saved 0) once its own tag checks, at the commit. */
    nnotes = 0;
    s_update("{\"access_token\":\"A3\",\"refresh_token\":\"R3\"}", 247);
    s_update("{\"access_token\":\"A4\",\"refresh_token\":\"R4\"}", 247);
    CHECK(nnotes == 0);
    CHECK(mg_proto_session_commit(&proto));
    CHECK(stores == 2 && nnotes == 2 && is_saved(0, 0) && is_saved(1, 1) && !strcmp(access_token, "A4")
          && !strcmp(refresh_token, "R4"));
    /* A hello stores a waiting pair first (its sealed open is under the session's keys); a new proof ends the
     * session at its challenge, so a pair still waiting then goes unstored: the client sends it again. */
    s_update("{\"access_token\":\"A5\",\"refresh_token\":\"R5\"}", 247);
    prove();
    CHECK(!mg_proto_session_commit(&proto) && stores == 2);
    nnotes = 0;
    s_hello(65536, 65536);
    CHECK(stores == 2 && is_hello(0));
    int count;
    char creds[MG_SESSION_MAX_CREDENTIALS + 1] = { 0 };
    s_open(1, mg_session_credentials, (uint8_t *)creds, MG_SESSION_MAX_CREDENTIALS, &count);
    CHECK(strstr(creds, "\"access_token\":\"A4\"") != NULL);
    /* The connection's end drops a pair not yet stored. */
    s_update("{\"access_token\":\"A6\",\"refresh_token\":\"R6\"}", 247);
    mg_proto_disconnect(&proto);
    mg_proto_connect(&proto);
    CHECK(!mg_proto_session_commit(&proto) && stores == 2);
}

static void pump_all(void);
static bool nothing_in_the_clear(const char *access, const char *refresh);
static void sealed_hello(void);
static bool note_is_answer(int i, const char *want);
static void send_sealed(const char *sealed_hex, uint16_t mtu, size_t flip);
static bool is_sealed_saved(int i, bool ok, const char *refresh);

/* No plain session on any link. An unsealed hello is refused insecure whether the link is
 * encrypted or not (a relayed token proof on the relay's own bond gets nothing), builds no credentials and sends
 * no token byte; the proof's nonces stay for the sealed hello after it, which opens the session. The VM data
 * machinery the plain cases covered (window, credit, burst, fragments, endings) comes back with the sealed-data
 * vectors. */
static void test_plain_refused(void)
{
    for (int enc = 0; enc < 2; enc++) {
        setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
        encrypted = enc;
        prove();
        hello(65536, 65536);
        pump_all();
        CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure) && !proto.session.hello && !proto.session.cred_out);
        CHECK(nothing_in_the_clear(SV_ACCESS, SV_REFRESH));
        state(mg_session_state_open, 1);
        CHECK(!mg_proto_session_open(&proto));
        nnotes = 0;
        send_update(PAIR);
        CHECK(stores == 0 && nnotes == 1 && is_error(0, mg_error_code_invalid_value));
        nnotes = 0;
        sealed_hello();
        CHECK(nnotes > 1 && is_hello(0) && proto.session.hello && proto.session.sealed);
    }
}

/* Control answers wait for a full stack and go in order: the hello answer
 * ahead of the credentials, a credentials_saved, an error. */
static void test_held_answers(void)
{
    setup(100, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    encrypted = false;
    prove();
    notify_budget = 0;
    sealed_hello();
    CHECK(nnotes == 0 && mg_proto_session_pump(&proto));
    notify_budget = -1;
    CHECK(!mg_proto_session_pump(&proto));
    int count;
    uint8_t creds[MG_SESSION_MAX_CREDENTIALS];
    CHECK(is_hello(0) && reassemble(1, mg_session_credentials, creds, sizeof(creds), &count) > 0
          && count == nnotes - 1);

    nnotes = 0;
    notify_budget = 0;
    send_sealed(SE_UPD_0, 100, 0);
    uint8_t data[8] = { mg_command_session, mg_session_data, mg_session_flag_first | mg_session_flag_last, 1, 0, 0, 0, 7 };
    write_bytes(data, sizeof(data));   /* no session open: an error; the sealed saved follows the control answers */
    CHECK(nnotes == 0 && stores == 1);
    notify_budget = 1;
    CHECK(mg_proto_session_pump(&proto));
    notify_budget = -1;
    CHECK(!mg_proto_session_pump(&proto));
    CHECK(nnotes == 2 && is_error(0, mg_error_code_invalid_value) && is_sealed_saved(1, true, "R2"));

    /* An answer that finds MG_SESSION_CTL_HELD waiting is counted (the console's status
     * line), never dropped silently; the eight held go in order. */
    nnotes = 0;
    notify_budget = 0;
    for (int i = 0; i < MG_SESSION_CTL_HELD + 1; i++) {
        write_bytes(data, sizeof(data));
    }
    CHECK(nnotes == 0 && proto.session.ctl_dropped == 1);
    mg_reply_stats_t st;
    mg_proto_reply_stats(&proto, &st);
    CHECK(st.session_dropped == 1);
    notify_budget = -1;
    CHECK(!mg_proto_session_pump(&proto));
    CHECK(nnotes == MG_SESSION_CTL_HELD && is_error(0, mg_error_code_invalid_value)
          && is_error(MG_SESSION_CTL_HELD - 1, mg_error_code_invalid_value));
}

static void test_json(void)
{
    char a[MG_SESSION_TOKEN_MAX], r[MG_SESSION_TOKEN_MAX];
    for (size_t i = 0; i < sizeof(SV_JSON) / sizeof(SV_JSON[0]); i++) {
        uint64_t exp = 7;
        bool ok = mg_session_parse_update(SV_JSON[i].json, (size_t)SV_JSON[i].len, a, r, sizeof(a), &exp);
        if (ok != SV_JSON[i].ok || (ok && (strcmp(a, SV_JSON[i].access) || strcmp(r, SV_JSON[i].refresh)
                                           || exp != SV_JSON[i].exp))) {
            fprintf(stderr, "JSON case %zu: got %d (%s / %s)\n", i, ok, ok ? a : "", ok ? r : "");
            CHECK(false);
        }
    }
    /* A token that doesn't fit is refused, not cut. */
    CHECK(!mg_session_parse_update("{\"access_token\":\"abcdef\",\"refresh_token\":\"r\"}", 44, a, r, 4, NULL));
    for (size_t i = 0; i < sizeof(SV_CREDS) / sizeof(SV_CREDS[0]); i++) {
        mg_session_creds_t c = { 0 };
        snprintf(c.node_id, sizeof(c.node_id), "%s", SV_CREDS[i].node);
        snprintf(c.access_token, sizeof(c.access_token), "%s", SV_CREDS[i].access);
        snprintf(c.refresh_token, sizeof(c.refresh_token), "%s", SV_CREDS[i].refresh);
        snprintf(c.device_key, sizeof(c.device_key), "%s", SV_CREDS[i].device_key);
        snprintf(c.key_id, sizeof(c.key_id), "%s", SV_CREDS[i].key_id);
        snprintf(c.vm_id, sizeof(c.vm_id), "%s", SV_CREDS[i].vm_id);
        snprintf(c.vm_kind, sizeof(c.vm_kind), "%s", SV_CREDS[i].vm_kind);
        char out[MG_SESSION_MAX_CREDENTIALS];
        size_t n = mg_session_creds_json(&c, out, sizeof(out));
        CHECK(n == strlen(SV_CREDS[i].json) && memcmp(out, SV_CREDS[i].json, n) == 0);
        CHECK(mg_session_creds_json(&c, out, n - 1) == 0);
    }
    /* Credentials over MG_SESSION_MAX_CREDENTIALS: hello can't send them. */
    setup(247, true, SV_MAX_MESSAGE, SV_WINDOW, 1u << 20);
    prove();
    memset(access_token, 'a', 1500);
    access_token[1500] = '\0';
    memset(refresh_token, 'r', 1500);
    refresh_token[1500] = '\0';
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
}

/* ---- sealed credentials -------------------------------- */

/* Note i is exactly want (hex). */
static bool note_is(int i, const char *want)
{
    uint8_t w[64];
    size_t wn = hex(want, w, sizeof(w));
    return i < nnotes && notes[i].len == wn && !memcmp(notes[i].d, w, wn);
}

/* The client: a current one
 * (sealed hellos carry mg_session_hello_flag_sealed_saved, as every sealed_saved
 * vector assumes), or false, one that predates sealed_saved (an older iOS app, the
 * bench app: sealed alone, as one_session_per_proof's hellos are). */
static bool client_saved = true;

/* A hello of the seal vectors ([0x89, hello, ..., flags], flags sealed), as
 * this client sends it: sealed_saved added for a current client. */
static void adjust_hello(uint8_t *b, size_t n)
{
    if (client_saved && n == 12 && b[0] == mg_command_session && b[1] == mg_session_hello
        && (b[11] & mg_session_hello_flag_sealed)) {
        b[11] |= mg_session_hello_flag_sealed_saved;
    }
}

static void write_hello(const char *h)
{
    uint8_t b[64];
    size_t n = hex(h, b, sizeof(b));
    adjust_hello(b, n);
    write_bytes(b, n);
}

/* Note i is the device's answer to such a hello, exactly: the vector's, its
 * echoed flags those this client sent (sealed_saved echoed to a current client
 * only); an error answer as it is. */
static bool note_is_answer(int i, const char *want)
{
    uint8_t w[64];
    size_t wn = hex(want, w, sizeof(w));
    adjust_hello(w, wn);
    return i < nnotes && notes[i].len == wn && !memcmp(notes[i].d, w, wn);
}

/* No notification carries a token's bytes (a 12-byte run of either). */
static bool nothing_in_the_clear(const char *access, const char *refresh)
{
    for (int i = 0; i < nnotes; i++) {
        for (const char *t = access; t; t = t == access ? refresh : NULL) {
            size_t tn = strlen(t) < 12 ? strlen(t) : 12;
            for (size_t at = 0; tn && at + tn <= notes[i].len; at++) {
                if (!memcmp(notes[i].d + at, t, tn)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void sealed_hello(void)
{
    write_hello(SE_SPP_HELLO1);
    dc_next = 1;   /* a sealed session's credentials take 0 */
}

/* Note i is the sealed credentials_saved the session
 * owes, in one write: [0x89, 8, first|last|sealed, total_len 29] then the
 * reference-shaped sealed bytes, [ok || SHA-256(refresh)[0:8]] (zeros when
 * not saved) under device -> client counter dc_next (then the next). */
static bool is_sealed_saved(int i, bool ok, const char *refresh)
{
    if (i >= nnotes || notes[i].len != MG_SESSION_FIRST_HEADER + MG_SESSION_SAVED_SEALED
        || notes[i].d[0] != mg_command_session || notes[i].d[1] != mg_session_credentials_saved
        || notes[i].d[2] != (mg_session_flag_first | mg_session_flag_last | mg_session_flag_sealed)
        || (notes[i].d[3] | notes[i].d[4] << 8 | notes[i].d[5] << 16 | (uint32_t)notes[i].d[6] << 24)
               != MG_SESSION_SAVED_SEALED) {
        return false;
    }
    mgseal_keys_t k;
    uint8_t want[MG_SESSION_SAVED_SEALED];
    if (mgseal_keys(&k, proof_k, cur_cn, device_nonce, node_id, strlen(node_id)) != MGSEAL_OK
        || mgseal_saved_plaintext(want + 4, ok, (const uint8_t *)refresh, ok ? strlen(refresh) : 0) != MGSEAL_OK
        || mgseal_seal(&k, MGSEAL_DIR_DEVICE, dc_next, want, MG_SESSION_SAVED_PT) != MGSEAL_OK) {
        return false;
    }
    bool same = !memcmp(notes[i].d + MG_SESSION_FIRST_HEADER, want, sizeof(want));
    if (same) {
        dc_next++;
    } else {
        printf("  note %d: not the sealed credentials_saved [%d] with counter %u\n", i, ok, (unsigned)dc_next);
    }
    return same;
}

/* A credentials_update: the sealed bytes (hex) in fragments of at most
 * mtu - 3 bytes, with mg_session_flag_sealed; flip > 0 flips that byte from
 * the end first (the tag). */
static void send_sealed(const char *sealed_hex, uint16_t mtu, size_t flip)
{
    uint8_t msg[MG_SESSION_MAX_CREDENTIALS + 64];
    size_t len = hex(sealed_hex, msg, sizeof(msg));
    if (flip) {
        msg[len - flip] ^= 0x01;
    }
    for (size_t off = 0; off < len;) {
        bool first = off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = mtu - 3 - hdr < len - off ? mtu - 3 - hdr : len - off;
        uint8_t f[MG_SESSION_FRAGMENT_MAX];
        f[0] = mg_command_session;
        f[1] = mg_session_credentials_update;
        f[2] = (uint8_t)(mg_session_flag_sealed | (first ? mg_session_flag_first : 0)
                         | (off + n == len ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, (uint32_t)len);
        }
        memcpy(f + hdr, msg + off, n);
        write_bytes(f, hdr + n);
        off += n;
    }
}

/* A refused update: one error invalid_value, nothing stored, the pair as it was. */
static bool refused_update(const char *access_before, int stores_before)
{
    return nnotes == 1 && is_error(0, mg_error_code_invalid_value) && stores == stores_before
           && !strcmp(access_token, access_before);
}

/* The sealed credentials after a sealed hello's answer (note 0): every
 * fragment flagged sealed and within the MTU, total_len the sealed length.
 * Printed as hex for the reference to open (test tokens only). */
static void sealed_credentials(const char *label, uint16_t mtu)
{
    uint8_t creds[MG_SESSION_MAX_CREDENTIALS + 64];
    int count;
    size_t len = reassemble(1, mg_session_credentials, creds, sizeof(creds), &count);
    CHECK(count == nnotes - 1 && count > 0 && len > 20);
    bool flagged = true;
    for (int i = 1; i < nnotes; i++) {
        flagged = flagged && (notes[i].d[2] & mg_session_flag_sealed) && notes[i].len <= (size_t)mtu - 3;
    }
    CHECK(flagged);
    CHECK(nnotes > 1 && notes[1].len > 7 && (notes[1].d[3] | notes[1].d[4] << 8 | notes[1].d[5] << 16
                                             | (uint32_t)notes[1].d[6] << 24) == len);
    printf("%s ", label);
    for (size_t i = 0; i < len && i < sizeof(creds); i++) {
        printf("%02x", creds[i]);
    }
    printf("\n");
}

/* The second proof: the seal vectors' re-proof nonces, the confirm computed here. */
static void prove_again(void)
{
    uint8_t cn[MG_TOKEN_PROOF_NONCE_LEN];
    hex(SE_CN2, cn, sizeof(cn));
    memcpy(cur_cn, cn, sizeof(cur_cn));
    hex(SE_DN2, device_nonce, sizeof(device_nonce));
    uint8_t ch[2 + MG_TOKEN_PROOF_NONCE_LEN] = { mg_command_token_proof, mg_token_proof_challenge };
    memcpy(ch + 2, cn, sizeof(cn));
    write_bytes(ch, sizeof(ch));
    uint8_t cf[2 + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof, mg_token_proof_confirm };
    CHECK(mg_token_proof_mac(proof_k, false, cn, device_nonce, cf + 2));
    write_bytes(cf, sizeof(cf));
    CHECK(mg_proto_proof_matched(&proto));
}

static void test_sealed_caps(void)
{
    /* Right after the command list: [10 FE 89 caps]; sealed always,
     * client_refreshes when the gadget can't refresh its own pair. */
    for (int refreshes = 0; refreshes < 2; refreshes++) {
        client_refreshes = refreshes;
        setup(247, true, 4096, 4096, 1u << 20);
        uint8_t rs = mg_command_request_status;
        write_bytes(&rs, 1);
        int list = -1, caps = -1;
        for (int i = 0; i < nnotes; i++) {
            if (notes[i].len > 1 && notes[i].d[0] == mg_command_supported_features
                && notes[i].d[1] != mg_command_sub_feature && list < 0) {
                list = i;
            }
            if (notes[i].len == 4 && notes[i].d[0] == mg_command_supported_features
                && notes[i].d[1] == mg_command_sub_feature && notes[i].d[2] == mg_command_session) {
                caps = i;
            }
        }
        CHECK(list >= 0 && caps == list + 1);
        CHECK(caps >= 0 && notes[caps].d[3] == (refreshes ? (mg_session_cap_sealed | mg_session_cap_client_refreshes)
                                                          : mg_session_cap_sealed));
    }
    /* No session, no caps list. */
    setup(247, false, 4096, 4096, 1u << 20);
    uint8_t rs = mg_command_request_status;
    write_bytes(&rs, 1);
    for (int i = 0; i < nnotes; i++) {
        CHECK(!(notes[i].len >= 3 && notes[i].d[0] == mg_command_supported_features
                && notes[i].d[1] == mg_command_sub_feature && notes[i].d[2] == mg_command_session));
    }
}

static void test_sealed_refusals(void)
{
    static const char PROOF_REQUIRED[] = "1989010600";
    /* Before a proof: proof_required and nothing else, sealed or not. */
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    sealed_hello();
    CHECK(nnotes == 1 && note_is(0, PROOF_REQUIRED));
    /* Unsealed after a proof, on a plain link or a bonded one: insecure alone (no
     * Security Request on any gadget); the proof's nonces stay for a sealed hello. */
    prove();
    hello(4096, 4096);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure));
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
    setup(247, true, 4096, 4096, 1u << 20);   /* encrypted (a bond) */
    prove();
    hello(4096, 4096);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure));
    /* No token: not_found, and the proof isn't spent. */
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    holds_token = false;
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_not_found));
    holds_token = true;
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
    /* Bad limits: invalid_value, and the proof isn't spent. */
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    uint8_t bad[12] = { mg_command_session, mg_session_hello, MG_SESSION_VERSION + 1, 0, 0x10, 0, 0, 0, 0x10, 0, 0,
                        mg_session_hello_flag_sealed };
    write_bytes(bad, sizeof(bad));
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
    /* A recovery mode: recovery, nothing else. */
    recovery = mg_device_mode_recovery;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_recovery));
    /* An unsealed hello on an encrypted link is refused insecure (no plain session, no
     * token byte in the clear); a sealed update after it is refused, and the proof's nonces stay for the sealed
     * hello. */
    setup(247, true, 4096, 4096, 1u << 20);
    prove();
    hello(4096, 4096);
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure) && !mg_proto_session_open(&proto));
    CHECK(nothing_in_the_clear(SV_ACCESS, SV_REFRESH));
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    CHECK(refused_update(SV_ACCESS, 0));
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
}

static void test_sealed_exchange(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    /* one_session_per_proof, hello 1: the device's hello echoes the flag. */
    sealed_hello();
    CHECK(note_is_answer(0, SE_SPP_ANSWER1));
    sealed_credentials("SEALED1", 247);
    CHECK(nothing_in_the_clear(SV_ACCESS, SV_REFRESH));
    /* hello 2 under the same proof: proof_required, nothing else; the open
     * session keeps its keys. */
    nnotes = 0;
    write_hello(SE_SPP_HELLO2);
    CHECK(nnotes == 1 && note_is_answer(0, SE_SPP_ANSWER2));
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    /* Answered sealed: device -> client counter 1, after the credentials' 0. */
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R2") && stores == 1 && !strcmp(access_token, "A2")
          && !strcmp(refresh_token, "R2"));
    /* Each refusal: invalid_value, nothing stored, no credentials_saved. A
     * wrong counter is refused at the first fragment; a refused tag (the
     * reflected direction, a flipped byte) doesn't use its counter. */
    const char *const bad[][2] = {
        { SE_UPD_0, "a replayed counter" },
        { SE_UPD_2_SKIP, "a skipped counter" },
        { SE_UPD_1_REFLECT, "the reflected direction" },
        { SE_UPD_K2_0_OLDKEY, "another key's counter 0" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        nnotes = 0;
        send_sealed(bad[i][0], 247, 0);
        if (!refused_update("A2", 1)) {
            fprintf(stderr, "  %s was not refused\n", bad[i][1]);
            CHECK(false);
        }
    }
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 1);   /* a flipped tag */
    CHECK(refused_update("A2", 1));
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 17);   /* a flipped ciphertext byte */
    CHECK(refused_update("A2", 1));
    nnotes = 0;
    send_update(PAIR);   /* unsealed, in a sealed session: insecure, nothing changes (mgcommands.h, Sealed data) */
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure) && stores == 1 && !strcmp(access_token, "A2"));
    /* The next counter is still 1: refused tags gave theirs back. */
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 0);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R3") && stores == 2 && !strcmp(access_token, "A3"));
    /* A sealed non-object: invalid_value, the pair unchanged. */
    nnotes = 0;
    send_sealed(SE_UPD_2_NOTOBJ, 247, 0);
    CHECK(refused_update("A3", 2));
    /* Fragmented at ATT MTU 23. */
    nnotes = 0;
    mg_proto_set_mtu(&proto, 23);
    send_sealed(SE_UPD_3, 23, 0);
    mg_proto_set_mtu(&proto, 247);
    /* At ATT MTU 23 the answer takes two writes. */
    {
        uint8_t got[MG_SESSION_SAVED_SEALED + 8];
        int count;
        size_t n = reassemble(0, mg_session_credentials_saved, got, sizeof(got), &count);
        CHECK(count == 2 && nnotes == 2 && n == MG_SESSION_SAVED_SEALED && notes[0].len <= 20 && notes[1].len <= 20);
        /* the same bytes in one write, as the check reads them */
        notes[0].len = MG_SESSION_FIRST_HEADER + MG_SESSION_SAVED_SEALED;
        notes[0].d[2] = mg_session_flag_first | mg_session_flag_last | mg_session_flag_sealed;
        memcpy(notes[0].d + MG_SESSION_FIRST_HEADER, got, n);
        nnotes = 1;
    }
    CHECK(is_sealed_saved(0, true, "R5") && stores == 3 && !strcmp(access_token, "A5") && !strcmp(refresh_token, "R5"));
    /* A re-proof: its challenge ends the sealed session (a sealed update now
     * has no hello), its match keys one new sealed session. */
    nnotes = 0;
    prove_again();
    nnotes = 0;
    send_sealed(SE_UPD_K2_0, 247, 0);
    CHECK(refused_update("A5", 3));
    nnotes = 0;
    write_hello(SE_SPP_HELLO3);
    dc_next = 1;
    CHECK(note_is_answer(0, SE_SPP_ANSWER3));
    sealed_credentials("SEALED2", 247);
    CHECK(nothing_in_the_clear("A5\"", "R5\""));
    nnotes = 0;
    send_sealed(SE_UPD_K2_0, 247, 0);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R4") && stores == 4 && !strcmp(access_token, "A4"));
    /* access_expires_at and another unknown member ignored; a missing token refused. */
    nnotes = 0;
    send_sealed(SE_UPD_K2_1_EXP, 247, 0);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R7") && stores == 5 && !strcmp(access_token, "A7")
          && !strcmp(refresh_token, "R7"));
    nnotes = 0;
    send_sealed(SE_UPD_K2_2_NOREFRESH, 247, 0);
    CHECK(refused_update("A7", 5));
    /* The connection's end: a new connection needs a new proof. */
    mg_proto_disconnect(&proto);
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    mg_proto_set_mtu(&proto, 247);
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_proof_required));
    nnotes = 0;
    send_sealed(SE_UPD_K2_0, 247, 0);
    CHECK(refused_update("A7", 5));

    /* At ATT MTU 23 a sealed hello gets invalid_value (the sealed session needs
     * MG_MIN_ATT_MTU) and takes nothing; the same proof's nonces at the floor, 100: every fragment fits, the same
     * bytes. */
    setup(23, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove_at_floor();
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
    setup(MG_MIN_ATT_MTU, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove_at_floor();
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
    sealed_credentials("SEALED_MTU100", MG_MIN_ATT_MTU);
    CHECK(nothing_in_the_clear(SV_ACCESS, SV_REFRESH));
}

/* store_later (the BLE builds): a sealed update waits whole for
 * mg_proto_session_commit() on the worker, which opens it there. */
static void test_sealed_store_later(void)
{
    store_later = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    CHECK(note_is_answer(0, SE_SPP_ANSWER1));
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    CHECK(nnotes == 0 && stores == 0);
    CHECK(mg_proto_session_commit(&proto));
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R2") && stores == 1 && !strcmp(access_token, "A2"));
    /* A wrong counter is refused at the first fragment, on the host task:
     * nothing waits for the commit. */
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);   /* replayed */
    CHECK(refused_update("A2", 1));
    CHECK(!mg_proto_session_commit(&proto));
    /* A tag is checked at the commit: nothing stored, no saved, and its
     * counter given back. */
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 1);   /* flipped tag */
    CHECK(nnotes == 0);
    CHECK(mg_proto_session_commit(&proto) && refused_update("A2", 1));
    /* The counter is still 1; a hello stores (opens) a waiting one first. */
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 0);
    write_hello(SE_SPP_HELLO2);
    CHECK(stores == 2 && !strcmp(access_token, "A3") && is_sealed_saved(0, true, "R3") && note_is_answer(1, SE_SPP_ANSWER2));
    /* The connection's end drops one not yet stored. */
    send_sealed(SE_UPD_2_NOTOBJ, 247, 0);
    mg_proto_disconnect(&proto);
    mg_proto_connect(&proto);
    CHECK(!mg_proto_session_commit(&proto) && stores == 2);
    /* A second update before the first is stored supersedes it: sealed [0]
     * for the first (counter 1), then sealed [1] for the second (counter 2,
     * the hash of its refresh_token) after its commit. */
    store_later = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    CHECK(mg_proto_session_commit(&proto) && nnotes == 1 && is_sealed_saved(0, true, "R2"));
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 0);
    send_sealed(SE_UPD_2_SKIP, 247, 0);   /* sealed under counter 2: the next one now */
    CHECK(mg_proto_session_commit(&proto));
    CHECK(nnotes == 2 && is_sealed_saved(0, false, "") && is_sealed_saved(1, true, "RX") && !strcmp(access_token, "AX"));
    /* A replacement whose tag doesn't check supersedes nothing: refused, and the update waiting is stored as
     * if it had come alone. */
    store_later = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    send_sealed(SE_UPD_1, 247, 1);   /* flipped tag */
    CHECK(nnotes == 0 && stores == 0);
    CHECK(mg_proto_session_commit(&proto));
    CHECK(nnotes == 2 && is_error(0, mg_error_code_invalid_value) && is_sealed_saved(1, true, "R2") && stores == 1
          && !strcmp(access_token, "A2"));
}

/* ---- the credentials-only device session (Wi-Fi gadgets; layer 2) ---------- */

static void creds_setup(uint16_t mtu)
{
    creds_only = true;
    setup(mtu, true, 4096, 4096, 1u << 20);
    encrypted = false;
}

/* Layer 2 sends what it owes from the worker's pump. */
/* The credentials-only session (layer 2) built with MGDS_CAP_SEALED_CLEAR (CONFIG_MUSE_GADGET_SEALED_CLEAR): the
 * cap bit, an unsealed token_proof clear refused insecure with nothing erased. */
static void test_creds_sealed_clear(void)
{
    creds_setup(247);
    CHECK(features_list_session());
    bool bit = false;
    for (int i = 0; i < nnotes; i++) {
        bit = bit || (notes[i].len == 4 && notes[i].d[0] == mg_command_supported_features
                      && notes[i].d[1] == mg_command_sub_feature && notes[i].d[2] == mg_command_session
                      && (notes[i].d[3] & mg_session_cap_sealed_clear));
    }
    CHECK(bit);
    static const uint8_t TP_CLEAR[2] = { mg_command_token_proof, mg_token_proof_clear };
    proof_clears = 0;
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));   /* before a match: proof_required */
    CHECK(nnotes == 1 && notes[0].d[1] == mg_command_token_proof && notes[0].d[3] == mg_error_code_proof_required);
    prove();
    nnotes = 0;
    write_bytes(TP_CLEAR, sizeof(TP_CLEAR));
    CHECK(nnotes == 1 && notes[0].d[0] == mg_command_error && notes[0].d[1] == mg_command_token_proof
          && notes[0].d[3] == mg_error_code_insecure && proof_clears == 0 && holds_token);
    /* A board whose erase runs on its own task: the sealed clear's [1] or [0] is the erase's result, nothing
     * before it. */
    for (int erased = 1; erased >= 0; erased--) {
        async_clear = true;
        creds_setup(247);
        prove();
        s_hello(4096, 4096);
        for (int i = 0; i < 50 && mg_proto_session_pump(&proto); i++) {
        }
        nnotes = 0;
        s_clear(0, false);
        for (int i = 0; i < 50 && mg_proto_session_pump(&proto); i++) {
        }
        CHECK(async_starts == 1 && nnotes == 0);
        mg_proto_clear_done(&proto, erased);
        for (int i = 0; i < 50 && mg_proto_session_pump(&proto); i++) {
        }
        CHECK(is_clear_answer(0, (uint8_t)erased));
    }
    if (!failures) {
        printf("PASS creds sealed clear: declared, the unsealed token_proof clear refused, the answer from the erase\n");
    }
}

/* Pump while something is held back (a deferred hello): it stays pending. */
static void pump_all_busy(void)
{
    for (int i = 0; i < 20; i++) {
        (void)mg_proto_session_pump(&proto);
    }
}

static void pump_all(void)
{
    for (int i = 0; i < 200 && mg_proto_session_pump(&proto); i++) {
    }
    CHECK(!mg_proto_session_pump(&proto));
}

static void test_creds_session(void)
{
    /* Caps: 0x89 listed, then [10 FE 89 01] right after the command list:
     * sealed only (a Wi-Fi gadget refreshes its own pair). */
    creds_setup(247);
    CHECK(mg_proto_session_offered(&proto));
    CHECK(features_list_session());
    int list = -1, caps = -1;
    for (int i = 0; i < nnotes; i++) {
        if (notes[i].len > 1 && notes[i].d[0] == mg_command_supported_features
            && notes[i].d[1] != mg_command_sub_feature && list < 0) {
            list = i;
        }
        if (notes[i].len == 4 && notes[i].d[0] == mg_command_supported_features
            && notes[i].d[1] == mg_command_sub_feature && notes[i].d[2] == mg_command_session) {
            caps = i;
        }
    }
    /* The credentials-only session lists credentials_only (no VM session); the
     * BLE-only VM session never does (test_sealed_caps). */
    CHECK(list >= 0 && caps == list + 1 && caps >= 0
          && notes[caps].d[3] == (mg_session_cap_sealed | mg_session_cap_credentials_only));

    /* Refusals: before a proof, an unsealed hello on a plain link, no token. */
    creds_setup(247);
    sealed_hello();
    pump_all();
    CHECK(nnotes == 1 && note_is_answer(0, SE_SPP_ANSWER2));   /* proof_required */
    prove();
    hello(4096, 4096);
    pump_all();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure));
    nnotes = 0;
    holds_token = false;
    sealed_hello();
    pump_all();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_not_found));
    holds_token = true;
    recovery = mg_device_mode_recovery;
    creds_setup(247);
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_recovery));

    /* one_session_per_proof, sealed credentials, updates. */
    creds_setup(247);
    prove();
    sealed_hello();
    pump_all();
    CHECK(note_is_answer(0, SE_SPP_ANSWER1));
    sealed_credentials("CREDS_SEALED1", 247);
    CHECK(nothing_in_the_clear(SV_ACCESS, SV_REFRESH));
    CHECK(proto.creds_cache == NULL);   /* the credential goes once it's sent */
    nnotes = 0;
    write_hello(SE_SPP_HELLO2);
    pump_all();
    CHECK(nnotes == 1 && note_is_answer(0, SE_SPP_ANSWER2));
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    pump_all();
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R2") && stores == 1 && !strcmp(access_token, "A2")
          && !strcmp(refresh_token, "R2"));
    const char *const bad[] = { SE_UPD_0, SE_UPD_2_SKIP, SE_UPD_1_REFLECT, SE_UPD_K2_0_OLDKEY };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        nnotes = 0;
        send_sealed(bad[i], 247, 0);
        pump_all();
        CHECK(refused_update("A2", 1));
    }
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 1);   /* a flipped tag */
    pump_all();
    CHECK(refused_update("A2", 1));
    nnotes = 0;
    send_update(PAIR);   /* unsealed in a sealed session: insecure, nothing changes (sealed data) */
    pump_all();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure) && stores == 1 && !strcmp(access_token, "A2"));
    nnotes = 0;
    send_sealed(SE_UPD_1, 23, 0);   /* the next counter, at ATT MTU 23's fragments */
    pump_all();
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R3") && stores == 2 && !strcmp(access_token, "A3"));
    nnotes = 0;
    send_sealed(SE_UPD_2_NOTOBJ, 247, 0);
    pump_all();
    CHECK(refused_update("A3", 2));
    /* The VM-session parts: state recorded, no open_request; data dropped
     * and credited back. */
    nnotes = 0;
    state(mg_session_state_open, 7);
    uint8_t data[8] = { mg_command_session, mg_session_data, mg_session_flag_first | mg_session_flag_last, 1, 0, 0, 0, 9 };
    write_bytes(data, sizeof(data));
    pump_all();
    for (int i = 0; i < nnotes; i++) {
        CHECK(!(notes[i].len >= 2 && notes[i].d[0] == mg_command_session && notes[i].d[1] == mg_session_open_request));
    }
    CHECK(credit_in(0) == 1 && states == 0 && messages == 0);
    /* A re-proof ends the session; its match keys one new sealed session. */
    nnotes = 0;
    prove_again();
    nnotes = 0;
    send_sealed(SE_UPD_K2_0, 247, 0);
    pump_all();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value) && stores == 2);
    nnotes = 0;
    write_hello(SE_SPP_HELLO3);
    dc_next = 1;
    pump_all();
    CHECK(note_is_answer(0, SE_SPP_ANSWER3));
    sealed_credentials("CREDS_SEALED2", 247);
    nnotes = 0;
    send_sealed(SE_UPD_K2_0, 247, 0);
    pump_all();
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R4") && stores == 3 && !strcmp(access_token, "A4"));
    /* The store refusing: sealed credentials_saved [0] (zeros), the pair as it was. */
    store_fails = true;
    nnotes = 0;
    send_sealed(SE_UPD_K2_1, 247, 0);
    pump_all();
    CHECK(nnotes == 1 && is_sealed_saved(0, false, "") && !strcmp(access_token, "A4"));
    store_fails = false;
    /* A setup change (a new setup's commit) ends it too. */
    mg_proto_proof_reset(&proto);
    nnotes = 0;
    send_sealed(SE_UPD_K2_1, 247, 0);
    pump_all();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_invalid_value));
    /* The connection's end: a new connection needs a new proof. */
    mg_proto_disconnect(&proto);
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    mg_proto_set_mtu(&proto, 247);
    nnotes = 0;
    sealed_hello();
    pump_all();
    CHECK(nnotes == 1 && note_is_answer(0, SE_SPP_ANSWER2));
    CHECK(proto.creds_cache == NULL && proto.creds_in == NULL);

    /* At the floor, ATT MTU 100: every fragment fits, the same sealed bytes. (Its refusal below the floor is the
     * portable core's.) */
    creds_setup(MG_MIN_ATT_MTU);
    prove_at_floor();
    sealed_hello();
    pump_all();
    CHECK(note_is_answer(0, SE_SPP_ANSWER1));
    sealed_credentials("CREDS_SEALED_MTU100", MG_MIN_ATT_MTU);
}

/* A plain hello (an encrypted link) builds no credentials at all. */
static void test_plain_credentials_buffer(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    prove();
    notify_budget = 0;
    hello(4096, 4096);
    CHECK(proto.session.cred_out == NULL && proto.session.cred_out_cap == 0 && !proto.session.hello);
    notify_budget = -1;
    pump_all();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_insecure));
}

/* A sealed hello that finds no room for its answer (every control slot
 * held by a full stack) is refused busy without taking the proof's nonces. */
static void test_sealed_hello_without_room(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    notify_budget = 0;
    uint8_t data[8] = { mg_command_session, mg_session_data, mg_session_flag_first | mg_session_flag_last, 1, 0, 0, 0, 7 };
    for (int i = 0; i < MG_SESSION_CTL_HELD; i++) {
        write_bytes(data, sizeof(data));   /* before a hello: an error each, held */
    }
    CHECK(proto.session.ctl_count == MG_SESSION_CTL_HELD);
    sealed_hello();
    CHECK(!proto.session.hello && proto.proof.nonces_held);   /* not answered, nonces kept */
    notify_budget = -1;
    pump_all();
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));   /* the same proof seals once */
}

/* While a re-setup is committing (the journal's begin until the commit
 * has ended the old setup's sessions), a client proven with the old K gets
 * no credentials: the VM session refuses hello busy, the credentials-only
 * one holds it; both keep the proof and answer once it's over. */
static void test_hello_during_resetup(void)
{
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    resetup_busy = true;
    sealed_hello();
    CHECK(nnotes == 1 && is_error(0, mg_error_code_busy) && proto.proof.nonces_held);
    hello(4096, 4096);   /* plain too */
    CHECK(nnotes == 2 && is_error(1, mg_error_code_busy));
    resetup_busy = false;
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));

    /* The credentials-only session (a Wi-Fi gadget): the hello waits. */
    creds_only = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    resetup_busy = true;
    sealed_hello();
    pump_all_busy();
    CHECK(nnotes == 0);
    resetup_busy = false;
    pump_all();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
    /* An update during one waits too, then is stored. */
    resetup_busy = true;
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    pump_all_busy();
    CHECK(nnotes == 0 && stores == 0);
    resetup_busy = false;
    pump_all();
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R2") && stores == 1 && !strcmp(access_token, "A2"));
    /* One held past 30 s (layer 2's deferral) is given up: sealed [0] while
     * the link is up (silence now reads as "assumed
     * saved"), the pair as it was. */
    resetup_busy = true;
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 0);
    pump_all_busy();
    now_ms += 29999;
    pump_all_busy();
    CHECK(nnotes == 0 && stores == 1);
    now_ms += 1;
    pump_all_busy();
    CHECK(nnotes == 1 && is_sealed_saved(0, false, "") && stores == 1 && !strcmp(access_token, "A2"));
    resetup_busy = false;
    pump_all();
    CHECK(nnotes == 1 && stores == 1);
}

static bool frame_is(int i, const char *want);

/* A proof with these nonces (hex), the confirm computed here. */
static void prove_with(const char *cnh, const char *dnh)
{
    uint8_t cn[MG_TOKEN_PROOF_NONCE_LEN];
    hex(cnh, cn, sizeof(cn));
    hex(dnh, device_nonce, sizeof(device_nonce));
    memcpy(cur_cn, cn, sizeof(cur_cn));
    uint8_t ch[2 + MG_TOKEN_PROOF_NONCE_LEN] = { mg_command_token_proof, mg_token_proof_challenge };
    memcpy(ch + 2, cn, sizeof(cn));
    write_bytes(ch, sizeof(ch));
    uint8_t cf[2 + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof, mg_token_proof_confirm };
    CHECK(mg_token_proof_mac(proof_k, false, cn, device_nonce, cf + 2));
    write_bytes(cf, sizeof(cf));
    CHECK(mg_proto_proof_matched(&proto));
}

/* sealed_credentials_saved's session: its K, node and pair, proven with its
 * nonces, its sealed hello answered with exactly its credentials. */
static void saved_session(bool creds)
{
    creds_only = creds;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    hex(SC_K, proof_k, sizeof(proof_k));
    node_id = SC_NODE;
    device_key = key_id = "";
    snprintf(access_token, sizeof(access_token), "%s", SC_ACCESS0);
    snprintf(refresh_token, sizeof(refresh_token), "%s", SC_REFRESH0);
    stored_expires = 0;
    prove_with(SC_CN, SC_DN);
    nnotes = 0;
    sealed_hello();
    if (creds) {
        pump_all();
    }
}

/* One update of the section, its frames as the client sent them. */
static void saved_update(size_t i, bool creds)
{
    nnotes = 0;
    for (int f = 0; f < SC_STEPS[i].n_client; f++) {
        write_hex(SC_STEPS[i].client[f]);
    }
    if (creds) {
        pump_all();
    }
}

/* The seal vectors' sealed_credentials_saved section, on the VM session (BLE-only) or the credentials-only one
 * (Wi-Fi gadgets): the credentials, then the vector's sealed [1] (stored) and
 * sealed [0] (the store refused) byte for byte; where the vector shows a
 * broken or old device (another pair's hash, an unsealed [1]) the device's
 * own answer: sealed [1 || the hash of that update's refresh_token] under the
 * next counter. creds also gives up on the second update after 30 s held:
 * the vector's sealed [0] again, byte for byte. */
static void test_saved_vectors(bool creds)
{
    saved_session(creds);
    CHECK(nnotes == 2 && frame_is(1, SC_CREDS));
    size_t n = sizeof(SC_STEPS) / sizeof(SC_STEPS[0]);
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        store_fails = i == 1;
        saved_update(i, creds);
        store_fails = false;
        bool ok;
        if (i <= 1) {
            ok = nnotes == 1 && SC_STEPS[i].saved[SC_STEPS[i].n_device - 1] == (i == 0)
                 && frame_is(0, SC_STEPS[i].device[SC_STEPS[i].n_device - 1]);
            dc_next += ok;
        } else {
            ok = nnotes == 1 && is_sealed_saved(0, true, SC_STEPS[i].refresh);
        }
        ok = ok && !strcmp(access_token, i == 1 ? SC_STEPS[0].access : SC_STEPS[i].access);
        if (!ok) {
            printf("sealed_credentials_saved %s: %d notes\n", SC_STEPS[i].name, nnotes);
            bad++;
        }
    }
    CHECK(bad == 0);
    if (!bad) {
        printf("SEALED_SAVED %zu updates (%s)\n", n, creds ? "creds" : "session");
    }
    if (creds) {
        saved_session(true);
        saved_update(0, true);
        CHECK(nnotes == 1 && frame_is(0, SC_STEPS[0].device[0]));
        resetup_busy = true;
        saved_update(1, false);   /* held: the store is busy */
        pump_all_busy();
        CHECK(nnotes == 0);
        now_ms += 30000;
        pump_all_busy();
        resetup_busy = false;
        bool same = nnotes == 1 && frame_is(0, SC_STEPS[1].device[SC_STEPS[1].n_device - 1])
                    && !strcmp(access_token, SC_STEPS[0].access);
        CHECK(same);
        if (same) {
            printf("CREDS_GIVE_UP sealed [0] at 30 s, byte for byte\n");
        }
    }
    creds_only = false;
    node_id = SV_NODE_ID;
    device_key = SV_DEVICE_KEY;
    key_id = SV_KEY_ID;
}

/* A sealed data message whose first fragment the stack refused holds the next device counter; if the VM
 * session then ends (close, open) before it went, it's dropped and its counter given back, so the next message
 * takes it and the client never sees a gap. */
static void test_unstarted_counter_back(void)
{
    setup(MG_MIN_ATT_MTU, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    nnotes = 0;
    sealed_hello();
    state(mg_session_state_open, 1);
    static uint8_t a[40], b[40];
    memset(a, 0x0a, sizeof(a));
    memset(b, 0x0b, sizeof(b));
    as_envelope(a, sizeof(a));
    as_envelope(b, sizeof(b));
    notify_budget = 0;
    CHECK(mg_proto_session_send(&proto, sid(), a, sizeof(a)));   /* sealed under counter 1, refused by the stack */
    (void)mg_proto_session_pump(&proto);
    notify_budget = -1;
    state(mg_session_state_closed, 1);
    state(mg_session_state_open, 2);
    nnotes = 0;
    CHECK(mg_proto_session_send(&proto, sid(), b, sizeof(b)));
    (void)mg_proto_session_pump(&proto);
    int found = 0;
    for (int i = 0; i < nnotes; i++) {
        const uint8_t *d = notes[i].d;
        if (notes[i].len > 10 && d[0] == mg_command_session && d[1] == mg_session_data && (d[2] & mg_session_flag_first)) {
            uint32_t ctr = (uint32_t)d[7] << 24 | (uint32_t)d[8] << 16 | (uint32_t)d[9] << 8 | d[10];
            CHECK(ctr == 1);   /* the dropped one's counter, not 2 */
            found++;
        }
    }
    CHECK(found == 1);
    if (!failures) {
        printf("PASS unstarted counter back: a sealed message dropped before it went gives its counter back\n");
    }
}

/* The device -> client counter on the wire: across sealed data part-way
 * out, stored credentials_updates and their sealed credentials_saved, data
 * sealed but held by a full stack, and more data, every sealed message's counter (in its first fragment) goes
 * out once and strictly increases, and every message opens under it: an
 * answer never takes a counter a data message already sealed. */
static void test_counter_order(void)
{
    setup(MG_MIN_ATT_MTU, true, 4096, 4096, 1u << 20);   /* the sealed session's floor */
    encrypted = false;
    prove();
    nnotes = 0;
    sealed_hello();
    state(mg_session_state_open, 1);
    static uint8_t d1[300], d2[40], d3[90], d4[20];
    memset(d1, 0x11, sizeof(d1));
    memset(d2, 0x22, sizeof(d2));
    memset(d3, 0x33, sizeof(d3));
    memset(d4, 0x44, sizeof(d4));
    /* Data 1 (counter 1) part-way out: the stack takes two of its fragments. */
    notify_budget = 2;
    CHECK(mg_proto_session_send(&proto, sid(), d1, sizeof(d1)));
    /* An update stored meanwhile: its answer goes between data 1's fragments,
     * counter 2 (data 1 holds 1); data 1 then finishes; data 2 takes 3. */
    notify_budget = 0;
    send_sealed(SE_UPD_0, 64, 0);
    notify_budget = -1;
    (void)mg_proto_session_pump(&proto);
    CHECK(mg_proto_session_send(&proto, sid(), d2, sizeof(d2)));
    /* Data 3 sealed (counter 4) but its first fragment refused; an update
     * comes before it goes: data 3 first, then the answer, 5. */
    notify_budget = 0;
    CHECK(mg_proto_session_send(&proto, sid(), d3, sizeof(d3)));
    send_sealed(SE_UPD_1, 64, 0);
    notify_budget = -1;
    (void)mg_proto_session_pump(&proto);
    CHECK(mg_proto_session_send(&proto, sid(), d4, sizeof(d4)));   /* 6 */
    CHECK(!mg_proto_session_pump(&proto) && stores == 2);

    /* The wire, in order: each sealed message reassembled per sub-command,
     * its counter read from its first fragment. */
    mgseal_keys_t k;
    CHECK(mgseal_keys(&k, proof_k, cur_cn, device_nonce, node_id, strlen(node_id)) == MGSEAL_OK);
    static uint8_t buf[3][4096];
    size_t got[3] = { 0, 0, 0 }, total[3] = { 0, 0, 0 };
    int64_t last = -1;
    int msgs = 0, bad = 0, between = 0;
    bool seen[7] = { false };
    static const uint8_t subs[3] = { mg_session_credentials, mg_session_data, mg_session_credentials_saved };
    for (int i = 0; i < nnotes; i++) {
        const uint8_t *d = notes[i].d;
        int w = -1;
        for (int j = 0; j < 3; j++) {
            if (notes[i].len > 3 && d[0] == mg_command_session && d[1] == subs[j]) {
                w = j;
            }
        }
        if (w < 0) {
            continue;
        }
        if (!(d[2] & mg_session_flag_sealed)) {
            bad++;
            continue;
        }
        size_t hdr = d[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        if (d[2] & mg_session_flag_first) {
            total[w] = d[3] | d[4] << 8 | d[5] << 16 | (size_t)d[6] << 24;
            got[w] = 0;
            uint32_t ctr = (uint32_t)d[7] << 24 | (uint32_t)d[8] << 16 | (uint32_t)d[9] << 8 | d[10];
            if ((int64_t)ctr <= last) {
                printf("counter order: %u after %lld (note %d)\n", (unsigned)ctr, (long long)last, i);
                bad++;
            }
            last = ctr;
            between += w == 2 && got[1] < total[1];   /* an answer while data was part-way out */
        }
        memcpy(buf[w] + got[w], d + hdr, notes[i].len - hdr);
        got[w] += notes[i].len - hdr;
        if (d[2] & mg_session_flag_last) {
            uint32_t ctr = (uint32_t)buf[w][0] << 24 | (uint32_t)buf[w][1] << 16 | (uint32_t)buf[w][2] << 8 | buf[w][3];
            size_t n = 0;
            if (got[w] != total[w] || mgseal_open(&k, MGSEAL_DIR_DEVICE, ctr, buf[w], got[w], &n) != MGSEAL_OK) {
                printf("counter order: message %d (sub %u, counter %u) doesn't open\n", msgs, subs[w], (unsigned)ctr);
                bad++;
            } else if (ctr >= 7 || seen[ctr]) {
                printf("counter order: counter %u again or out of range\n", (unsigned)ctr);
                bad++;
                ctr = 0;
            }
            seen[ctr < 7 ? ctr : 0] = true;
            /* What each is, by its counter (messages complete out of counter
             * order when an answer goes between a data message's fragments). */
            static const uint8_t want_sub[7] = { mg_session_credentials, mg_session_data, mg_session_credentials_saved,
                                                 mg_session_data, mg_session_data, mg_session_credentials_saved,
                                                 mg_session_data };
            static const uint8_t want_byte[7] = { '{', 0x11, 1, 0x22, 0x33, 1, 0x44 };
            if (ctr >= 7 || subs[w] != want_sub[ctr] || buf[w][4] != want_byte[ctr]) {
                printf("counter order: counter %u is sub %u\n", (unsigned)ctr, subs[w]);
                bad++;
            }
            msgs++;
        }
    }
    CHECK(bad == 0 && msgs == 7 && between == 1);
    if (!bad && msgs == 7 && between == 1) {
        printf("COUNTER_ORDER 7 sealed messages, counters 0-6 once each, in order\n");
    }
}

/* The credentials-only device (layer 2) has no plain session either: an unsealed hello on an
 * encrypted link is refused insecure, and an update after it is refused, nothing stored. */
static void test_plain_saved_creds(void)
{
    creds_setup(247);
    encrypted = true;
    prove();
    hello(4096, 4096);
    pump_all();
    bool one = nnotes == 1 && is_error(0, mg_error_code_insecure);
    nnotes = 0;
    send_update(PAIR);
    pump_all();
    bool zero = nnotes == 1 && stores == 0 && !strcmp(access_token, SV_ACCESS);
    CHECK(one && zero);
    if (one && zero) {
        printf("PLAIN_REFUSED (creds)\n");
    }
    creds_only = false;
}

/* request_status's caps byte, or -1 if it wasn't sent. */
static int caps_now(void)
{
    nnotes = 0;
    uint8_t rs = mg_command_request_status;
    write_bytes(&rs, 1);
    for (int i = 0; i < nnotes; i++) {
        if (notes[i].len == 4 && notes[i].d[0] == mg_command_supported_features
            && notes[i].d[1] == mg_command_sub_feature && notes[i].d[2] == mg_command_session) {
            return notes[i].d[3];
        }
    }
    return -1;
}

/* client_refreshes whenever the gadget holds no Wi-Fi credentials, asked
 * at each request_status. */
static void test_caps_follow_wifi(void)
{
    const int sealed = mg_session_cap_sealed, both = mg_session_cap_sealed | mg_session_cap_client_refreshes;
    /* The Wi-Fi gadgets' credentials-only session also lists credentials_only. */
    const int only = mg_session_cap_credentials_only;
    /* A Wi-Fi gadget set up token-only (no Wi-Fi credentials): the client refreshes. */
    creds_only = true;
    no_wifi = 1;
    setup(247, true, 4096, 4096, 1u << 20);
    CHECK(caps_now() == (both | only));
    /* Wi-Fi added: it refreshes itself, from the next request_status on. */
    no_wifi = 0;
    CHECK(caps_now() == (sealed | only));
    no_wifi = 1;
    CHECK(caps_now() == (both | only));
    /* A BLE-only build asked the same way. */
    no_wifi = 0;
    setup(247, true, 4096, 4096, 1u << 20);
    CHECK(caps_now() == sealed);
    no_wifi = 1;
    CHECK(caps_now() == both);
    no_wifi = -1;
}

/* The pair is taken out under the lock, stored without it, and
 * credentials_saved queued only if the session that sent it is still the
 * one open. */
static void test_store_outside_the_lock(void)
{
    store_later = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 0);
    char *a = NULL, *r = NULL;
    uint32_t gen = 0;
    uint64_t exp_out = 0;
    CHECK(mg_proto_session_take_update(&proto, &a, &r, &exp_out, &gen) && a && r && !strcmp(a, "A2") && !strcmp(r, "R2"));
    CHECK(nnotes == 0);
    char *a2 = NULL, *r2 = NULL;
    uint32_t g2 = 0;
    CHECK(!mg_proto_session_take_update(&proto, &a2, &r2, &exp_out, &g2));   /* handed out once */
    mg_proto_session_update_saved(&proto, gen, true);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R2"));
    free(a);
    free(r);
    /* A hello meanwhile (a new proof's sealed one) fences it: no credentials_saved for the old session. */
    send_sealed(SE_UPD_1, 247, 0);
    CHECK(mg_proto_session_take_update(&proto, &a, &r, &exp_out, &gen));
    prove_again();
    sealed_hello();
    nnotes = 0;
    mg_proto_session_update_saved(&proto, gen, true);
    CHECK(nnotes == 0);
    free(a);
    free(r);
    /* So does the link's end. */
    store_later = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    send_sealed(SE_UPD_0, 247, 0);
    CHECK(mg_proto_session_take_update(&proto, &a, &r, &exp_out, &gen));
    mg_proto_disconnect(&proto);
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    nnotes = 0;
    mg_proto_session_update_saved(&proto, gen, true);
    CHECK(nnotes == 0);
    free(a);
    free(r);
    /* A sealed one that doesn't open is refused at the take, nothing handed out. */
    store_later = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    nnotes = 0;
    send_sealed(SE_UPD_0, 247, 1);   /* a flipped tag */
    CHECK(nnotes == 0 && !mg_proto_session_take_update(&proto, &a, &r, &exp_out, &gen) && refused_update(SV_ACCESS, 0));
    /* A wrong counter doesn't get that far: refused at its first fragment. */
    nnotes = 0;
    send_sealed(SE_UPD_1, 247, 0);   /* counter 1 where 0 is next (the refused tag gave 0 back) */
    CHECK(refused_update(SV_ACCESS, 0) && !mg_proto_session_take_update(&proto, &a, &r, &exp_out, &gen));
}

/* The receive ring is taken when the client reports the session open
 * and given back when it ends or the link goes; none held in between. */
static void test_ring_per_session(void)
{
    lazy_ring = true;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    CHECK(ring_allocs == 0 && proto.session.ring == NULL);   /* a hello takes none */
    state(mg_session_state_open, 1);
    CHECK(ring_allocs == 1 && proto.session.ring && mg_proto_session_open(&proto));
    uint8_t msg[300];
    memset(msg, 0x5a, sizeof(msg));
    state(mg_session_state_closed, 1);
    CHECK(ring_frees == 1 && proto.session.ring == NULL);
    state(mg_session_state_open, 2);
    CHECK(ring_allocs == 2 && proto.session.ring);
    prove_again();
    sealed_hello();   /* a new hello ends the session */
    CHECK(ring_frees == 2 && proto.session.ring == NULL);
    state(mg_session_state_open, 3);
    mg_proto_disconnect(&proto);   /* the link goes */
    CHECK(ring_allocs == 3 && ring_frees == 3 && proto.session.ring == NULL);
    /* None to be had: not open, the client asked to close, data refused. */
    mg_proto_connect(&proto);
    mg_proto_subscribe(&proto, MG_CH_CONTROL, true);
    mg_proto_set_mtu(&proto, 247);
    prove();
    sealed_hello();
    ring_alloc_fails = true;
    nnotes = 0;
    state(mg_session_state_open, 4);
    CHECK(!mg_proto_session_open(&proto) && proto.session.ring == NULL);
    bool asked = false;
    for (int i = 0; i < nnotes; i++) {
        asked = asked || (notes[i].len == 3 && notes[i].d[0] == mg_command_session
                          && notes[i].d[1] == mg_session_open_request && notes[i].d[2] == 0);
    }
    CHECK(asked);
    nnotes = 0;
    client_send(msg, 10, 247);
    CHECK(nnotes >= 1 && is_error(0, mg_error_code_invalid_value));
}

/* access_expires_at (mgcommands.h; layer 2): the expiry a
 * sealed credentials_update carries is stored with the pair and handed back
 * as the credentials' last field (printed for the reference to open); an
 * update without it leaves none (credentials then carry none). The same on
 * the VM session (BLE-only) and the credentials-only one (Wi-Fi gadgets). */
static void test_access_expires_at(bool creds)
{
    const char *tag = creds ? "CREDS_EXP" : "EXP";
    char label[32];
    creds_only = creds;
    setup(247, true, 4096, 4096, 1u << 20);
    encrypted = false;
    prove();
    sealed_hello();
    nnotes = 0;
    send_sealed(SE_UPD_EXP_0, 247, 0);
    if (creds) {
        pump_all();
    }
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R7") && !strcmp(access_token, "A7") && stored_expires == 1793860000u);
    /* Handed back at the next sealed hello (a new proof). */
    prove_again();
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1 && note_is_answer(0, SE_SPP_ANSWER1));
    snprintf(label, sizeof(label), "%s_KEPT", tag);
    sealed_credentials(label, 247);
    /* An update without it: none kept, none handed back. */
    nnotes = 0;
    send_sealed(SE_UPD_K2_0, 247, 0);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R4") && !strcmp(access_token, "A4") && stored_expires == 0);
    prove();
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes > 1);
    snprintf(label, sizeof(label), "%s_NONE", tag);
    sealed_credentials(label, 247);
    /* Any other form is none; a duplicate is refused, the pair and expiry kept. */
    nnotes = 0;
    send_sealed(SE_UPD_EXP_STRING_0, 247, 0);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R8") && !strcmp(access_token, "A8") && stored_expires == 0);
    nnotes = 0;
    send_sealed(SE_UPD_EXP_1, 247, 0);
    CHECK(nnotes == 1 && is_sealed_saved(0, true, "R7") && stored_expires == 1793860000u);
    int before = stores;
    nnotes = 0;
    send_sealed(SE_UPD_EXP_DUP_2, 247, 0);
    CHECK(refused_update("A7", before) && stored_expires == 1793860000u);
    creds_only = false;
}

/* The seal vectors' access_expires_at section byte
 * for byte: every sealed credentials message the device sends, every answer
 * to a sealed update and what it keeps, over three proofs, on the VM session
 * (BLE-only) or the credentials-only one (Wi-Fi gadgets). */
static void test_reference_expires_vectors(bool creds)
{
    creds_only = creds;
    setup(100, true, 4096, 4096, 1u << 20);
    encrypted = false;
    hex(SX_K, proof_k, sizeof(proof_k));
    node_id = SX_NODE;
    device_key = key_id = "";   /* the section's credentials carry no device key */
    snprintf(access_token, sizeof(access_token), "%s", SX_ACCESS0);
    snprintf(refresh_token, sizeof(refresh_token), "%s", SX_REFRESH0);
    stored_expires = 0;
    int session = -1, bad = 0;
    for (size_t i = 0; i < sizeof(SX_STEPS) / sizeof(SX_STEPS[0]); i++) {
        if (SX_STEPS[i].session != session) {
            /* A new proof with the section's nonces, then a sealed hello. */
            session = SX_STEPS[i].session;
            uint8_t cn[MG_TOKEN_PROOF_NONCE_LEN];
            hex(SX_SESSIONS[session].cn, cn, sizeof(cn));
            hex(SX_SESSIONS[session].dn, device_nonce, sizeof(device_nonce));
            memcpy(cur_cn, cn, sizeof(cur_cn));
            uint8_t ch[2 + MG_TOKEN_PROOF_NONCE_LEN] = { mg_command_token_proof, mg_token_proof_challenge };
            memcpy(ch + 2, cn, sizeof(cn));
            write_bytes(ch, sizeof(ch));
            uint8_t cf[2 + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof, mg_token_proof_confirm };
            CHECK(mg_token_proof_mac(proof_k, false, cn, device_nonce, cf + 2));
            write_bytes(cf, sizeof(cf));
            CHECK(mg_proto_proof_matched(&proto));
            nnotes = 0;
            sealed_hello();
        }
        if (SX_STEPS[i].direction == 1) {
            /* The device's sealed credentials, exactly. */
            uint8_t want[MG_SESSION_MAX_CREDENTIALS + 64], got[MG_SESSION_MAX_CREDENTIALS + 64];
            size_t wn = hex(SX_STEPS[i].sealed, want, sizeof(want));
            int count;
            size_t gn = reassemble(1, mg_session_credentials, got, sizeof(got), &count);
            if (gn != wn || memcmp(got, want, wn)) {
                printf("expires step %zu: the sealed credentials differ (%zu bytes, want %zu)\n", i, gn, wn);
                bad++;
            }
        } else {
            nnotes = 0;
            send_sealed(SX_STEPS[i].sealed, 100, 0);
            if (creds) {
                pump_all();
            }
            /* The section keeps the legacy one-byte answer (it tests the
             * storage rule); a current device answers the
             * same verdict sealed (sealed_credentials_saved has those bytes):
             * [1 || SHA-256(its refresh_token)[0:8]], the next counter. */
            bool saved = !strcmp(SX_STEPS[i].device, "890801");
            if (!(nnotes == 1 && (saved ? is_sealed_saved(0, true, SX_STEPS[i].refresh) : note_is(0, SX_STEPS[i].device)))) {
                printf("expires step %zu: answered %d notes, not %s\n", i, nnotes, saved ? "sealed [1]" : SX_STEPS[i].device);
                bad++;
            }
            if (stored_expires != SX_STEPS[i].stored || (saved && strcmp(access_token, SX_STEPS[i].access))) {
                printf("expires step %zu: keeps %llu, want %llu\n", i, (unsigned long long)stored_expires,
                       SX_STEPS[i].stored);
                bad++;
            }
        }
    }
    CHECK(bad == 0);
    node_id = SV_NODE_ID;
    device_key = SV_DEVICE_KEY;
    key_id = SV_KEY_ID;
    creds_only = false;
}

/* The seal vectors' sealed_data section (mgcommands.h, Sealed data):
 * a sealed session carrying data both ways between
 * the credentials and a credentials_update, one counter per direction shared
 * by all of them, a replayed counter (invalid_value), unsealed data
 * (insecure) and a message aborted after its first fragment (its counter
 * used). The device's notifications are the vector's frames exactly, and the
 * client's frames give the vector's outcome. */
/* Note i is exactly want (hex), at any length up to a fragment. */
static bool frame_is(int i, const char *want)
{
    uint8_t w[MG_SESSION_FRAGMENT_MAX];
    size_t wn = hex(want, w, sizeof(w));
    return i < nnotes && notes[i].len == wn && !memcmp(notes[i].d, w, wn);
}


static uint32_t rd32_le(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void test_sealed_data_vectors(bool sealed_saved)
{
    /* sealed_saved: sealed_data_large, a current client (its hello carries sealed_saved): the
     * update's answer sealed under device counter 2, the data after it 3.
     * Else sealed_data, a client that predates sealed_saved (sealed alone: an older
     * iOS app, the bench app): the update's answer the legacy unsealed
     * [1], which takes no counter, so the device's next data is counter 2
     * as before sealed_saved. */
    const sd_step_t *steps = sealed_saved ? SD_STEPS : SD0_STEPS;
    size_t nsteps = sealed_saved ? sizeof(SD_STEPS) / sizeof(SD_STEPS[0]) : sizeof(SD0_STEPS) / sizeof(SD0_STEPS[0]);
    const char *sec = sealed_saved ? "sealed_data_large" : "sealed_data";
    client_saved = sealed_saved;
    setup(SD_MTU, true, 4096, 4096, 1u << 20);
    encrypted = false;
    hex(SD_K, proof_k, sizeof(proof_k));
    node_id = SD_NODE;
    device_key = key_id = "";
    snprintf(access_token, sizeof(access_token), "%s", SD_ACCESS0);
    snprintf(refresh_token, sizeof(refresh_token), "%s", SD_REFRESH0);
    uint8_t cn[MG_TOKEN_PROOF_NONCE_LEN];
    hex(SD_CN, cn, sizeof(cn));
    hex(SD_DN, device_nonce, sizeof(device_nonce));
    uint8_t ch[2 + MG_TOKEN_PROOF_NONCE_LEN] = { mg_command_token_proof, mg_token_proof_challenge };
    memcpy(ch + 2, cn, sizeof(cn));
    write_bytes(ch, sizeof(ch));
    uint8_t cf[2 + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof, mg_token_proof_confirm };
    CHECK(mg_token_proof_mac(proof_k, false, cn, device_nonce, cf + 2));
    write_bytes(cf, sizeof(cf));
    CHECK(mg_proto_proof_matched(&proto));
    nnotes = 0;
    sealed_hello();
    /* The hello's answer echoes what it accepted: sealed_saved to a current client only. */
    CHECK(nnotes >= 1 && notes[0].len == 12 && notes[0].d[1] == mg_session_hello
          && notes[0].d[11] == (sealed_saved ? (mg_session_hello_flag_sealed | mg_session_hello_flag_sealed_saved)
                                    : mg_session_hello_flag_sealed));
    int bad = 0;
    bool opened = false;
    static note_t answer;   /* the device's answer to the update step, checked at its own step */
    int answers = 0;
    size_t replayed = 0;   /* the steps this run went through: a section with none can't pass */
    for (size_t i = 0; i < nsteps; i++) {
        replayed++;
        const char *name = steps[i].name;
        uint8_t pt[4096];
        size_t ptn = hex(steps[i].plaintext, pt, sizeof(pt));
        if (steps[i].direction == 1 && steps[i].sub == mg_session_credentials_saved) {
            /* The sealed credentials_saved (counter 2) the
             * update step drew from the device: exactly the vector's frame. */
            nnotes = 1;
            notes[0] = answer;
            if (!(answers == 1 && steps[i].n_frames == 1 && frame_is(0, steps[i].frames[0]))) {
                printf("%s %s: not the vector's frame\n", sec, name);
                bad++;
            }
            continue;
        }
        if (steps[i].direction == 1) {
            /* The device's: its credentials after the hello's answer, or data
             * the session code queued; either way exactly the frames. */
            int from = 0;
            if (steps[i].sub == mg_session_credentials) {
                from = 1;   /* note 0: the hello's answer */
            } else {
                if (!opened) {
                    uint8_t st[5] = { mg_command_session, mg_session_state, mg_session_state_open, 1, 0 };
                    write_bytes(st, sizeof(st));
                    opened = true;
                }
                nnotes = 0;
                CHECK(mg_proto_session_send(&proto, sid(), pt, ptn));
            }
            bool same = nnotes - from == steps[i].n_frames;
            for (int f = 0; same && f < steps[i].n_frames; f++) {
                same = frame_is(from + f, steps[i].frames[f]);
            }
            if (!same) {
                printf("%s %s: %d notes, want %d frames\n", sec, name, nnotes - from, steps[i].n_frames);
                bad++;
            }
            continue;
        }
        if (!opened) {
            uint8_t st[5] = { mg_command_session, mg_session_state, mg_session_state_open, 1, 0 };
            write_bytes(st, sizeof(st));
            opened = true;
        }
        nnotes = 0;
        int stores_before = stores;
        size_t sent = 0;
        for (int f = 0; f < steps[i].n_frames; f++) {
            uint8_t fr[MG_SESSION_FRAGMENT_MAX];
            size_t n = hex(steps[i].frames[f], fr, sizeof(fr));
            write_bytes(fr, n);
            if (fr[1] == mg_session_data) {
                sent += n - (fr[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER);
            }
        }
        if (!strcmp(steps[i].outcome, "abort") || !strcmp(steps[i].outcome, "aborted")) {
            /* Abort the rest of the step: the frames end with the abort. */
            static uint8_t msg[4096];
            bool ok = !mg_proto_session_take(&proto, sid(), msg, sizeof(msg));
            for (int k = 0; k < nnotes; k++) {
                ok = ok && notes[k].d[0] != mg_command_error;
            }
            if (!ok) {
                printf("%s %s: not aborted quietly\n", sec, name);
                bad++;
            }
            continue;
        }
        int errors = 0, answered = 0;
        uint32_t credit = 0;
        for (int k = 0; k < nnotes; k++) {
            errors += notes[k].d[0] == mg_command_error;
            answered += steps[i].answer && frame_is(k, steps[i].answer);
            if (notes[k].len == 6 && notes[k].d[0] == mg_command_session && notes[k].d[1] == mg_session_credit) {
                credit += rd32_le(notes[k].d + 2);
            }
        }
        if (!strcmp(steps[i].outcome, "refused")) {
            /* Exactly the vector's answer, the message dropped, its credit back. */
            static uint8_t msg[4096];
            if (!(errors == 1 && answered == 1 && credit == sent && !mg_proto_session_take(&proto, sid(), msg, sizeof(msg))
                  && stores == stores_before)) {
                printf("%s %s: %d errors, answered %d, credit %u of %zu\n", sec, name, errors, answered,
                       (unsigned)credit, sent);
                bad++;
            }
        } else if (steps[i].sub == mg_session_credentials_update) {
            /* Stored at once (store_later off here): the pair and its expiry,
             * then the answer (the vector's next step). */
            if (!(errors == 0 && stores == stores_before + 1 && nnotes == 1
                  && notes[0].d[1] == mg_session_credentials_saved && (sealed_saved || is_saved(0, 1))
                  && !strcmp(access_token, "mga_data_access_2") && stored_expires == 1893456000ull)) {
                printf("%s %s: not stored\n", sec, name);
                bad++;
            } else {
                answer = notes[0];
                answers++;
            }
        } else {
            static uint8_t msg[4096];
            size_t got = mg_proto_session_take(&proto, sid(), msg, sizeof(msg));
            if (!(errors == 0 && got == ptn && !memcmp(msg, pt, ptn))) {
                printf("%s %s: took %zu bytes, want %zu\n", sec, name, got, ptn);
                bad++;
            }
        }
    }
    CHECK(bad == 0 && answers == 1 && replayed == nsteps && replayed > 0);
    if (!bad && sealed_saved) {
        printf("SEALED_DATA_LARGE %zu steps byte for byte\n", replayed);
    } else if (!bad) {
        printf("SEALED_DATA_LARGE %zu steps byte for byte (credentials_saved 890801, no counter)\n", replayed);
    }
    client_saved = true;
    node_id = SV_NODE_ID;
    device_key = SV_DEVICE_KEY;
    key_id = SV_KEY_ID;
}

/* A client that predates sealed_saved (its sealed hello carries sealed alone: an
 * older iOS app, the bench app): every credentials_saved is the legacy one byte, unsealed, and
 * takes no device counter, exactly as before sealed_saved. On the BLE-only session
 * (creds false): stored [1], refused [0], and a superseded update's [0]
 * (store_later), then the device's next sealed data under counter 1 (the
 * credentials took 0: no gap). On the credentials-only session (layer 2):
 * [1], [0], and a write-back given up on after 30 s held stays silent. */
static void test_saved_legacy(bool creds)
{
    client_saved = false;
    saved_session(creds);
    /* The hello's answer echoes sealed alone; the credentials under 0 as before. */
    bool hello_ok = nnotes == 2 && notes[0].len == 12 && notes[0].d[11] == mg_session_hello_flag_sealed
                    && frame_is(1, SC_CREDS);
    CHECK(hello_ok);
    saved_update(0, creds);
    bool one = nnotes == 1 && is_saved(0, 1) && !strcmp(access_token, SC_STEPS[0].access);
    store_fails = true;
    saved_update(1, creds);
    store_fails = false;
    bool zero = nnotes == 1 && is_saved(0, 0) && !strcmp(access_token, SC_STEPS[0].access);
    CHECK(one && zero);
    bool tail = false;
    if (creds) {
        saved_session(true);
        saved_update(0, true);
        CHECK(nnotes == 1 && is_saved(0, 1));
        resetup_busy = true;
        saved_update(1, false);   /* held: the store is busy */
        pump_all_busy();
        CHECK(nnotes == 0);
        now_ms += 30000;
        pump_all_busy();
        resetup_busy = false;
        pump_all();
        tail = nnotes == 0 && !strcmp(access_token, SC_STEPS[0].access);   /* given up: silent, as before sealed_saved */
        CHECK(tail);
        if (hello_ok && one && zero && tail) {
            printf("SAVED_LEGACY creds: [1] [0], give-up silent\n");
        }
    } else {
        /* A superseded update (store_later): [0] for it, then [1] for the
         * second, both one byte (one_session_per_proof's proof and node:
         * the SE_UPD_* frames are sealed for them). */
        node_id = SV_NODE_ID;
        device_key = SV_DEVICE_KEY;
        key_id = SV_KEY_ID;
        store_later = true;
        setup(247, true, 4096, 4096, 1u << 20);
        encrypted = false;
        prove();
        sealed_hello();
        CHECK(nnotes > 1 && notes[0].len == 12 && notes[0].d[11] == mg_session_hello_flag_sealed);
        nnotes = 0;
        send_sealed(SE_UPD_0, 247, 0);
        CHECK(mg_proto_session_commit(&proto) && nnotes == 1 && is_saved(0, 1));
        nnotes = 0;
        send_sealed(SE_UPD_1, 247, 0);
        send_sealed(SE_UPD_2_SKIP, 247, 0);
        CHECK(mg_proto_session_commit(&proto));
        bool sup = nnotes == 2 && is_saved(0, 0) && is_saved(1, 1) && !strcmp(access_token, "AX");
        CHECK(sup);
        /* Three answers, no counter: the device's first data is counter 1,
         * sealed under this session's keys. */
        state(mg_session_state_open, 1);
        nnotes = 0;
        static const uint8_t msg[] = "legacy counter";
        CHECK(mg_proto_session_send(&proto, sid(), msg, sizeof(msg)));
        uint32_t ctr = 0xFFFFFFFFu;
        if (nnotes >= 1 && notes[0].len > MG_SESSION_FIRST_HEADER + 4 && notes[0].d[1] == mg_session_data) {
            const uint8_t *c = notes[0].d + MG_SESSION_FIRST_HEADER;
            ctr = (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3];
        }
        tail = ctr == 1;
        CHECK(tail);
        if (hello_ok && one && zero && sup && tail) {
            printf("SAVED_LEGACY session: [1] [0] superseded [0], next data counter 1\n");
        }
    }
    client_saved = true;
    creds_only = false;
    node_id = SV_NODE_ID;
    device_key = SV_DEVICE_KEY;
    key_id = SV_KEY_ID;
}

/* The device's hello answer echoes only the flags it accepted: a sealed hello
 * with sealed_saved (and an unknown bit) gets sealed | sealed_saved; sealed
 * alone, sealed; sealed_saved without sealed means nothing (no sealed
 * session to count in): on a plain link insecure, on an encrypted one the
 * plain 11-byte answer with no flags byte and the one-byte credentials_saved. */
static void test_hello_flags(bool creds)
{
    static const uint8_t SS = mg_session_hello_flag_sealed_saved, SE = mg_session_hello_flag_sealed;
    const uint8_t sent[3] = { (uint8_t)(SE | SS | 0x80u), SE, SS };
    const uint8_t echo[3] = { (uint8_t)(SE | SS), SE, 0 };
    int ok = 0;
    for (int k = 0; k < 3; k++) {
        creds_only = creds;
        setup(247, true, 4096, 4096, 1u << 20);
        encrypted = false;
        prove();
        uint8_t h[12] = { mg_command_session, mg_session_hello, MG_SESSION_VERSION, 0, 0x10, 0, 0, 0, 0x10, 0, 0, sent[k] };
        write_bytes(h, sizeof(h));
        if (creds) {
            pump_all();
        }
        bool good;
        if (!echo[k]) {
            good = nnotes == 1 && is_error(0, mg_error_code_insecure);
            /* On a bonded link too: insecure, no plain session. */
            creds_only = creds;
            setup(247, true, 4096, 4096, 1u << 20);
            prove();
            write_bytes(h, sizeof(h));
            if (creds) {
                pump_all();
            }
            good = good && nnotes == 1 && is_error(0, mg_error_code_insecure);
        } else {
            good = nnotes > 1 && notes[0].len == 12 && notes[0].d[1] == mg_session_hello && notes[0].d[11] == echo[k]
                   && (notes[1].d[2] & mg_session_flag_sealed);
        }
        if (!good) {
            printf("hello flags %02x: %d notes\n", sent[k], nnotes);
        }
        ok += good;
    }
    CHECK(ok == 3);
    if (ok == 3) {
        printf("HELLO_FLAGS %s: echoed sealed|sealed_saved, sealed, none\n", creds ? "creds" : "session");
    }
    creds_only = false;
}

/* The seal vectors' sealed_interleaved section ("Two in flight"),
 * frame by frame: data and a
 * credentials_update reassemble separately, each taking its counter at its
 * first fragment. An update takes 0, data takes 1 and goes through, the
 * update is refused at its tag: 0 stays open once (the hole), so the real
 * update 0 is stored, its replay refused (every write of it), and data 2
 * goes through. later: the update waits whole for the worker's commit
 * (store_later, the BLE builds), where its tag is checked. */
static void test_sealed_interleaved_vectors(bool later)
{
    /* What the tests after this one find, as they found it. */
    char access_was[sizeof(access_token)], refresh_was[sizeof(refresh_token)];
    memcpy(access_was, access_token, sizeof(access_was));
    memcpy(refresh_was, refresh_token, sizeof(refresh_was));
    uint64_t expires_was = stored_expires;
    store_later = later;
    setup(SI_MTU, true, 4096, 4096, 1u << 20);
    encrypted = false;
    hex(SI_K, proof_k, sizeof(proof_k));
    node_id = SI_NODE;
    device_key = key_id = "";
    snprintf(access_token, sizeof(access_token), "%s", "mga_il_0");
    snprintf(refresh_token, sizeof(refresh_token), "%s", "mgr_il_0");
    stored_expires = 0;
    uint8_t cn[MG_TOKEN_PROOF_NONCE_LEN];
    hex(SI_CN, cn, sizeof(cn));
    hex(SI_DN, device_nonce, sizeof(device_nonce));
    memcpy(cur_cn, cn, sizeof(cur_cn));
    uint8_t ch[2 + MG_TOKEN_PROOF_NONCE_LEN] = { mg_command_token_proof, mg_token_proof_challenge };
    memcpy(ch + 2, cn, sizeof(cn));
    write_bytes(ch, sizeof(ch));
    uint8_t cf[2 + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof, mg_token_proof_confirm };
    CHECK(mg_token_proof_mac(proof_k, false, cn, device_nonce, cf + 2));
    write_bytes(cf, sizeof(cf));
    CHECK(mg_proto_proof_matched(&proto));
    nnotes = 0;
    sealed_hello();
    CHECK(nnotes >= 1 && notes[0].len == 12 && notes[0].d[0] == mg_command_session && notes[0].d[1] == mg_session_hello);   /* sealed: the flag echoed */
    uint8_t st[5] = { mg_command_session, mg_session_state, mg_session_state_open, 1, 0 };
    write_bytes(st, sizeof(st));
    int bad = 0, stores0 = stores;
    for (size_t i = 0; i < sizeof(SI_STEPS) / sizeof(SI_STEPS[0]); i++) {
        const char *name = SI_STEPS[i].name, *outcome = SI_STEPS[i].outcome;
        bool update = SI_STEPS[i].sub == mg_session_credentials_update;
        uint8_t pt[4096];
        size_t ptn = hex(SI_STEPS[i].plaintext, pt, sizeof(pt));
        nnotes = 0;
        int stores_before = stores;
        size_t sent = 0;
        for (int f = 0; f < SI_STEPS[i].n_frames; f++) {
            uint8_t fr[MG_SESSION_FRAGMENT_MAX];
            size_t n = hex(SI_STEPS[i].frames[f], fr, sizeof(fr));
            write_bytes(fr, n);
            if (fr[1] == mg_session_data) {
                sent += n - (fr[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER);
            }
        }
        bool at_commit = false;
        if (later && update && nnotes == 0 && strcmp(outcome, "partial")) {
            /* Whole and waiting: opened (its tag checked) at the worker's commit. */
            CHECK(mg_proto_session_commit(&proto));
            at_commit = true;
        }
        int errors = 0, answered = 0;
        uint32_t credit = 0;
        for (int k = 0; k < nnotes; k++) {
            errors += notes[k].d[0] == mg_command_error;
            answered += SI_STEPS[i].answer && frame_is(k, SI_STEPS[i].answer);
            if (notes[k].len == 6 && notes[k].d[0] == mg_command_session && notes[k].d[1] == mg_session_credit) {
                credit += rd32_le(notes[k].d + 2);
            }
        }
        static uint8_t msg[4096];
        bool ok;
        if (!strcmp(outcome, "partial")) {
            ok = nnotes == 0 && stores == stores_before && !mg_proto_session_take(&proto, sid(), msg, sizeof(msg));
        } else if (!strcmp(outcome, "refused") && update) {
            /* Exactly the vector's answer for every write of it (nothing open
             * after a refused first fragment), or once at the commit; nothing
             * stored. */
            ok = errors == answered && errors == (at_commit ? 1 : SI_STEPS[i].n_frames) && errors == nnotes
                 && stores == stores_before;
        } else if (!strcmp(outcome, "refused")) {
            ok = errors == 1 && answered == 1 && credit == sent && stores == stores_before
                 && !mg_proto_session_take(&proto, sid(), msg, sizeof(msg));
        } else if (update) {
            ok = errors == 0 && stores == stores_before + 1 && nnotes == 1 && is_sealed_saved(0, true, SI_REFRESH)
                 && !strcmp(access_token, SI_ACCESS) && !strcmp(refresh_token, SI_REFRESH) && stored_expires == SI_EXPIRES;
        } else {
            size_t got = mg_proto_session_take(&proto, sid(), msg, sizeof(msg));
            ok = errors == 0 && stores == stores_before && got == ptn && !memcmp(msg, pt, ptn);
        }
        if (!ok) {
            printf("sealed_interleaved %s: %s: %d notes, %d errors, answered %d, stores %d -> %d\n", later ? "later" : "now",
                   name, nnotes, errors, answered, stores_before, stores);
            bad++;
        }
    }
    CHECK(bad == 0 && stores == stores0 + 1 && !strcmp(access_token, SI_ACCESS));
    if (!bad) {
        printf("SEALED_INTERLEAVED %zu steps frame by frame (%s)\n", sizeof(SI_STEPS) / sizeof(SI_STEPS[0]),
               later ? "store_later" : "stored at once");
    }
    memcpy(access_token, access_was, sizeof(access_was));
    memcpy(refresh_token, refresh_was, sizeof(refresh_was));
    stored_expires = expires_was;
    node_id = SV_NODE_ID;
    device_key = SV_DEVICE_KEY;
    key_id = SV_KEY_ID;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "unit";
    if (!strcmp(mode, "creds_clear")) {
        test_creds_sealed_clear();
    } else if (!strcmp(mode, "creds")) {
        test_creds_session();
        test_access_expires_at(true);
        test_reference_expires_vectors(true);
        test_saved_vectors(true);
        test_plain_saved_creds();
        test_saved_legacy(true);
        test_hello_flags(true);
        if (!failures) {
            printf("PASS mg_session creds (%d checks)\n", checks);
        }
    } else if (!strcmp(mode, "sealed")) {
        test_sealed_caps();
        test_sealed_refusals();
        test_sealed_exchange();
        test_sealed_store_later();
        test_sealed_data_vectors(true);
        test_sealed_data_vectors(false);
        test_sealed_interleaved_vectors(false);
        test_sealed_interleaved_vectors(true);
        test_plain_credentials_buffer();
        test_sealed_hello_without_room();
        test_hello_during_resetup();
        test_caps_follow_wifi();
        test_store_outside_the_lock();
        test_ring_per_session();
        test_access_expires_at(false);
        test_reference_expires_vectors(false);
        test_saved_vectors(false);
        test_counter_order();
        test_unstarted_counter_back();
        test_saved_legacy(false);
        test_hello_flags(false);
        if (!failures) {
            printf("PASS mg_session sealed (%d checks)\n", checks);
        }
    } else {
        test_offered();
        test_plain_refused();
        test_full_stack();
        test_window();
        test_window_under_max();
        test_nudge();
        test_small_max_message();
        test_burst();
        test_endings();
        test_credentials_update();
        test_sealed_first_fragment();
        test_sealed_clear();
        test_owner_fails_closed();
        test_sealed_clear_async();
        test_new_hello_drops_partials();
        test_held_answers();
        test_store_later();
        test_json();
        if (!failures) {
            printf("PASS mg_session unit (%d checks)\n", checks);
        }
    }
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    return 0;
}
