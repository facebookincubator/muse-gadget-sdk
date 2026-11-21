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

#include "lwip/ip4_addr.h"

// Custom L3 netif whose "wire" is the Noise data tunnel carrying IPv4 packet
// batches. Outbound packets from lwIP are flattened and handed to
// noise_tunnel_send_packet(). Inbound packets received over the tunnel are
// handed in via noise_tunnel_set_packet_cb().

// One-time bring-up. Adds the netif to lwIP with the given static IPs.
// Safe to call from any task; serializes onto the TCP/IP thread internally.
// Returns true on success.
bool tunnel_netif_start(const ip4_addr_t *ip,
                        const ip4_addr_t *netmask,
                        const ip4_addr_t *gw);

// Mark the link up or down. lwIP drops outbound packets while down.
void tunnel_netif_set_link(bool up);

// netif index (for diagnostics / NAPT). 0 if not started.
uint8_t tunnel_netif_index(void);

// Atomic counters of packets bridged each direction since boot.
typedef struct {
    uint32_t rx_pkts;     // raw tunnel -> lwIP
    uint32_t rx_bytes;
    uint32_t tx_pkts;     // lwIP -> raw tunnel (queued)
    uint32_t tx_bytes;
    uint32_t tx_dropped;  // queue full / oversize / alloc fail
} tunnel_stats_t;

void tunnel_netif_get_stats(tunnel_stats_t *out);
