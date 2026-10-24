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

// Internal glue between the Noise session owner (noise_control.cpp) and the
// tunnel data-plane stream (noise_tunnel.cpp). Not part of the public API.
//
// noise_control.cpp owns the single ClientSession and its event loop. The
// tunnel multiplexes onto that session as a second stream. To avoid pulling
// noise core C++ types into this header, the session owner passes an opaque
// context plus a small set of function pointers the tunnel uses to emit
// outbound ServiceFrames; the tunnel exposes lifecycle + pump hooks the
// session owner calls.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tunnel stream id on the shared Noise session (control uses stream 1).
#define TUNNEL_STREAM_ID 2

// Emit-side callbacks the session owner provides to the tunnel. All run on
// the session task, so they may touch the ClientSession directly.
typedef struct {
    void *ctx;  // opaque, passed back to the callbacks below

    // Open the POST /link-tunnel stream (StartOutboundApplicationRequest +
    // flush). Returns true on success.
    bool (*open_stream)(void *ctx);

    // Send a BodyChunk on the tunnel stream carrying `data`/`len` (the
    // [u16-LE len][pkt]... batch). Returns true on success.
    bool (*send_body)(void *ctx, const uint8_t *data, size_t len);

    // Optional: the same, encoding and encrypting the batch where it sits.
    // The batch is buf[headroom, +len); the bytes before it (at least
    // NOISE_TUNNEL_TX_HEADROOM) and after it (NOISE_TUNNEL_TX_TAILROOM) are
    // scratch for the frame headers and the tag. NULL: send_body is used.
    bool (*send_body_inplace)(void *ctx, uint8_t *buf, size_t headroom,
                              size_t len, size_t tailroom);
} noise_tunnel_emit_t;

// Room around each outbound batch for in-place encoding: the WebSocket header
// (up to 14 bytes) and the Noise frame headers (at most 64) in front, the AEAD
// tag behind.
#define NOISE_TUNNEL_TX_HEADROOM 96
#define NOISE_TUNNEL_TX_TAILROOM 16

// ---- Called by the session owner (noise_control.cpp) ------------------------

// TOTAL free DMA is the burst margin: roughly two 8 KiB batches (or ten
// MTU-sized buffers) for the chunk plus concurrent WiFi/lwIP churn. Keep this
// at the result-queue boundary, before every control chunk, tunnel batch and
// ping. Download admission also leaves this margin after its allocations.
// It is not a contiguous-allocation requirement or a reservation.
#define NOISE_TX_DMA_RESERVE_BYTES (16 * 1024)

// Total-free burst margin for tunnel batches, result dequeue, every control
// chunk and tunnel pings. Optionally return the same sample for diagnostics.
bool noise_tx_has_dma_headroom(size_t *dma_free);

// The same margin, counting `reclaimable` bytes the send itself frees (Muse's
// queued request payloads; on boards without PSRAM they are DMA-capable too).
// Muse and display builds only.
bool noise_tx_has_dma_headroom_reclaiming(size_t reclaimable);

// Small contiguous AES-allocation floor, independent of the total-free burst
// margin above. Control chunks and tunnel pings require BOTH checks. This
// largest-block heap walk is intentionally absent from the tunnel batch hot
// path; stream opens also retain their existing fragmentation exposure.
bool noise_tx_has_contiguous_dma_headroom(void);

// Session established: open the tunnel stream and mark it live. Returns false
// if the stream could not be opened (caller may treat as fatal or continue).
bool noise_tunnel_on_session_up(const noise_tunnel_emit_t *emit);

// Retry opening the tunnel stream if it is currently down while the control
// session is still up (e.g. after a transient backend 503). Rate-limited
// internally; safe to call every loop iteration. No-op if already up.
void noise_tunnel_maybe_reopen(const noise_tunnel_emit_t *emit);

// Session torn down: mark the tunnel down and forget the emit context.
void noise_tunnel_on_session_down(void);

// Deliver an inbound BodyChunk received on TUNNEL_STREAM_ID to the packet cb.
void noise_tunnel_on_inbound(const uint8_t *data, size_t len);

// Send batches queued at entry while DMA headroom permits. Queued batches stay
// pending under pressure; return promptly so control/RX/keepalives can run.
// Returns the number sent (0 if empty or gated), or -1 on send error.
int noise_tunnel_pump_tx(const noise_tunnel_emit_t *emit);

// Tunnel-stream keepalive. Call periodically from the session loop while the
// tunnel is up. Sends a sentinel-byte keepalive ping when the stream has been
// idle and DMA headroom permits. Marks the tunnel down (so maybe_reopen
// reconnects) if no inbound frame — pong or data — has arrived within the
// keepalive timeout. This detects
// the half-open case where the WS/Noise transport is alive but the VM-side
// backend behind raw.sock has gone away. No-op if the tunnel is down.
void noise_tunnel_tick(const noise_tunnel_emit_t *emit);

#ifdef __cplusplus
}
#endif
