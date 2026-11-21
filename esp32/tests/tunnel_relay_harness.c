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

// The tunnel relay's packet sorting and which LAN senders reach the VM
// (tunnel_relay.c), and the check that keeps short packets from NAPT
// (lwip_hooks/tunnel_napt_guard.h), extracted by test_tunnel_relay.py.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "relay_classify.inc"

// Just enough of lwIP's netif for the NAPT input check.
typedef struct { uint32_t addr; } ip4_addr_t;
struct netif { struct netif *next; ip4_addr_t ip, mask; uint8_t napt; };
static struct netif *netif_list;
#define NETIF_FOREACH(n) for ((n) = netif_list; (n) != NULL; (n) = (n)->next)
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_netmask(n) (&(n)->mask)
#define ip4_addr_get_u32(a) ((a)->addr)
#include "napt_guard.inc"

static uint32_t ip(const char *s) { return inet_addr(s); }

// An IPv4/UDP packet from src to dst; returns its length.
static size_t udp(uint8_t *p, const char *src, const char *dst, uint16_t sport, uint16_t dport,
                  const char *data, uint8_t ttl, uint16_t frag) {
    size_t n = strlen(data), total = 28 + n;
    memset(p, 0, 28);
    p[0] = 0x45; p[2] = (uint8_t)(total >> 8); p[3] = (uint8_t)total;
    p[6] = (uint8_t)(frag >> 8); p[7] = (uint8_t)frag; p[8] = ttl; p[9] = 17;
    uint32_t s = ip(src), d = ip(dst);
    memcpy(p + 12, &s, 4); memcpy(p + 16, &d, 4);
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport;
    p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    p[24] = (uint8_t)((8 + n) >> 8); p[25] = (uint8_t)(8 + n);
    memcpy(p + 28, data, n);
    return total;
}

static relay_kind_t sort(const uint8_t *p, size_t n, relay_udp_t *u) {
    // The station is 192.168.4.62/22, the VM 10.42.0.2/24.
    return relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), ip("192.168.4.62"),
                          ip("255.255.252.0"), u);
}

static void check_classify(void) {
    uint8_t p[256];
    relay_udp_t u;
    size_t n = udp(p, "10.42.0.2", "255.255.255.255", 50123, 9999, "scan", 64, 0);
    assert(sort(p, n, &u) == RELAY_BROADCAST);
    assert(u.sport == 50123 && u.dport == 9999 && u.data_len == 4 && !memcmp(u.data, "scan", 4));
    assert(u.dst == ip("255.255.255.255"));
    // The LAN's own broadcast address is a broadcast too.
    n = udp(p, "10.42.0.2", "192.168.7.255", 50123, 137, "q", 64, 0);
    assert(sort(p, n, &u) == RELAY_BROADCAST);
    n = udp(p, "10.42.0.2", "239.255.255.250", 50124, 1900, "M-SEARCH", 4, 0);
    assert(sort(p, n, &u) == RELAY_MULTICAST && u.dport == 1900);
    p[8] = 0;   // TTL 0 goes nowhere
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "192.168.6.100", 4002, 4003, "ctl", 64, 0);
    assert(sort(p, n, &u) == RELAY_UNICAST && u.sport == 4002 && u.dport == 4003);
    // The device itself, the tunnel and the internet stay lwIP's.
    n = udp(p, "10.42.0.2", "192.168.4.62", 4002, 4003, "x", 64, 0);
    assert(sort(p, n, &u) == RELAY_PASS);
    n = udp(p, "10.42.0.2", "10.42.0.1", 4002, 53, "x", 64, 0);
    assert(sort(p, n, &u) == RELAY_PASS);
    n = udp(p, "10.42.0.2", "8.8.8.8", 4002, 53, "x", 64, 0);
    assert(sort(p, n, &u) == RELAY_PASS);
    // Without a station address nothing is on the LAN.
    n = udp(p, "10.42.0.2", "192.168.6.100", 4002, 4003, "x", 64, 0);
    assert(relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), 0, 0, &u) == RELAY_PASS);
    // A /32 has no broadcast address of its own.
    n = udp(p, "10.42.0.2", "192.168.4.62", 4002, 4003, "x", 64, 0);
    assert(relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), ip("192.168.4.62"),
                          0xffffffffu, &u) == RELAY_PASS);
    // DHCP's ports are the home network's own: nothing to or from them is
    // relayed, and unicast stays NAPT's.
    n = udp(p, "10.42.0.2", "255.255.255.255", 67, 68, "offer", 64, 0);
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "255.255.255.255", 68, 67, "discover", 64, 0);
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "192.168.7.255", 50123, 68, "x", 64, 0);
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "224.0.0.251", 67, 5353, "x", 64, 0);
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "192.168.6.100", 68, 67, "renew", 64, 0);
    assert(sort(p, n, &u) == RELAY_PASS);
    // LLMNR is relayed, like mDNS (and NetBIOS's broadcast above).
    n = udp(p, "10.42.0.2", "224.0.0.252", 50125, 5355, "q", 64, 0);
    assert(sort(p, n, &u) == RELAY_MULTICAST);
    // The tunnel's subnet is lwIP's, even on a LAN that overlaps it; the rest
    // of that LAN is relayed as usual.
    n = udp(p, "10.42.0.2", "10.42.0.1", 4002, 4003, "x", 64, 0);
    assert(relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), ip("10.42.0.20"),
                          ip("255.255.0.0"), &u) == RELAY_PASS);
    n = udp(p, "10.42.0.2", "10.42.0.255", 4002, 4003, "x", 64, 0);
    assert(relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), ip("10.42.0.20"),
                          ip("255.255.0.0"), &u) == RELAY_PASS);
    n = udp(p, "10.42.0.2", "10.42.3.4", 4002, 4003, "x", 64, 0);
    assert(relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), ip("10.42.0.20"),
                          ip("255.255.0.0"), &u) == RELAY_UNICAST);
    n = udp(p, "10.42.0.2", "10.42.255.255", 4002, 4003, "x", 64, 0);
    assert(relay_classify(p, n, ip("10.42.0.2"), ip("255.255.255.0"), ip("10.42.0.20"),
                          ip("255.255.0.0"), &u) == RELAY_BROADCAST);
    // Fragments: a broadcast one is dropped, a unicast one left to lwIP.
    n = udp(p, "10.42.0.2", "255.255.255.255", 1, 2, "x", 64, 0x2000);
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "192.168.6.100", 1, 2, "x", 64, 0x0010);
    assert(sort(p, n, &u) == RELAY_PASS);

    // Anything not from the VM is dropped, whatever its protocol.
    n = udp(p, "10.42.0.3", "192.168.6.100", 1, 2, "x", 64, 0);
    assert(sort(p, n, &u) == RELAY_DROP);
    p[9] = 6;
    assert(sort(p, n, &u) == RELAY_DROP);
    n = udp(p, "10.42.0.2", "192.168.6.100", 1, 2, "x", 64, 0);
    p[9] = 6;
    assert(sort(p, n, &u) == RELAY_PASS);   // TCP from the VM: NAPT's
    // Malformed headers.
    n = udp(p, "10.42.0.2", "255.255.255.255", 1, 2, "abc", 64, 0);
    assert(sort(p, 19, &u) == RELAY_DROP);
    assert(sort(p, n - 1, &u) == RELAY_DROP);   // shorter than its total length
    p[0] = 0x65;
    assert(sort(p, n, &u) == RELAY_DROP);       // IPv6
    p[0] = 0x44;
    assert(sort(p, n, &u) == RELAY_DROP);       // header length 16
    p[0] = 0x45; p[25] = 7;
    assert(sort(p, n, &u) == RELAY_DROP);       // UDP length under 8
    p[25] = 12;
    assert(sort(p, n, &u) == RELAY_DROP);       // UDP length past the packet
    p[25] = 11;
    assert(sort(p, n, &u) == RELAY_BROADCAST);
}

static void check_senders(void) {
    uint32_t lan = ip("192.168.4.62"), mask = ip("255.255.252.0");
    uint32_t tun = ip("10.42.0.1"), tmask = ip("255.255.255.0");
    assert(relay_sender_ok(ip("192.168.6.100"), lan, mask, tun, tmask));
    assert(relay_sender_ok(ip("192.168.7.254"), lan, mask, tun, tmask));
    // The station itself, the tunnel and anything routed are forged or not an answer.
    assert(!relay_sender_ok(lan, lan, mask, tun, tmask));
    assert(!relay_sender_ok(ip("10.42.0.1"), lan, mask, tun, tmask));
    assert(!relay_sender_ok(ip("10.42.0.2"), lan, mask, tun, tmask));
    assert(!relay_sender_ok(ip("8.8.8.8"), lan, mask, tun, tmask));
    assert(!relay_sender_ok(ip("192.168.8.1"), lan, mask, tun, tmask));
    // Without a station address nobody is on the LAN.
    assert(!relay_sender_ok(ip("192.168.6.100"), 0, 0, tun, tmask));
    // A LAN that overlaps the tunnel: its hosts count, the tunnel's addresses don't.
    uint32_t wide = ip("10.1.2.3"), wmask = ip("255.0.0.0");
    assert(relay_sender_ok(ip("10.9.8.7"), wide, wmask, tun, tmask));
    assert(!relay_sender_ok(ip("10.42.0.1"), wide, wmask, tun, tmask));
    assert(!relay_sender_ok(ip("10.42.0.2"), wide, wmask, tun, tmask));
}

// An IPv4 header to dst with this protocol, header length and total length.
static size_t ipv4(uint8_t *p, uint8_t proto, int hlen, int total, const char *dst) {
    memset(p, 0, 80);
    p[0] = (uint8_t)(0x40 | hlen / 4);
    p[2] = (uint8_t)(total >> 8); p[3] = (uint8_t)total;
    p[9] = proto;
    uint32_t d = ip(dst);
    memcpy(p + 16, &d, 4);
    return (size_t)total;
}

static void check_napt_guard(void) {
    uint8_t p[80];
    uint32_t station = ip("192.168.4.62");
    // Whole headers pass: TCP, UDP, ICMP, and a TCP header after IP options.
    assert(!tunnel_napt_input_short(p, (uint16_t)ipv4(p, 6, 20, 40, "192.168.4.62"), station));
    assert(!tunnel_napt_input_short(p, (uint16_t)ipv4(p, 17, 20, 28, "192.168.4.62"), station));
    assert(!tunnel_napt_input_short(p, (uint16_t)ipv4(p, 1, 20, 28, "192.168.4.62"), station));
    assert(!tunnel_napt_input_short(p, (uint16_t)ipv4(p, 6, 60, 80, "192.168.4.62"), station));
    // Headers cut short: by the IP length, or by the first buffer.
    assert(tunnel_napt_input_short(p, (uint16_t)ipv4(p, 6, 60, 64, "192.168.4.62"), station));
    assert(tunnel_napt_input_short(p, (uint16_t)ipv4(p, 6, 20, 39, "192.168.4.62"), station));
    assert(tunnel_napt_input_short(p, (uint16_t)ipv4(p, 17, 20, 27, "192.168.4.62"), station));
    assert(tunnel_napt_input_short(p, (uint16_t)ipv4(p, 1, 20, 24, "192.168.4.62"), station));
    ipv4(p, 6, 20, 60, "192.168.4.62");
    assert(tunnel_napt_input_short(p, 30, station));
    // A fragment's few bytes count the same.
    ipv4(p, 17, 20, 24, "192.168.4.62");
    p[6] = 0x00; p[7] = 0x10;
    assert(tunnel_napt_input_short(p, 24, station));
    // Not for NAPT: another address, another protocol, or not IPv4 at all
    // (ip4_input drops those itself).
    assert(!tunnel_napt_input_short(p, (uint16_t)ipv4(p, 6, 20, 24, "192.168.4.63"), station));
    assert(!tunnel_napt_input_short(p, (uint16_t)ipv4(p, 50, 20, 24, "192.168.4.62"), station));
    ipv4(p, 6, 20, 24, "192.168.4.62");
    p[0] = 0x65;
    assert(!tunnel_napt_input_short(p, 24, station));
    assert(!tunnel_napt_input_short(p, 12, station));

    // From the LAN, nothing for the tunnel's subnet (the device's own tunnel
    // address included); from the tunnel, anything.
    struct netif tun = { NULL, { ip("10.42.0.1") }, { ip("255.255.255.0") }, 1 };
    struct netif lan = { &tun, { station }, { ip("255.255.252.0") }, 0 };
    netif_list = &lan;
    assert(tunnel_napt_input_inside(&lan, ip("10.42.0.2")));
    assert(tunnel_napt_input_inside(&lan, ip("10.42.0.1")));
    assert(!tunnel_napt_input_inside(&lan, station));
    assert(!tunnel_napt_input_inside(&lan, ip("10.42.1.2")));
    assert(!tunnel_napt_input_inside(&tun, ip("10.42.0.1")));
    assert(!tunnel_napt_input_inside(&tun, ip("192.168.4.20")));
    // A LAN that overlaps the tunnel: the station's own address still passes
    // (a short packet to it is still dropped), the tunnel's addresses don't.
    lan.ip.addr = ip("10.42.0.20"); lan.mask.addr = ip("255.255.0.0");
    assert(!tunnel_napt_input_inside(&lan, ip("10.42.0.20")));
    assert(tunnel_napt_input_inside(&lan, ip("10.42.0.1")));
    assert(tunnel_napt_input_inside(&lan, ip("10.42.0.2")));
    assert(tunnel_napt_input_short(p, (uint16_t)ipv4(p, 6, 20, 30, "10.42.0.20"), ip("10.42.0.20")));
}

int main(void) {
    check_classify();
    check_senders();
    check_napt_guard();
    puts("PASS relay: packet sorting, source check, malformed headers, LAN senders, short packets and the tunnel's subnet kept from the LAN");
    return 0;
}
