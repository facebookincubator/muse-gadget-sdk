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

// lwIP input hook for tunnel builds, compiled into ESP-IDF's lwIP as
// ESP_IDF_LWIP_HOOK_FILENAME (main/CMakeLists.txt); lwIP calls it before NAPT
// (ip4_input). It drops two kinds of packet from the LAN:
// - too short for NAPT: ESP-IDF's NAPT rewrites the ports and checksum of a
//   TCP, UDP or ICMP packet addressed to the station before lwIP checks that
//   its header is there, so it would read and write past the packet's end;
// - addressed to the tunnel's own subnet: no LAN host has reason to send one,
//   and lwIP would route it to the VM, or to a listener on the tunnel side,
//   with whatever source it claims. NAPT's rewrite of replies to the VM comes
//   after the hook, so those still pass;
// - TCP for a TCP forward (net.forward) from a source that isn't a device on
//   the home network.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#if IP_NAPT && IP_NAPT_PORTMAP
#include "lwip/lwip_napt.h"
#include "lwip/prot/ip.h"
#endif

// Whether NAPT could reach past a packet's first buffer: a TCP, UDP or ICMP
// packet to `station` whose header isn't all in it, fragments included.
// pkt holds first_len bytes; station is in network order. Anything else is
// left to ip4_input.
static inline bool tunnel_napt_input_short(const uint8_t *pkt, uint16_t first_len,
                                           uint32_t station) {
    if (first_len < 20 || (pkt[0] >> 4) != 4) return false;
    uint16_t hlen = (uint16_t)((pkt[0] & 0x0f) * 4);
    uint16_t total = (uint16_t)(pkt[2] << 8 | pkt[3]);
    uint32_t dest;
    memcpy(&dest, pkt + 16, sizeof(dest));
    if (hlen < 20 || dest != station) return false;
    uint16_t need;
    switch (pkt[9]) {
        case 6: need = 20; break;    // TCP
        case 17: need = 8; break;    // UDP
        case 1: need = 8; break;     // ICMP echo
        default: return false;
    }
    return hlen + need > total || hlen + need > first_len;
}

// Whether dest, in a packet that came in on inp, is in the subnet of a netif
// NAPT hides (the tunnel's) while inp isn't one. inp's own address never is,
// even on a LAN that overlaps the tunnel. Network order.
static inline bool tunnel_napt_input_inside(const struct netif *inp, uint32_t dest) {
    if (inp->napt || dest == ip4_addr_get_u32(netif_ip4_addr(inp))) return false;
    struct netif *n;
    NETIF_FOREACH(n) {
        uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(n));
        if (n->napt && mask && ((dest ^ ip4_addr_get_u32(netif_ip4_addr(n))) & mask) == 0)
            return true;
    }
    return false;
}

#if IP_NAPT && IP_NAPT_PORTMAP
// Whether pkt, which came in on inp, is TCP to the station for a TCP forward
// from a source that isn't a host on inp's subnet outside the tunnel's: a
// forward is for devices on the home network, as a UDP forward's relay port
// is. Call it only once tunnel_napt_input_short() has passed pkt.
static inline bool tunnel_napt_input_foreign(const struct netif *inp, const uint8_t *pkt) {
    uint16_t hlen = (uint16_t)((pkt[0] & 0x0f) * 4);
    uint32_t src, dest;
    memcpy(&src, pkt + 12, sizeof(src));
    memcpy(&dest, pkt + 16, sizeof(dest));
    uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(inp));
    uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(inp));
    u32_t maddr, daddr;
    u16_t dport;
    if (inp->napt || pkt[9] != IP_PROTO_TCP || hlen < 20 || dest != ip
        || !ip_portmap_get(IP_PROTO_TCP, (u16_t)(pkt[hlen + 2] << 8 | pkt[hlen + 3]),
                           &maddr, &daddr, &dport)) {
        return false;
    }
    return !mask || src == ip || ((src ^ ip) & mask) != 0 || tunnel_napt_input_inside(inp, src);
}
#endif

static inline int tunnel_napt_input_guard(struct pbuf *p, struct netif *inp) {
    const uint8_t *pkt = (const uint8_t *)p->payload;
    if (!inp || p->len < 20 || (pkt[0] >> 4) != 4) return 0;   // ip4_input drops these
    uint32_t dest;
    memcpy(&dest, pkt + 16, sizeof(dest));
    if (!tunnel_napt_input_short(pkt, p->len, ip4_addr_get_u32(netif_ip4_addr(inp)))
        && !tunnel_napt_input_inside(inp, dest)
#if IP_NAPT && IP_NAPT_PORTMAP
        && !tunnel_napt_input_foreign(inp, pkt)
#endif
    ) {
        return 0;
    }
    pbuf_free(p);
    return 1;
}

#define LWIP_HOOK_IP4_INPUT(p, inp) tunnel_napt_input_guard(p, inp)
