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

// noise_tunnel — L3 data plane multiplexed on the Noise control session.
//
// The tunnel does NOT open its own TLS/WebSocket/Noise connection. It rides
// the single Noise session owned by noise_control (same /v1/noise connection
// through the shared LB), as a second ServiceFrame stream (stream_id = 2)
// carrying a streaming POST /link-tunnel. IP packet batches flow as BodyChunk
// frames whose body is the tunnel_netif [u16-LE len][packet]... sub-framing.
// Noise message boundaries delimit batches, so there is no outer u32 envelope.
//
// Wiring mirrors the old raw_tunnel API so tunnel_netif.c can call it
// unchanged (send batch out, deliver received batch to the packet cb).

typedef void (*noise_tunnel_packet_cb)(const uint8_t *data, size_t len);

// Register the callback invoked (from the session task) once per inbound
// tunnel BodyChunk, with the raw [u16-LE len][packet]... batch payload.
void noise_tunnel_set_packet_cb(noise_tunnel_packet_cb cb);

// Queue an outbound batch payload (tunnel_netif's [u16-LE len][packet]...
// framing) for the session task to send as a BodyChunk on the tunnel stream.
// Non-blocking; returns false if the tunnel is down or the TX pool is full.
bool noise_tunnel_send_packet(const uint8_t *data, size_t len);

// Zero-copy form of noise_tunnel_send_packet: take a free batch buffer of
// *cap bytes to fill in place, then hand it to commit (queued for the session
// task) or abort (back to the pool). Returns NULL if the tunnel is down or
// the pool is empty. Exactly one of commit/abort per acquired buffer.
uint8_t *noise_tunnel_acquire_batch(size_t *cap);
bool noise_tunnel_commit_batch(uint8_t *buf, size_t len);
void noise_tunnel_abort_batch(uint8_t *buf);

// True once the tunnel stream is open on the live Noise session.
bool noise_tunnel_is_connected(void);

#ifdef __cplusplus
}
#endif
