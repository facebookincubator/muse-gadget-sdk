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

// net.forward's forwards and the relay ports they share slots with: the real
// tunnel_relay.c, extracted by test_tunnel_relay.py, over a small fake of the
// lwIP calls it makes (bound ports, the NAPT port map, a clock and a timer).
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "tunnel_relay.h"

// ---- lwIP and ESP-IDF fakes ----
typedef int8_t err_t;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
#define ERR_OK 0
#define ERR_USE (-8)
#define IPADDR_TYPE_V4 0
#define IP_PROTO_TCP 6
#define IP_NAPT_PORTMAP 1
#define IP_NAPT_PORT_RANGE_START 49152
#define IP_NAPT_PORT_RANGE_END 61439

typedef struct ip4_addr { uint32_t addr; } ip4_addr_t;
typedef ip4_addr_t ip_addr_t;
#define IPADDR4_INIT(u32) { (u32) }
#define IP_IS_V4(a) ((void)(a), 1)
#define ip_2_ip4(a) (a)
#define ip4_addr_isany_val(a) ((a).addr == 0)
#define ip4_addr_eq(a, b) ((a)->addr == (b)->addr)
#define PP_HTONL(x) htonl(x)
static const ip_addr_t any_addr;
#define IP4_ADDR_ANY (&any_addr)

struct netif { bool up, link_up; ip4_addr_t ip, mask; };
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link_up)
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_netmask(n) (&(n)->mask)

typedef struct esp_netif_obj { int unused; } esp_netif_t;
static esp_netif_t sta_netif;
static struct netif lan_if, tunnel_if;
static esp_netif_t *wifi_mgr_get_netif(void) { return &sta_netif; }
static void *esp_netif_get_netif_impl(esp_netif_t *n) { (void)n; return &lan_if; }

static bool tunnel_connected;
static bool noise_tunnel_is_connected(void) { return tunnel_connected; }

#define LOCK_TCPIP_CORE() ((void)0)
#define UNLOCK_TCPIP_CORE() ((void)0)
static void fake_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGW(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log(tag, __VA_ARGS__)

static uint32_t now_ms = 1000;
typedef void (*sys_timeout_handler)(void *arg);
static sys_timeout_handler timer;
static uint32_t sys_now(void) { return now_ms; }
static void sys_timeout(uint32_t ms, sys_timeout_handler h, void *arg) {
    (void)ms; (void)arg;
    assert(!timer);
    timer = h;
}
static void sys_untimeout(sys_timeout_handler h, void *arg) {
    (void)arg;
    if (timer == h) timer = NULL;
}

static err_t igmp_joingroup_netif(struct netif *n, const ip4_addr_t *g) { (void)n; (void)g; return ERR_OK; }
static err_t igmp_leavegroup_netif(struct netif *n, const ip4_addr_t *g) { (void)n; (void)g; return ERR_OK; }

struct pbuf { u16_t len; uint8_t data[1500]; };
#define PBUF_TRANSPORT 0
#define PBUF_RAM 0
static int pbufs;
static struct pbuf *pbuf_alloc(int layer, u16_t len, int type) {
    (void)layer; (void)type;
    struct pbuf *p = calloc(1, sizeof(*p));
    p->len = len;
    pbufs++;
    return p;
}
static err_t pbuf_take(struct pbuf *p, const void *data, u16_t len) {
    memcpy(p->data, data, len);
    return ERR_OK;
}
static void pbuf_free(struct pbuf *p) { pbufs--; free(p); }

// Ports bound on the device. The device's own services hold `device_tcp`.
#define MAX_PCBS 64
static const u16_t device_tcp = 80;
struct udp_pcb { u16_t local_port; void *recv_arg; };
struct tcp_pcb { u16_t local_port; };
static struct udp_pcb *udp_pcbs[MAX_PCBS];
static struct tcp_pcb *tcp_pcbs[MAX_PCBS];
#define SOF_BROADCAST 0
#define ip_set_option(pcb, opt) ((void)(pcb), (void)(opt))
#define udp_set_multicast_ttl(pcb, ttl) ((void)(pcb), (void)(ttl))
typedef void (*udp_recv_fn)(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port);

static struct udp_pcb *udp_pcb_on(u16_t port) {
    for (int i = 0; i < MAX_PCBS; i++)
        if (udp_pcbs[i] && udp_pcbs[i]->local_port == port) return udp_pcbs[i];
    return NULL;
}
static struct udp_pcb *udp_new_ip_type(int type) {
    (void)type;
    for (int i = 0; i < MAX_PCBS; i++)
        if (!udp_pcbs[i]) return udp_pcbs[i] = calloc(1, sizeof(struct udp_pcb));
    return NULL;
}
static void udp_remove(struct udp_pcb *pcb) {
    for (int i = 0; i < MAX_PCBS; i++)
        if (udp_pcbs[i] == pcb) udp_pcbs[i] = NULL;
    free(pcb);
}
static err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *a, u16_t port) {
    (void)a;
    if (udp_pcb_on(port)) return ERR_USE;
    pcb->local_port = port;
    return ERR_OK;
}
static void udp_bind_netif(struct udp_pcb *pcb, struct netif *n) { (void)pcb; (void)n; }
static void udp_recv(struct udp_pcb *pcb, udp_recv_fn fn, void *arg) { (void)fn; pcb->recv_arg = arg; }

// The last datagram sent to the LAN.
static u16_t sent_from, sent_to;
static int sends;
static err_t udp_sendto_if(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst,
                           u16_t port, struct netif *n) {
    (void)p; (void)dst; (void)n;
    sent_from = pcb->local_port;
    sent_to = port;
    sends++;
    return ERR_OK;
}
static err_t udp_sendto_if_src(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst,
                               u16_t port, struct netif *n, const ip_addr_t *src) {
    (void)pcb; (void)p; (void)dst; (void)port; (void)n; (void)src;
    return ERR_OK;
}

static bool tcp_held(u16_t port) {
    if (port == device_tcp) return true;
    for (int i = 0; i < MAX_PCBS; i++)
        if (tcp_pcbs[i] && tcp_pcbs[i]->local_port == port) return true;
    return false;
}
static int tcp_live(void) {
    int n = 0;
    for (int i = 0; i < MAX_PCBS; i++) n += tcp_pcbs[i] != NULL;
    return n;
}
static struct tcp_pcb *tcp_new_ip_type(int type) {
    (void)type;
    for (int i = 0; i < MAX_PCBS; i++)
        if (!tcp_pcbs[i]) return tcp_pcbs[i] = calloc(1, sizeof(struct tcp_pcb));
    return NULL;
}
static err_t tcp_bind(struct tcp_pcb *pcb, const ip_addr_t *a, u16_t port) {
    (void)a;
    if (tcp_held(port)) return ERR_USE;
    pcb->local_port = port;
    return ERR_OK;
}
static err_t tcp_close(struct tcp_pcb *pcb) {
    for (int i = 0; i < MAX_PCBS; i++)
        if (tcp_pcbs[i] == pcb) tcp_pcbs[i] = NULL;
    free(pcb);
    return ERR_OK;
}
static void tcp_bind_netif(struct tcp_pcb *pcb, const struct netif *netif) { (void)pcb; (void)netif; }
// lwIP swaps the pcb for a listening one on the same port; here it stays.
static struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog) {
    (void)backlog;
    return pcb;
}

// lwIP's port map: add appends even for a port it maps; remove drops the first.
#define PORTMAP_MAX 8
static struct { u16_t mport, dport; u32_t daddr; } portmap[PORTMAP_MAX];
static int portmaps;
static u8_t ip_portmap_add(u8_t proto, u32_t maddr, u16_t mport, u32_t daddr, u16_t dport) {
    (void)maddr;
    assert(proto == IP_PROTO_TCP);
    if (portmaps == PORTMAP_MAX) return 0;
    portmap[portmaps].mport = mport;
    portmap[portmaps].dport = dport;
    portmap[portmaps].daddr = daddr;
    portmaps++;
    return 1;
}
static u8_t ip_portmap_remove(u8_t proto, u16_t mport) {
    assert(proto == IP_PROTO_TCP);
    for (int i = 0; i < portmaps; i++) {
        if (portmap[i].mport != mport) continue;
        memmove(&portmap[i], &portmap[i + 1], (size_t)(portmaps - i - 1) * sizeof(portmap[0]));
        portmaps--;
        return 1;
    }
    return 0;
}

#include "tunnel_relay_forward.inc"

// ---- the forwards ----
static uint32_t ip(const char *s) { return inet_addr(s); }

static void advance(uint32_t ms) {
    now_ms += ms;
    sys_timeout_handler h = timer;
    timer = NULL;
    if (h) h(NULL);
}

// One UDP datagram from the VM (10.42.0.2) to dst; true if lwIP takes it.
static bool from_vm(const char *dst, u16_t sport, u16_t dport) {
    uint8_t p[29] = { 0x45, 0, 0, 29 };
    p[8] = 64; p[9] = 17;
    uint32_t s = ip("10.42.0.2"), d = ip(dst);
    memcpy(p + 12, &s, 4); memcpy(p + 16, &d, 4);
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport;
    p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    p[25] = 9; p[28] = 'x';
    return tunnel_relay_from_vm(p, sizeof(p));
}

static int fwd(bool tcp, u16_t port, u16_t to_port, uint32_t ttl_s, const char **error) {
    *error = NULL;
    return tunnel_relay_forward(tcp, port, to_port, ttl_s, error);
}

static void refused(bool tcp, u16_t port, u16_t to_port, const char *why) {
    const char *error;
    assert(fwd(tcp, port, to_port, 60, &error) == -1);
    assert(error && !strcmp(error, why));
}

static void check_gates(void) {
    const char *error;
    tunnel_connected = false;
    refused(false, 5000, 6000, "the tunnel is down");
    refused(true, 8123, 8123, "the tunnel is down");
    assert(!udp_pcb_on(5000) && !tcp_live() && !portmaps);
    tunnel_connected = true;
    // lwIP's ephemeral ports and NAPT's mapped ports stay the device's.
    refused(false, 49152, 6000, "port must be from 1 to 49151");
    refused(true, 49152, 6000, "port must be from 1 to 49151");
    assert(fwd(false, 49151, 6000, 60, &error) == 60 && udp_pcb_on(49151));
    assert(fwd(false, 49151, 6000, 0, &error) == 0 && !udp_pcb_on(49151));
    assert(fwd(true, 49151, 6000, 60, &error) == 60 && tcp_held(49151));
    assert(fwd(true, 49151, 6000, 0, &error) == 0 && !tcp_held(49151) && !portmaps);
}

static void check_tcp(void) {
    const char *error;
    // A port the device already serves is refused, and the probe is let go.
    refused(true, device_tcp, 8080, "port is in use on the device");
    assert(!tcp_live() && !portmaps);

    // While a forward lasts, its port stays bound so nothing else takes it.
    assert(fwd(true, 8123, 8123, 60, &error) == 60);
    assert(tcp_held(8123) && tcp_live() == 1);
    assert(portmaps == 1 && portmap[0].mport == 8123 && portmap[0].dport == 8123
           && portmap[0].daddr == ip("10.42.0.2"));
    struct tcp_pcb *other = tcp_new_ip_type(IPADDR_TYPE_V4);
    assert(tcp_bind(other, IP4_ADDR_ANY, 8123) == ERR_USE);
    tcp_close(other);

    // One to_port serves one device port; the existing map stays.
    refused(true, 8124, 8123, "to_port already forwards another device port");
    assert(!tcp_held(8124) && tcp_live() == 1);
    assert(portmaps == 1 && portmap[0].mport == 8123 && portmap[0].dport == 8123);
    // Renewing it keeps one map entry and the same hold.
    assert(fwd(true, 8123, 8123, 120, &error) == 120);
    assert(portmaps == 1 && tcp_live() == 1 && tcp_held(8123));

    // ttl_s 0 ends it at once.
    assert(fwd(true, 8123, 8123, 0, &error) == 0);
    assert(!tcp_held(8123) && !tcp_live() && !portmaps);
    // So does its deadline, on the next tick past it.
    assert(fwd(true, 8123, 8123, 5, &error) == 5);
    advance(4000);
    assert(tcp_held(8123) && portmaps == 1);
    advance(2000);
    assert(!tcp_held(8123) && !tcp_live() && !portmaps);
    // And a reset (the tunnel went down or reopened).
    assert(fwd(true, 8123, 8123, 60, &error) == 60);
    tunnel_relay_reset();
    assert(!tcp_held(8123) && !tcp_live() && !portmaps && !timer);
}

static void check_udp(void) {
    const char *error;
    // The VM's replies go out through the forward's port...
    assert(fwd(false, 5000, 6000, 60, &error) == 60 && udp_pcb_on(5000));
    sends = 0;
    assert(!from_vm("192.168.6.100", 6000, 7000));
    assert(sends == 1 && sent_from == 5000 && sent_to == 7000);
    // ...without keeping it open: ttl_s 0 closes it at once,
    assert(fwd(false, 5000, 6000, 0, &error) == 0 && !udp_pcb_on(5000));
    // a reply to a port without a forward is lwIP's again,
    assert(from_vm("192.168.6.100", 6000, 7000));
    // and the deadline closes it on the next tick past it.
    assert(fwd(false, 5000, 6000, 5, &error) == 5);
    assert(!from_vm("192.168.6.100", 6000, 7000));
    // A reply doesn't hold the port against another to_port either.
    assert(fwd(false, 5000, 6001, 5, &error) == 5);
    assert(fwd(false, 5000, 6000, 5, &error) == 5);
    advance(6000);
    assert(!udp_pcb_on(5000));
    // DHCP's ports stay the home network's, either side of a UDP forward.
    refused(false, 67, 6000, "UDP forwards can't use DHCP's ports (67, 68)");
    refused(false, 5000, 68, "UDP forwards can't use DHCP's ports (67, 68)");
    assert(!udp_pcb_on(67) && !udp_pcb_on(5000));
    // A to_port relays through one device port.
    assert(fwd(false, 5000, 6000, 60, &error) == 60);
    refused(false, 5001, 6000, "to_port already relays through another device port");
    assert(!udp_pcb_on(5001));
    tunnel_relay_reset();
    assert(!udp_pcb_on(5000) && !timer);
}

static void check_slots(void) {
    const char *error;
    // A forward, then the VM's broadcasts take the other 15 slots.
    assert(fwd(false, 5000, 6000, 600, &error) == 600);
    for (u16_t i = 0; i < 15; i++) {
        advance(100);
        assert(!from_vm("255.255.255.255", (u16_t)(30000 + i), 9999));
        assert(udp_pcb_on((u16_t)(30000 + i)));
    }
    // A new relay port replaces the least recently used relay port, never the
    // forward, which (relay_until 0) would otherwise look oldest.
    sends = 0;
    assert(!from_vm("255.255.255.255", 31000, 9999));
    assert(sends == 1 && sent_from == 31000);
    assert(udp_pcb_on(5000) && !udp_pcb_on(30000) && udp_pcb_on(30001));
    // So does a new forward, so the VM's discovery can't lock forwards out...
    assert(fwd(false, 5001, 6001, 600, &error) == 600);
    assert(udp_pcb_on(5000) && udp_pcb_on(5001) && !udp_pcb_on(30001) && udp_pcb_on(30002));
    // ...once its own port is bound: a port in use replaces nothing.
    struct udp_pcb *service = udp_new_ip_type(0);
    assert(service && udp_bind(service, NULL, 5353) == ERR_OK);
    refused(false, 5353, 6353, "port is in use on the device");
    assert(udp_pcb_on(30002));
    udp_remove(service);
    // With every slot a forward, a broadcast gets no relay port and another
    // forward is refused.
    for (u16_t i = 2; i < 16; i++)
        assert(fwd(false, (u16_t)(5000 + i), (u16_t)(6000 + i), 600, &error) == 600);
    assert(!udp_pcb_on(31000) && udp_pcb_on(5000) && udp_pcb_on(5015));
    refused(false, 5016, 6016, "too many UDP forwards");
    sends = 0;
    from_vm("255.255.255.255", 32000, 9999);
    assert(!sends && !udp_pcb_on(32000));
    tunnel_relay_reset();
    assert(!udp_pcb_on(5000) && !udp_pcb_on(5015));
}

int main(void) {
    struct ip4_addr vm = { ip("10.42.0.2") };
    tunnel_if.up = tunnel_if.link_up = true;
    lan_if.up = lan_if.link_up = true;
    lan_if.ip.addr = ip("192.168.4.62");
    lan_if.mask.addr = ip("255.255.252.0");
    tunnel_relay_init(&tunnel_if, &vm);
#if !HOMEHUB_NAPT_INPUT_GUARD
    // Without the check that keeps short packets from NAPT: UDP forwards, but
    // no TCP forward.
    const char *error = NULL;
    tunnel_connected = true;
    refused(true, 8123, 8123, "TCP forwards need lwIP's NAPT port map and its input check");
    assert(!tcp_live() && !portmaps);
    assert(fwd(false, 5000, 5000, 60, &error) == 60 && udp_pcb_on(5000));
    assert(fwd(false, 5000, 5000, 0, &error) == 0 && !udp_pcb_on(5000));
    puts("PASS relay forwards: no TCP forward without the NAPT input check");
    return 0;
#endif
    check_gates();
    check_tcp();
    check_udp();
    check_slots();
    for (int i = 0; i < MAX_PCBS; i++) assert(!udp_pcbs[i] && !tcp_pcbs[i]);
    assert(!pbufs);
    puts("PASS relay forwards: port limit, held TCP ports, one to_port per port, "
         "replies don't extend a forward, expiry, reset, a forward replaces the oldest relay port "
         "and is never replaced");
    return 0;
}
