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
 * The BLE side of mg_session_gadget_harness.cpp: one mg_proto_t with the
 * device session, built as C (mg_proto.h reaches headers C++ can't take), and
 * driven as mg_ble.c drives it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool (*notify)(const uint8_t *d, size_t n);   /* a Control notification */
    bool (*encrypted)(void);
    bool (*credentials)(mg_session_creds_t *c);
    bool (*store_tokens)(const char *access_token, const char *refresh_token, uint64_t expires);   /* expires: access_expires_at, 0 none */
    void (*state)(uint8_t state, uint16_t epoch);
    void (*message)(void);
    void (*cleared)(void);               /* the board's clear ran (the tokens and K gone); NULL: nothing */
} gb_ops_t;

/* A new connection at this ATT MTU; with k and nonce, a matching token proof
 * (mg-token-proof-v1.json's challenge and confirm frames). */
uint32_t gb_rx_credit(void);   /* what the client may still send (the window check) */
void gb_connect(const gb_ops_t *ops, uint32_t max_message, uint32_t window, uint16_t mtu, const uint8_t *k,
                const uint8_t *device_nonce, const uint8_t *challenge, size_t challenge_len, const uint8_t *confirm,
                size_t confirm_len);
void gb_disconnect(void);
void gb_set_nonce(const uint8_t *device_nonce);   /* the next proof's device nonce */
bool gb_proof_matched(void);
void gb_write(const uint8_t *d, size_t n);
bool gb_open(void);
bool gb_connected(void);
uint32_t gb_id(void);
void gb_reask_marks(uint32_t *hellos, uint32_t *saved);
bool gb_send(uint32_t session, const uint8_t *msg, size_t len);
size_t gb_room(void);
uint32_t gb_client_max(void);
size_t gb_take(uint32_t session, uint8_t *out, size_t cap);
void gb_request(bool open);
bool gb_sealed(void);
int gb_refused(void);   /* data messages the session refused (session_data_refused) */
void gb_set_mtu(uint16_t mtu);   /* the link's ATT MTU from here on */
void gb_nudge(void);   /* the vectors' nudge event: the open session's keepalive (mg_proto_session_nudge) */
void gb_pump(void);   /* the vectors' buffer event: the stack has room again (mg_proto_session_pump) */
void gb_set_counters(long long rx, long long tx);   /* the vectors' counters event: -1 keeps one */

#ifdef __cplusplus
}
#endif
