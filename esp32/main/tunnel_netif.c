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

#include "tunnel_netif.h"
#include "stack_monitor.h"

#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/tcpip.h"
#include "lwip/lwip_napt.h"

#include "noise_tunnel.h"

static const char *TAG = "link.tun";

#define TUNNEL_MTU 1280
// Outbound queue depth. With pre-allocated PSRAM slots this is a permanent
// reservation of DEPTH * TX_SLOT_SIZE bytes — but it lives in PSRAM so it
// doesn't compete with internal SRAM that WiFi / lwIP / mbedtls need.
// 128 * 1500 = 192 KB in PSRAM (8 MB available). Sized to absorb a
// full TCP-window worth of packets plus end-of-burst tail. On targets
// without PSRAM (e.g. ESP32-C5 DevKit) drop to 32 slots = 48 KB so we
// don't starve internal SRAM.
#if CONFIG_SPIRAM
#define TX_QUEUE_DEPTH 128
#else
// Without PSRAM these slots eat internal SRAM that mbedtls / WiFi need.
// 16 * 1500 = 24 KB. Below this the burst tail starts dropping packets,
// but better that than starving the TLS handshake.
#define TX_QUEUE_DEPTH 16
#endif
#define TX_SLOT_SIZE   1500
#define TX_TASK_STACK  4096
#define TX_TASK_PRIO   5

// Batching parameters. We pack many IP packets into one tunnel batch so the
// per-record mbedtls cost is paid once for ~8 KB instead of once per ~1.3 KB
// packet. Wire format inside the batch (one BodyChunk on the Noise tunnel
// stream; Noise message boundaries delimit batches, so there is no outer
// length envelope):
//   [u16 LE length][packet bytes][u16 LE length][packet bytes]...
// A single packet in a frame is just [len][pkt] — both sides parse the
// same way. Length is little-endian (native on C5 RISC-V + the Linux VM).
#define BATCH_MAX_BYTES    8192
#define BATCH_MAX_PACKETS  16
#define BATCH_FLUSH_MS     5             // age cap for the oldest packet

typedef struct {
    uint8_t *buf;
    uint16_t len;
} tx_item_t;

static struct netif s_netif;
static bool s_started = false;
static QueueHandle_t s_tx_queue = NULL;       // tx_item_t* (filled work)
static QueueHandle_t s_tx_free_queue = NULL;  // tx_item_t* (free pool)
static tx_item_t s_tx_slots[TX_QUEUE_DEPTH];
static TaskHandle_t s_tx_task = NULL;
typedef struct {
    _Atomic uint32_t rx_pkts;
    _Atomic uint32_t rx_bytes;
    _Atomic uint32_t tx_pkts;
    _Atomic uint32_t tx_bytes;
    _Atomic uint32_t tx_dropped;
} atomic_tunnel_stats_t;

static atomic_tunnel_stats_t s_stats = {0};

// Drain the TX queue, batching multiple IP packets into one tunnel batch, and
// queue it for the session task to send as a BodyChunk on the tunnel stream.
// The batch is packed straight into a noise_tunnel pool buffer (PSRAM), so
// there is no staging copy and no internal-RAM batch buffer. The tunnel
// writer runs on its own task, so lwIP's TCP/IP task only ever copies one
// packet into a slot (tunnel_output) and never blocks on the session.
//
// Batching policy: pack up to BATCH_MAX_PACKETS packets (or BATCH_MAX_BYTES
// of payload+length-prefix) into one message. Flush early when the first
// queued packet is older than BATCH_FLUSH_MS — keeps latency bounded for
// thin traffic like a stray ping while letting bulk transfers coalesce.
// With no free batch buffer (the session is behind), packets are dropped and
// counted, as a full send pool did before; TCP backs off.
static void tx_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    tx_item_t *item;
    for (;;) {
        // Block indefinitely for the first packet.
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        size_t cap = 0;
        uint8_t *batch = noise_tunnel_acquire_batch(&cap);
        if (cap > BATCH_MAX_BYTES) cap = BATCH_MAX_BYTES;
        size_t batch_len = 0;
        int batch_count = 0;
        TickType_t batch_start = xTaskGetTickCount();

        // Append the just-received packet, then opportunistically drain
        // more with a small wait so we coalesce without idling the link.
        while (item) {
            uint16_t plen = item->len;
            if (!batch) {
                atomic_fetch_add_explicit(&s_stats.tx_dropped, 1,
                                          memory_order_relaxed);
            } else if (plen > 0 && (batch_len + 2 + plen) <= cap) {
                // [u16 LE length][packet]
                batch[batch_len++] = (uint8_t)(plen & 0xff);
                batch[batch_len++] = (uint8_t)((plen >> 8) & 0xff);
                memcpy(&batch[batch_len], item->buf, plen);
                batch_len += plen;
                batch_count++;
            }
            xQueueSend(s_tx_free_queue, &item, 0);
            item = NULL;

            if (!batch) break;
            if (batch_count >= BATCH_MAX_PACKETS) break;
            if (batch_len > (cap - TX_SLOT_SIZE - 2)) break;

            // Time-bounded wait for the next packet. 0 ticks = pure peek,
            // 1 tick = wait at least one scheduler tick. Stop coalescing
            // once BATCH_FLUSH_MS has elapsed since the first packet.
            TickType_t age = xTaskGetTickCount() - batch_start;
            TickType_t budget = pdMS_TO_TICKS(BATCH_FLUSH_MS);
            TickType_t wait = (age >= budget) ? 0 : (budget - age);
            if (xQueueReceive(s_tx_queue, &item, wait) != pdTRUE) {
                item = NULL;
                break;
            }
        }

        if (batch) {
            if (batch_len > 0) {
                noise_tunnel_commit_batch(batch, batch_len);
            } else {
                noise_tunnel_abort_batch(batch);
            }
        }
        stack_monitor_poll(&stack);
    }
}

// lwIP IPv4 output: copy the pbuf into a pre-allocated PSRAM slot and enqueue.
// Hot path is malloc-free — under burst, repeated 1.3 KB malloc/free was
// failing even with >100 KB total internal heap free due to fragmentation.
// Actual WS send happens on s_tx_task to avoid lwIP TCP/IP re-entrancy.
static err_t tunnel_output(struct netif *netif, struct pbuf *p,
                           const ip4_addr_t *ipaddr) {
    (void)netif;
    (void)ipaddr;

    if (p->tot_len > TUNNEL_MTU) {
        ESP_LOGW(TAG, "tx drop: %u > MTU", (unsigned)p->tot_len);
        atomic_fetch_add_explicit(&s_stats.tx_dropped, 1, memory_order_relaxed);
        return ERR_BUF;
    }
    if (!s_tx_queue || !s_tx_free_queue) {
        atomic_fetch_add_explicit(&s_stats.tx_dropped, 1, memory_order_relaxed);
        return ERR_IF;
    }

    tx_item_t *item;
    if (xQueueReceive(s_tx_free_queue, &item, 0) != pdTRUE) {
        // Pool exhausted — TX worker can't keep up. Drop, lwIP backs off.
        ESP_LOGW(TAG, "tx pool empty, dropping %u bytes", (unsigned)p->tot_len);
        atomic_fetch_add_explicit(&s_stats.tx_dropped, 1, memory_order_relaxed);
        return ERR_IF;
    }
    if (pbuf_copy_partial(p, item->buf, p->tot_len, 0) != p->tot_len) {
        xQueueSend(s_tx_free_queue, &item, 0);
        atomic_fetch_add_explicit(&s_stats.tx_dropped, 1, memory_order_relaxed);
        return ERR_BUF;
    }
    item->len = (uint16_t)p->tot_len;

    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        // Should never happen — both queues are sized identically.
        xQueueSend(s_tx_free_queue, &item, 0);
        atomic_fetch_add_explicit(&s_stats.tx_dropped, 1, memory_order_relaxed);
        return ERR_IF;
    }
    atomic_fetch_add_explicit(&s_stats.tx_pkts, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &s_stats.tx_bytes, p->tot_len, memory_order_relaxed);
    return ERR_OK;
}

// netif init callback. lwIP guarantees this runs on the TCP/IP thread.
static err_t tunnel_netif_init_cb(struct netif *netif) {
    netif->name[0] = 't';
    netif->name[1] = 'n';
    netif->mtu = TUNNEL_MTU;
    netif->flags = 0;  // start link-down; raw tunnel status raises it
    netif->output = tunnel_output;
    netif->output_ip6 = NULL;
    netif->linkoutput = NULL;
    return ERR_OK;
}

// Noise tunnel RX → lwIP. The BodyChunk payload is one or more packets, each
// prefixed with a little-endian u16 length (inner framing; Noise message
// boundaries delimit batches, so there is no outer envelope to strip). We
// must own a pbuf the stack can free; copy each packet into its own.
// PBUF_LINK (not PBUF_RAW) so lwIP can prepend a 14-byte Ethernet header
// when forwarding through the WiFi STA.
static void on_tunnel_packet(const uint8_t *data, size_t len) {
    if (!s_started || len == 0) return;

    size_t off = 0;
    while (off + 2 <= len) {
        uint16_t plen = (uint16_t)data[off] | ((uint16_t)data[off + 1] << 8);
        off += 2;
        if (plen == 0 || off + plen > len) {
            ESP_LOGW(TAG, "rx malformed frame: off=%u plen=%u total=%u",
                     (unsigned)off, plen, (unsigned)len);
            return;
        }

        struct pbuf *p = pbuf_alloc(PBUF_LINK, plen, PBUF_POOL);
        if (!p) {
            ESP_LOGW(TAG, "rx drop: pbuf_alloc(%u)", plen);
            off += plen;
            continue;
        }
        if (pbuf_take(p, &data[off], plen) != ERR_OK) {
            pbuf_free(p);
            off += plen;
            continue;
        }
        if (tcpip_input(p, &s_netif) != ERR_OK) {
            ESP_LOGW(TAG, "tcpip_input rejected packet");
            pbuf_free(p);
            off += plen;
            continue;
        }
        atomic_fetch_add_explicit(&s_stats.rx_pkts, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &s_stats.rx_bytes, plen, memory_order_relaxed);
        off += plen;
    }
    if (off != len) {
        ESP_LOGW(TAG, "rx trailing bytes: off=%u total=%u",
                 (unsigned)off, (unsigned)len);
    }
}

bool tunnel_netif_start(const ip4_addr_t *ip,
                        const ip4_addr_t *netmask,
                        const ip4_addr_t *gw) {
    if (s_started) return true;

    // A failed start is retried on every reconnect, so each step keeps what
    // an earlier attempt made instead of leaking it and making it again.
    // Both queues store pointers into s_tx_slots[]. The free queue starts
    // full and the work queue starts empty; items shuttle between them.
    if (!s_tx_queue) {
        s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t *));
    }
    if (!s_tx_free_queue) {
        s_tx_free_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t *));
    }
    if (!s_tx_queue || !s_tx_free_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return false;
    }
    // Pre-allocate every slot once. Total cost: TX_QUEUE_DEPTH * TX_SLOT_SIZE
    // bytes (~96 KB at 64*1500). Allocate from PSRAM so we don't eat into
    // internal SRAM that WiFi / lwIP / mbedtls need; falls back to internal
    // SRAM if no PSRAM is configured.
    for (int i = 0; i < TX_QUEUE_DEPTH; i++) {
        if (s_tx_slots[i].buf) continue;
        s_tx_slots[i].buf = heap_caps_malloc(TX_SLOT_SIZE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_tx_slots[i].buf) {
            s_tx_slots[i].buf = malloc(TX_SLOT_SIZE);  // no-PSRAM fallback
        }
        s_tx_slots[i].len = 0;
        if (!s_tx_slots[i].buf) {
            ESP_LOGE(TAG, "tx slot %d alloc failed", i);
            return false;
        }
        tx_item_t *p = &s_tx_slots[i];
        xQueueSend(s_tx_free_queue, &p, 0);
    }
    if (!s_tx_task
        && xTaskCreate(tx_task, "tun_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO,
                       &s_tx_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }

    err_t err = netifapi_netif_add(&s_netif, ip, netmask, gw,
                                   NULL, tunnel_netif_init_cb, tcpip_input);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "netifapi_netif_add failed: %d", err);
        return false;
    }
    netifapi_netif_set_up(&s_netif);
    // Enable NAPT on this (LAN-side) netif. lwIP's NAPT translates packets
    // coming IN on a netif marked napt=1 as they leave via another. Despite
    // the common phrasing "enable NAT on the WAN interface", lwIP's API
    // wants the flag on the *source* netif (see ip_napt_forward()).
    //
    // Must hold the core lock: there is no netifapi_ wrapper for this one, and
    // ip_napt_init() publishes ip_napt_max/ip_portmap_max *before* it callocs
    // the tables. Without the lock, an RX packet arriving in that window (WiFi
    // task runs ip4_input inline under LWIP_TCPIP_CORE_LOCKING_INPUT) passes
    // ip_napt_recv()'s `ip_napt_max == 0` guard and derefs a NULL portmap
    // table, panicking right after link.register goes out. Measured at 3
    // crashes per 20 boots without this lock, 0 per 20 with it.
    LOCK_TCPIP_CORE();
    int napt_enabled = ip_napt_enable_netif(&s_netif, 1);
    UNLOCK_TCPIP_CORE();
    if (napt_enabled != 1) {
        ESP_LOGW(TAG, "ip_napt_enable_netif failed (netif up? %d)",
                 (int)netif_is_up(&s_netif));
    } else {
        ESP_LOGI(TAG, "NAPT enabled on tunnel netif");
    }
    // Link starts down. Caller raises it via tunnel_netif_set_link(true)
    // once the WS transport is ready.

    noise_tunnel_set_packet_cb(on_tunnel_packet);
    s_started = true;
    ESP_LOGI(TAG, "tunnel netif up: ip=%s mtu=%d idx=%u",
             ip4addr_ntoa(ip), TUNNEL_MTU, (unsigned)s_netif.num);
    return true;
}

void tunnel_netif_set_link(bool up) {
    if (!s_started) return;
    if (up) {
        netifapi_netif_set_link_up(&s_netif);
    } else {
        netifapi_netif_set_link_down(&s_netif);
    }
}

uint8_t tunnel_netif_index(void) {
    return s_started ? s_netif.num : 0;
}

void tunnel_netif_get_stats(tunnel_stats_t *out) {
    if (!out) return;
    *out = (tunnel_stats_t){
        .rx_pkts = atomic_load_explicit(&s_stats.rx_pkts, memory_order_relaxed),
        .rx_bytes = atomic_load_explicit(&s_stats.rx_bytes, memory_order_relaxed),
        .tx_pkts = atomic_load_explicit(&s_stats.tx_pkts, memory_order_relaxed),
        .tx_bytes = atomic_load_explicit(&s_stats.tx_bytes, memory_order_relaxed),
        .tx_dropped = atomic_load_explicit(
            &s_stats.tx_dropped, memory_order_relaxed),
    };
}
