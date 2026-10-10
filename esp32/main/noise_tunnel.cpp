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

#include "noise_tunnel.h"
#include "noise_tunnel_internal.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "link.noise_tun";

// Max batch payload = tunnel_netif BATCH_MAX_BYTES. Keep in sync.
#define TUN_MAX_MSG_BYTES 8192

// TOTAL free DMA is the burst margin: roughly two 8 KiB batches (or ten
// MTU-sized buffers) for the chunk plus concurrent WiFi/lwIP churn. Keep this
// at the result-queue boundary, before every control chunk, tunnel batch and
// ping. It is not a contiguous-allocation requirement or a reservation.
static constexpr size_t NOISE_TX_DMA_RESERVE_BYTES = 16 * 1024;

// LARGEST free DMA block is a separate, much smaller allocation floor. C5 AES
// allocates heap_caps_aligned_calloc(8, n * 2, sizeof(crypto_dma_desc_t), DMA):
// even a maximum 65,519-byte Noise plaintext needs ceil(65519 / 4080) = 17
// descriptors per direction, or 17 * 2 * 12 = 408 bytes. Budget another 512
// bytes for the PSA driver's malloc'ed esp_gcm_context and 64 for alignment /
// allocator overhead: 2 * (408 + 512 + 64) = 1968, rounded up to 2 KiB.
// Our 8 KiB BodyChunks need fewer descriptors. This floor complements the
// 16 KiB TOTAL-free burst margin; never reuse that reserve as a block size.
// Neither sample prevents concurrent allocations between the check and seal.
static constexpr size_t NOISE_TX_DMA_CONTIGUOUS_BYTES = 2 * 1024;

// A largest-block query walks the heap: use it for control chunks and the
// infrequent ping. Tunnel batches retain the cheap total-free hot-path gate;
// they (and stream opens) still have a fragmentation gap. A failed seal must
// tear down the session; largest-block diagnostics help identify that case.
static constexpr uint64_t TUN_TX_HEAP_LOG_INTERVAL_US = 5 * 1000 * 1000;
static uint64_t s_next_tx_heap_log_us = 0;  // session-task-owned

extern "C" bool noise_tx_has_dma_headroom(size_t *dma_free) {
    size_t available = heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (dma_free) *dma_free = available;
    return available >= NOISE_TX_DMA_RESERVE_BYTES;
}

#if CONFIG_MUSE_ENABLED
extern "C" bool noise_tx_has_dma_headroom_reclaiming(size_t reclaimable) {
    return heap_caps_get_free_size(MALLOC_CAP_DMA) + reclaimable
        >= NOISE_TX_DMA_RESERVE_BYTES;
}
#endif

extern "C" bool noise_tx_has_contiguous_dma_headroom(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA)
        >= NOISE_TX_DMA_CONTIGUOUS_BYTES;
}

// Tunnel-stream keepalive sentinel: a single reserved byte sent as the ping
// BodyChunk (and echoed as the pong). An EMPTY BodyChunk can't be used — the
// server-side noise-transport dispatcher and the ingress bridge both drop
// zero-length body frames, so an empty ping never reaches the VM backend.
// One byte is unambiguous vs a real packet batch, whose inner framing is
// [u16-LE len][pkt]... (always >= 2 bytes). Must match the VM constant
// the remote tunnel service's keepalive sentinel.
#define TUN_KEEPALIVE_SENTINEL 0x00

// How often to retry opening the tunnel stream while it is down but the
// control session is up (e.g. after a transient backend 503).
#define TUN_REOPEN_INTERVAL_US (5 * 1000 * 1000)

// Tunnel-stream keepalive. The WebSocket ping/pong keepalive in noise_control
// only proves the device↔ingress-rev-proxy transport is alive; it says nothing
// about the VM-side tunnel backend behind raw.sock. That backend
// can go away — e.g. a service restart/redeploy — while ingress-rev-proxy keeps
// the multiplexed Noise stream open, leaving the tunnel HALF-OPEN: the WS is
// healthy so we get no stream reset, but no packets flow and we never reconnect.
//
// To detect that, when the tunnel has been idle we send a zero-length BodyChunk
// (a ping); the tunnel backend echoes a zero-length pong (which the bridge relays
// as an empty batch). Any inbound tunnel frame — a pong or real data — refreshes
// liveness. If nothing arrives for TUN_KEEPALIVE_TIMEOUT_US (several missed
// pings), we declare the tunnel dead so noise_tunnel_maybe_reopen reconnects it.
#define TUN_KEEPALIVE_INTERVAL_US (20 * 1000 * 1000)  // ping after 20s idle
#define TUN_KEEPALIVE_TIMEOUT_US  (65 * 1000 * 1000)  // dead after ~3 missed

#if CONFIG_SPIRAM
#define TX_POOL_SIZE 8
#else
#define TX_POOL_SIZE 4
#endif

struct tx_slot {
    uint8_t *buf;   // TUN_MAX_MSG_BYTES, PSRAM-preferred
    size_t len;
};

static noise_tunnel_packet_cb s_packet_cb = nullptr;
static std::atomic_bool s_connected{false};
static uint64_t s_last_open_attempt_us = 0;

// Tunnel-stream keepalive liveness (see TUN_KEEPALIVE_* above). Both are set
// when the stream opens and updated on the session task, so no locking is
// needed: s_last_rx_us on every inbound tunnel frame, s_last_ping_us when we
// emit a ping.
static uint64_t s_last_rx_us = 0;
static uint64_t s_last_ping_us = 0;

// Outbound batch pool: send_packet (any task) fills a free slot and posts it
// to s_work_q; the session task drains s_work_q and returns slots to s_free_q.
static QueueHandle_t s_work_q = nullptr;  // tx_slot*
static QueueHandle_t s_free_q = nullptr;  // tx_slot*
static tx_slot s_slots[TX_POOL_SIZE];
static bool s_pool_ready = false;

static void ensure_pool() {
    if (s_pool_ready) return;
    s_work_q = xQueueCreate(TX_POOL_SIZE, sizeof(tx_slot *));
    s_free_q = xQueueCreate(TX_POOL_SIZE, sizeof(tx_slot *));
    if (!s_work_q || !s_free_q) {
        ESP_LOGE(TAG, "tunnel queue alloc failed");
        return;
    }
    for (int i = 0; i < TX_POOL_SIZE; i++) {
        s_slots[i].buf = static_cast<uint8_t *>(
            heap_caps_malloc(TUN_MAX_MSG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_slots[i].buf) {
            s_slots[i].buf = static_cast<uint8_t *>(malloc(TUN_MAX_MSG_BYTES));
        }
        s_slots[i].len = 0;
        // Only enqueue slots that actually allocated a buffer — otherwise
        // send_packet would later memcpy into a NULL buf. A short pool just
        // means fewer in-flight batches, not a crash.
        if (s_slots[i].buf) {
            tx_slot *p = &s_slots[i];
            xQueueSend(s_free_q, &p, 0);
        } else {
            ESP_LOGW(TAG, "tunnel TX slot %d alloc failed; pool shortened", i);
        }
    }
    s_pool_ready = true;
}

// ---- Public API -------------------------------------------------------------

extern "C" void noise_tunnel_set_packet_cb(noise_tunnel_packet_cb cb) {
    s_packet_cb = cb;
}

extern "C" bool noise_tunnel_send_packet(const uint8_t *data, size_t len) {
    if (!s_connected.load(std::memory_order_acquire)
        || !s_work_q || !s_free_q) return false;
    if (len == 0 || len > TUN_MAX_MSG_BYTES) return false;

    tx_slot *slot = nullptr;
    if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) return false;  // pool full
    memcpy(slot->buf, data, len);
    slot->len = len;
    if (xQueueSend(s_work_q, &slot, 0) != pdTRUE) {
        xQueueSend(s_free_q, &slot, 0);
        return false;
    }
    return true;
}

static tx_slot *slot_for(uint8_t *buf) {
    for (int i = 0; i < TX_POOL_SIZE; i++) {
        if (s_slots[i].buf == buf) return &s_slots[i];
    }
    return nullptr;
}

extern "C" uint8_t *noise_tunnel_acquire_batch(size_t *cap) {
    if (!s_connected.load(std::memory_order_acquire)
        || !s_work_q || !s_free_q) return nullptr;
    tx_slot *slot = nullptr;
    if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) return nullptr;  // pool full
    slot->len = 0;
    if (cap) *cap = TUN_MAX_MSG_BYTES;
    return slot->buf;
}

extern "C" bool noise_tunnel_commit_batch(uint8_t *buf, size_t len) {
    tx_slot *slot = slot_for(buf);
    if (!slot) return false;
    // A batch committed after the stream dropped is stale: return it.
    if (len == 0 || len > TUN_MAX_MSG_BYTES
        || !s_connected.load(std::memory_order_acquire)) {
        xQueueSend(s_free_q, &slot, 0);
        return false;
    }
    slot->len = len;
    if (xQueueSend(s_work_q, &slot, 0) != pdTRUE) {
        xQueueSend(s_free_q, &slot, 0);
        return false;
    }
    return true;
}

extern "C" void noise_tunnel_abort_batch(uint8_t *buf) {
    tx_slot *slot = slot_for(buf);
    if (slot) xQueueSend(s_free_q, &slot, 0);
}

extern "C" bool noise_tunnel_is_connected(void) {
    return s_connected.load(std::memory_order_relaxed);
}

// ---- Session-owner hooks ----------------------------------------------------

// Attempt to open the tunnel stream and mark it live. Records the attempt time
// for rate-limiting retries.
static bool try_open_stream(const noise_tunnel_emit_t *emit) {
    s_last_open_attempt_us = esp_timer_get_time();
    if (!emit->open_stream(emit->ctx)) {
        ESP_LOGW(TAG, "failed to open /link-tunnel stream");
        return false;
    }
    // Drop any stale batches queued from a previous session/stream.
    if (s_work_q && s_free_q) {
        tx_slot *stale = nullptr;
        while (xQueueReceive(s_work_q, &stale, 0) == pdTRUE) {
            xQueueSend(s_free_q, &stale, 0);
        }
    }
    s_connected.store(true, std::memory_order_release);
    // Arm keepalive liveness from the open: treat the successful open as the
    // first "rx" so we don't immediately time out or ping before any traffic.
    uint64_t now = esp_timer_get_time();
    s_last_rx_us = now;
    s_last_ping_us = now;
    ESP_LOGI(TAG, "tunnel stream open (stream %d)", TUNNEL_STREAM_ID);
    return true;
}

extern "C" bool noise_tunnel_on_session_up(const noise_tunnel_emit_t *emit) {
    ensure_pool();
    if (!emit || !emit->open_stream) return false;
    return try_open_stream(emit);
}

extern "C" void noise_tunnel_maybe_reopen(const noise_tunnel_emit_t *emit) {
    if (s_connected.load(std::memory_order_relaxed)
        || !emit || !emit->open_stream) return;
    uint64_t now = esp_timer_get_time();
    if (now - s_last_open_attempt_us < TUN_REOPEN_INTERVAL_US) return;
    ESP_LOGI(TAG, "retrying tunnel stream open");
    try_open_stream(emit);
}

extern "C" void noise_tunnel_on_session_down(void) {
    s_connected.store(false, std::memory_order_relaxed);
}

extern "C" void noise_tunnel_on_inbound(const uint8_t *data, size_t len) {
    // Any inbound tunnel frame — a real packet batch OR a keepalive pong —
    // proves the VM backend is alive, so refresh liveness first.
    s_last_rx_us = esp_timer_get_time();
    // A single sentinel byte is the keepalive pong, not a packet batch — its
    // liveness is already recorded above; do not forward it to the netif (a real
    // batch is always >= 2 bytes: [u16-LE len][pkt]...).
    if (len == 1 && data && data[0] == TUN_KEEPALIVE_SENTINEL) {
        return;
    }
    if (s_packet_cb && data && len > 0) {
        s_packet_cb(data, len);
    }
}

extern "C" int noise_tunnel_pump_tx(const noise_tunnel_emit_t *emit) {
    if (!s_connected.load(std::memory_order_relaxed)
        || !s_work_q || !emit || !emit->send_body) return 0;

    int sent = 0;
    tx_slot *slot = nullptr;
    // Bound this turn even if producers keep refilling the pool. Returning on
    // pressure leaves queued batches intact and lets the session loop service
    // control traffic, RX and keepalives; never sleep or retry a failed seal.
    for (UBaseType_t pending = uxQueueMessagesWaiting(s_work_q); pending > 0; --pending) {
        size_t dma_free;
        if (!noise_tx_has_dma_headroom(&dma_free)) {
            uint64_t now = esp_timer_get_time();
            if (now >= s_next_tx_heap_log_us) {
                s_next_tx_heap_log_us = now + TUN_TX_HEAP_LOG_INTERVAL_US;
                ESP_LOGW(TAG, "tunnel TX paused: DMA headroom (dma_free=%u dma_largest=%u)",
                         (unsigned)dma_free,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            }
            break;
        }
        if (xQueueReceive(s_work_q, &slot, 0) != pdTRUE) break;
        bool ok = emit->send_body(emit->ctx, slot->buf, slot->len);
        xQueueSend(s_free_q, &slot, 0);
        if (!ok) {
            ESP_LOGW(TAG, "tunnel send_body failed (dma_free=%u dma_largest=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            return -1;
        }
        sent++;
    }
    return sent;
}

extern "C" void noise_tunnel_tick(const noise_tunnel_emit_t *emit) {
    if (!s_connected.load(std::memory_order_relaxed)
        || !emit || !emit->send_body) return;
    uint64_t now = esp_timer_get_time();

    // Half-open detection: no inbound tunnel frame (data or pong) for the whole
    // timeout window suggests the VM backend is gone even though the WS/Noise
    // transport is fine. Drop the tunnel so maybe_reopen re-establishes it.
    // If DMA pressure gates pings during outbound-only traffic for 65s, this
    // can also mark a healthy tunnel down; reopen drops queued batches. That
    // loss is acceptable and self-healing, and preserves half-open detection
    // without requiring a send under pressure.
    if (now - s_last_rx_us >= TUN_KEEPALIVE_TIMEOUT_US) {
        ESP_LOGW(TAG, "tunnel keepalive timeout (%llus idle); marking down",
                 (unsigned long long)((now - s_last_rx_us) / 1000000));
        s_connected.store(false, std::memory_order_relaxed);
        return;
    }

    // Send a keepalive ping once per interval while the tunnel is otherwise
    // idle. The ping is a single-byte sentinel BodyChunk (NOT empty — empty body
    // frames are dropped by the server dispatcher/bridge before the backend sees
    // them). The tunnel backend echoes it as a sentinel pong, which refreshes
    // s_last_rx_us above. Rate-limited to one ping per interval; real inbound
    // traffic implicitly proves liveness without needing a ping.
    if (now - s_last_ping_us >= TUN_KEEPALIVE_INTERVAL_US
        && now - s_last_rx_us >= TUN_KEEPALIVE_INTERVAL_US) {
        // The ping seals through the same AES DMA path as data. Leave the
        // timestamp unchanged while gated so the next tick can retry.
        if (!noise_tx_has_dma_headroom(nullptr)
            || !noise_tx_has_contiguous_dma_headroom()) return;
        s_last_ping_us = now;
        const uint8_t ping = TUN_KEEPALIVE_SENTINEL;
        if (!emit->send_body(emit->ctx, &ping, 1)) {
            // A failed send is a live signal the stream is broken; drop now
            // rather than waiting out the full timeout.
            ESP_LOGW(TAG, "tunnel keepalive ping failed; marking down (dma_free=%u dma_largest=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            s_connected.store(false, std::memory_order_relaxed);
        }
    }
}
