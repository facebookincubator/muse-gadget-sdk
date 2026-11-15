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

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "link_events.h"

static const char *TAG = "link.noise_tun";

// Max batch payload = tunnel_netif BATCH_MAX_BYTES. Keep in sync.
#define TUN_MAX_MSG_BYTES 8192

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

#if CONFIG_HOMEHUB_DAEMON_REQUESTS
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
// TUN_KEEPALIVE_TIMEOUT_US lives in noise_tunnel_internal.h: the session loop
// uses the same window to decide that the whole connection is dead.
#define TUN_KEEPALIVE_INTERVAL_US (20 * 1000 * 1000)  // ping after 20s idle

// Outbound stall. Keepalive only watches inbound frames, so while the VM keeps
// sending (e.g. TCP retransmits) a tunnel whose batches never leave because the
// DMA gate stays shut looks alive forever. If queued batches make no progress
// for this long, tear the session down: reopening the stream would itself wait
// on the gate and frees nothing, while a teardown releases the TLS buffers and
// socket pbufs held in DMA-capable RAM.
#define TUN_TX_STALL_TIMEOUT_US (30 * 1000 * 1000)
// Shorter outbound stalls that recover on their own still go in the
// connectivity history once they last this long.
#define TUN_TX_BLOCKED_RECORD_US (5 * 1000 * 1000)

#if CONFIG_SPIRAM
#define TX_POOL_SIZE 8
#else
#define TX_POOL_SIZE 4
#endif

struct tx_slot {
    uint8_t *base;  // headroom + TUN_MAX_MSG_BYTES + tailroom, PSRAM-preferred
    uint8_t *buf;   // the batch: base + NOISE_TUNNEL_TX_HEADROOM
    size_t len;
};

#define TX_SLOT_ALLOC \
    (NOISE_TUNNEL_TX_HEADROOM + TUN_MAX_MSG_BYTES + NOISE_TUNNEL_TX_TAILROOM)

static noise_tunnel_packet_cb s_packet_cb = nullptr;
static std::atomic_bool s_connected{false};
static uint64_t s_last_open_attempt_us = 0;
// Set when we mark the tunnel down without the server knowing (keepalive
// timeout, failed ping), so the server still tracks TUNNEL_STREAM_ID and would
// reject a reopen on it as "duplicate id". Session-task-owned.
static bool s_needs_reset = false;

// Tunnel-stream keepalive liveness (see TUN_KEEPALIVE_* above). Both are set
// when the stream opens and updated on the session task, so no locking is
// needed: s_last_rx_us on every inbound tunnel frame, s_last_ping_us when we
// emit a ping.
static uint64_t s_last_rx_us = 0;
static uint64_t s_last_ping_us = 0;
// s_last_rx_us in whole seconds for readers on other tasks (bug.report): a
// 64-bit read can tear on the 32-bit C5. UINT32_MAX until the first open.
static std::atomic<uint32_t> s_last_rx_s{UINT32_MAX};
// Send time of the oldest ping with no inbound frame since; 0 when none is
// outstanding. Session-task-owned.
static uint64_t s_unanswered_ping_us = 0;
// Set by a keepalive timeout to s_unanswered_ping_us if that ping had
// TUN_PING_ANSWER_US to be answered, else 0; consumed by
// noise_tunnel_take_unanswered_timeout(). Session-task-owned.
static uint64_t s_timeout_ping_us = 0;

// An inbound tunnel frame, or a stream open, which counts as one.
static void note_tunnel_rx(uint64_t now) {
    s_last_rx_us = now;
    s_last_rx_s.store((uint32_t)(now / 1000000), std::memory_order_relaxed);
    s_unanswered_ping_us = 0;
}

// Start of the current outbound stall (batches queued, DMA-gated, none sent).
// Session-task-owned, cleared by any send, an empty queue or a stream open.
static bool s_tx_stalled = false;
static uint64_t s_tx_stalled_since_us = 0;
static size_t s_tx_stall_min_dma = 0;

// Connectivity history (link_events), session-task-owned. An outage runs from
// the first mark-down to the first inbound frame on a reopened stream: a
// refused reopen is reset milliseconds after the open, so the open proves
// nothing. A reopen loop thus records one tunnel_down/tunnel_up pair, with the
// attempts, DMA wait and last failure summarized on recovery.
static uint64_t s_outage_since_us = 0;  // 0: no outage in progress
static bool s_rx_since_open = false;
static uint32_t s_outage_reopens = 0;
static uint64_t s_outage_dma_wait_us = 0;
static uint64_t s_reopen_gated_since_us = 0;
static char s_outage_last_failure[TUNNEL_FAILURE_MAX] = "";

// `why` is null when the session ended; its session_down event says why.
static void note_tunnel_down(const char *why) {
    if (s_outage_since_us) {
        if (why) snprintf(s_outage_last_failure, sizeof(s_outage_last_failure), "%s", why);
        return;
    }
    s_outage_since_us = std::max<uint64_t>(esp_timer_get_time(), 1);
    s_outage_reopens = 0;
    s_outage_dma_wait_us = 0;
    s_reopen_gated_since_us = 0;
    s_outage_last_failure[0] = '\0';
    if (why) link_events_record(LINK_EVENT_TUNNEL_DOWN, "%s", why);
}

static void note_tunnel_up(uint64_t now) {
    if (!s_outage_since_us) {
        link_events_record(LINK_EVENT_TUNNEL_UP, "opened");
        return;
    }
    unsigned down_s = (unsigned)((now - s_outage_since_us) / 1000000);
    ESP_LOGI(TAG, "tunnel restored after %us down (%u reopen attempts)",
             down_s, (unsigned)s_outage_reopens);
    link_events_record(LINK_EVENT_TUNNEL_UP,
                       "back after %us: %u reopens, %us DMA wait%s%s",
                       down_s, (unsigned)s_outage_reopens,
                       (unsigned)(s_outage_dma_wait_us / 1000000),
                       s_outage_last_failure[0] ? ", last fail: " : "",
                       s_outage_last_failure);
    s_outage_since_us = 0;
}

static void end_tx_stall(uint64_t now, const char *outcome) {
    if (s_tx_stalled && now - s_tx_stalled_since_us >= TUN_TX_BLOCKED_RECORD_US) {
        unsigned blocked_s = (unsigned)((now - s_tx_stalled_since_us) / 1000000);
        ESP_LOGI(TAG, "tunnel TX blocked %us on DMA headroom (min dma_free=%u); %s",
                 blocked_s, (unsigned)s_tx_stall_min_dma, outcome);
        link_events_record(LINK_EVENT_TUNNEL_TX_BLOCKED,
                           "outbound blocked %us on DMA headroom (min dma_free=%u); %s",
                           blocked_s, (unsigned)s_tx_stall_min_dma, outcome);
    }
    s_tx_stalled = false;
}

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
        s_slots[i].base = static_cast<uint8_t *>(
            heap_caps_malloc(TX_SLOT_ALLOC, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_slots[i].base) {
            s_slots[i].base = static_cast<uint8_t *>(malloc(TX_SLOT_ALLOC));
        }
        s_slots[i].buf = s_slots[i].base
            ? s_slots[i].base + NOISE_TUNNEL_TX_HEADROOM : nullptr;
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
    uint64_t now = esp_timer_get_time();
    end_tx_stall(now, "queue dropped by reopen");
    s_rx_since_open = false;
    s_connected.store(true, std::memory_order_release);
    // Arm keepalive liveness from the open: treat the successful open as the
    // first "rx" so we don't immediately time out or ping before any traffic.
    note_tunnel_rx(now);
    s_last_ping_us = now;
    ESP_LOGI(TAG, "tunnel stream open (stream %d)", TUNNEL_STREAM_ID);
    return true;
}

extern "C" bool noise_tunnel_on_session_up(const noise_tunnel_emit_t *emit) {
    ensure_pool();
    s_needs_reset = false;
    if (!emit || !emit->open_stream) return false;
    return try_open_stream(emit);
}

extern "C" bool noise_tunnel_maybe_reopen(const noise_tunnel_emit_t *emit) {
    if (s_connected.load(std::memory_order_relaxed)
        || !emit || !emit->open_stream || !emit->reset_stream) return true;
    uint64_t now = esp_timer_get_time();
    if (now - s_last_open_attempt_us < TUN_REOPEN_INTERVAL_US) return true;
    // Reset + Request seal through the AES DMA path; a failed seal would turn
    // a tunnel blip into a full reconnect. Retry next turn once pressure clears.
    if (!noise_tx_has_dma_headroom(nullptr)
        || !noise_tx_has_contiguous_dma_headroom()) {
        if (!s_reopen_gated_since_us) {
            s_reopen_gated_since_us = now;
        } else if (now - s_reopen_gated_since_us >= TUN_TX_STALL_TIMEOUT_US) {
            // Control traffic keeps the session alive, so waiting longer could
            // leave the tunnel down indefinitely. As with a TX stall, a
            // teardown releases the TLS buffers and socket pbufs held in
            // DMA-capable RAM.
            ESP_LOGW(TAG, "tunnel reopen blocked %llus on DMA headroom; tearing down session "
                     "(dma_free=%u dma_largest=%u)",
                     (unsigned long long)((now - s_reopen_gated_since_us) / 1000000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            s_outage_dma_wait_us += now - s_reopen_gated_since_us;
            s_reopen_gated_since_us = 0;
            return false;
        }
        return true;
    }
    if (s_reopen_gated_since_us) {
        s_outage_dma_wait_us += now - s_reopen_gated_since_us;
        s_reopen_gated_since_us = 0;
    }
    ++s_outage_reopens;
    ESP_LOGI(TAG, "retrying tunnel stream open");
    s_last_open_attempt_us = now;
    // The daemon dispatcher never reaps a stream we abandoned locally, so
    // without this Reset every reopen on the reused id is refused.
    if (s_needs_reset) {
        if (!emit->reset_stream(emit->ctx)) {
            // Some Reset failures leave Noise established; reopening without
            // the Reset can never succeed, so the session must be torn down.
            ESP_LOGW(TAG, "tunnel stream reset failed; tearing down session");
            return false;
        }
        s_needs_reset = false;
    }
    try_open_stream(emit);
    return true;
}

extern "C" uint32_t noise_tunnel_rx_idle_s(void) {
    uint32_t last_rx_s = s_last_rx_s.load(std::memory_order_relaxed);
    if (last_rx_s == UINT32_MAX) return UINT32_MAX;
    return (uint32_t)(esp_timer_get_time() / 1000000) - last_rx_s;
}

extern "C" uint64_t noise_tunnel_unanswered_ping_us(void) {
    return s_unanswered_ping_us;
}

extern "C" uint64_t noise_tunnel_take_unanswered_timeout(void) {
    uint64_t ping_us = s_timeout_ping_us;
    s_timeout_ping_us = 0;
    return ping_us;
}

extern "C" void noise_tunnel_on_session_down(void) {
    s_connected.store(false, std::memory_order_relaxed);
    s_unanswered_ping_us = 0;
    s_timeout_ping_us = 0;
    end_tx_stall(esp_timer_get_time(), "session ended");
    s_reopen_gated_since_us = 0;
    note_tunnel_down(nullptr);
}

extern "C" void noise_tunnel_on_server_drop(const char *why) {
    s_connected.store(false, std::memory_order_relaxed);
    note_tunnel_down(why);
}

extern "C" void noise_tunnel_on_inbound(const uint8_t *data, size_t len) {
    // Any inbound tunnel frame — a real packet batch OR a keepalive pong —
    // proves the VM backend is alive, so refresh liveness first.
    note_tunnel_rx(esp_timer_get_time());
    if (!s_rx_since_open) {
        s_rx_since_open = true;
        note_tunnel_up(s_last_rx_us);
    }
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
    UBaseType_t pending = uxQueueMessagesWaiting(s_work_q);
    if (pending == 0) end_tx_stall(esp_timer_get_time(), "queue drained");
    // Bound this turn even if producers keep refilling the pool. Returning on
    // pressure leaves queued batches intact and lets the session loop service
    // control traffic, RX and keepalives; never sleep or retry a failed seal.
    for (; pending > 0; --pending) {
        size_t dma_free;
        if (!noise_tx_has_dma_headroom(&dma_free)) {
            uint64_t now = esp_timer_get_time();
            if (!s_tx_stalled) {
                s_tx_stalled = true;
                s_tx_stalled_since_us = now;
                s_tx_stall_min_dma = dma_free;
            } else if (now - s_tx_stalled_since_us >= TUN_TX_STALL_TIMEOUT_US) {
                ESP_LOGW(TAG, "tunnel TX stalled %llus on DMA headroom; tearing down session "
                         "(dma_free=%u dma_largest=%u)",
                         (unsigned long long)((now - s_tx_stalled_since_us) / 1000000),
                         (unsigned)dma_free,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
                // The warning above already logs it; only record the event.
                link_events_record(LINK_EVENT_TUNNEL_TX_BLOCKED,
                                   "outbound blocked %us on DMA headroom (min dma_free=%u); "
                                   "tearing down session",
                                   (unsigned)((now - s_tx_stalled_since_us) / 1000000),
                                   (unsigned)std::min(s_tx_stall_min_dma, dma_free));
                s_tx_stalled = false;
                return -1;
            } else {
                s_tx_stall_min_dma = std::min(s_tx_stall_min_dma, dma_free);
            }
            if (now >= s_next_tx_heap_log_us) {
                s_next_tx_heap_log_us = now + TUN_TX_HEAP_LOG_INTERVAL_US;
                ESP_LOGW(TAG, "tunnel TX paused: DMA headroom (dma_free=%u dma_largest=%u)",
                         (unsigned)dma_free,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            }
            break;
        }
        if (xQueueReceive(s_work_q, &slot, 0) != pdTRUE) break;
        bool ok = emit->send_body_inplace
            ? emit->send_body_inplace(emit->ctx, slot->base, NOISE_TUNNEL_TX_HEADROOM,
                                      slot->len, NOISE_TUNNEL_TX_TAILROOM)
            : emit->send_body(emit->ctx, slot->buf, slot->len);
        xQueueSend(s_free_q, &slot, 0);
        if (!ok) {
            ESP_LOGW(TAG, "tunnel send_body failed (dma_free=%u dma_largest=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            return -1;
        }
        end_tx_stall(esp_timer_get_time(), "resumed");
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
        s_needs_reset = true;
        s_connected.store(false, std::memory_order_relaxed);
        // No ping outstanding means they were DMA-gated, and one sent just
        // before the timeout hasn't had time for a reply: neither proves the
        // connection dead.
        s_timeout_ping_us = s_unanswered_ping_us
                            && now - s_unanswered_ping_us >= TUN_PING_ANSWER_US
                            ? s_unanswered_ping_us : 0;
        s_unanswered_ping_us = 0;
        char why[TUNNEL_FAILURE_MAX];
        snprintf(why, sizeof(why), "keepalive timeout, last ping %us ago",
                 (unsigned)((now - s_last_ping_us) / 1000000));
        note_tunnel_down(why);
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
        if (!s_unanswered_ping_us) s_unanswered_ping_us = now;
        const uint8_t ping = TUN_KEEPALIVE_SENTINEL;
        if (!emit->send_body(emit->ctx, &ping, 1)) {
            // A failed send is a live signal the stream is broken; drop now
            // rather than waiting out the full timeout.
            ESP_LOGW(TAG, "tunnel keepalive ping failed; marking down (dma_free=%u dma_largest=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            s_needs_reset = true;
            s_connected.store(false, std::memory_order_relaxed);
            note_tunnel_down("keepalive ping send failed");
        }
    }
}
