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

#include "tunnel_relay.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/igmp.h"
#include "lwip/ip.h"
#include "lwip/lwip_napt.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

#include "noise_tunnel.h"
#include "wifi_mgr.h"

static const char *TAG = "link.relay";

#define RELAY_PORTS     16      // relay ports and UDP forwards together
#define RELAY_GROUPS    4       // multicast groups joined per relay port
#define TCP_FORWARDS    8
#define RELAY_IDLE_MS   120000  // a relay port lives this long after the VM's last send
#define RELAY_TICK_MS   1000
// lwIP's own ephemeral ports and NAPT's mapped ports start here: a forward
// above it could take over one of the device's connections.
#define FORWARD_PORT_LIMIT 49152

typedef enum {
    RELAY_PASS,       // lwIP's: NAPT, or the device itself
    RELAY_DROP,
    RELAY_BROADCAST,  // UDP to 255.255.255.255 or the LAN's broadcast address
    RELAY_MULTICAST,  // UDP to 224.0.0.0/4
    RELAY_UNICAST,    // UDP to a LAN host: relayed when its source port has a relay port
} relay_kind_t;

typedef struct {
    uint32_t dst;           // network order
    uint16_t sport, dport;  // host order
    const uint8_t *data;
    uint16_t data_len;
} relay_udp_t;

// Sorts one IPv4 packet from the VM. Addresses in network order; tun_mask is
// the tunnel's netmask (the VM is on its subnet), and lan_mask is 0 while the
// station has no address.
static relay_kind_t relay_classify(const uint8_t *pkt, size_t len, uint32_t vm,
                                   uint32_t tun_mask, uint32_t lan_ip, uint32_t lan_mask,
                                   relay_udp_t *u) {
    if (len < 20 || (pkt[0] >> 4) != 4) return RELAY_DROP;
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    size_t total = ((size_t)pkt[2] << 8) | pkt[3];
    if (ihl < 20 || total < ihl || total > len) return RELAY_DROP;
    uint32_t src, dst;
    memcpy(&src, pkt + 12, 4);
    memcpy(&dst, pkt + 16, 4);
    if (src != vm) return RELAY_DROP;
    if (pkt[9] != 17) return RELAY_PASS;
    // The tunnel's own subnet is lwIP's, even on a LAN that overlaps it.
    if (tun_mask && ((dst ^ vm) & tun_mask) == 0) return RELAY_PASS;
    bool multicast = (((const uint8_t *)&dst)[0] & 0xf0) == 0xe0;
    bool on_lan = lan_mask && (dst & lan_mask) == (lan_ip & lan_mask);
    bool broadcast = dst == 0xffffffffu
                     || (on_lan && ~lan_mask && (dst & ~lan_mask) == ~lan_mask);
    if (!multicast && !broadcast && (!on_lan || dst == lan_ip)) return RELAY_PASS;
    bool fragment = ((((unsigned)pkt[6] << 8) | pkt[7]) & 0x3fff) != 0;  // MF or an offset
    if (fragment || total < ihl + 8) return multicast || broadcast ? RELAY_DROP : RELAY_PASS;
    const uint8_t *h = pkt + ihl;
    size_t ulen = ((size_t)h[4] << 8) | h[5];
    if (ulen < 8 || ihl + ulen > total) return RELAY_DROP;
    u->dst = dst;
    u->sport = (uint16_t)((h[0] << 8) | h[1]);
    u->dport = (uint16_t)((h[2] << 8) | h[3]);
    u->data = h + 8;
    u->data_len = (uint16_t)(ulen - 8);
    // DHCP (67, 68) stays the home network's own: the VM can't answer or ask
    // for leases on it. Unicast is NAPT's, never a relay port's.
    if (u->sport == 67 || u->sport == 68 || u->dport == 67 || u->dport == 68)
        return multicast || broadcast ? RELAY_DROP : RELAY_PASS;
    if (multicast) return pkt[8] ? RELAY_MULTICAST : RELAY_DROP;  // TTL 0 goes nowhere
    return broadcast ? RELAY_BROADCAST : RELAY_UNICAST;
}

// A relay port: one UDP pcb on the LAN for one VM port. The VM's broadcast and
// multicast sends make one (relay_until); net.forward makes one on a chosen
// port (forward_until). It lives while either deadline is ahead.
typedef struct {
    struct udp_pcb *pcb;    // NULL: free
    uint16_t vm_port;
    uint32_t relay_until;   // sys_now() deadlines, 0 = none
    uint32_t forward_until;
    uint8_t ngroups;
    ip4_addr_t groups[RELAY_GROUPS];
} relay_port_t;

typedef struct {
    uint16_t port, to_port;
    uint32_t until;         // 0: free
    struct tcp_pcb *hold;   // listening on `port`, so no device service takes it meanwhile
} tcp_forward_t;

static struct netif *s_tunnel;
static ip4_addr_t s_vm;
static relay_port_t s_ports[RELAY_PORTS];
static tcp_forward_t s_tcp[TCP_FORWARDS];
static struct udp_pcb *s_deliver;   // never bound: sends LAN replies with their source port
static bool s_armed;
static uint32_t s_dropped;

static bool ahead(uint32_t until, uint32_t now) {
    return until && (int32_t)(until - now) > 0;
}

static uint32_t deadline(uint32_t ms) {
    uint32_t t = sys_now() + ms;
    return t ? t : 1;
}

static struct netif *lan_netif(void) {
    esp_netif_t *sta = wifi_mgr_get_netif();
    return sta ? esp_netif_get_netif_impl(sta) : NULL;
}

static bool lan_has_address(struct netif *lan) {
    return lan && netif_is_up(lan) && !ip4_addr_isany_val(*netif_ip4_addr(lan));
}

static void relay_tick(void *arg);

static void relay_arm(void) {
    if (s_armed) return;
    s_armed = true;
    sys_timeout(RELAY_TICK_MS, relay_tick, NULL);
}

static void relay_close(relay_port_t *r) {
    struct netif *lan = lan_netif();
    for (int i = 0; lan && i < r->ngroups; i++) igmp_leavegroup_netif(lan, &r->groups[i]);
    udp_remove(r->pcb);
    memset(r, 0, sizeof(*r));
}

static void tcp_forward_close(tcp_forward_t *f) {
#if IP_NAPT_PORTMAP
    ip_portmap_remove(IP_PROTO_TCP, f->port);
#endif
    if (f->hold) tcp_close(f->hold);
    memset(f, 0, sizeof(*f));
}

// Whether a datagram from `sender` may reach the VM: only hosts on the
// station's subnet, not the station itself, answer what the relay sends (it
// sends only there, and multicast with TTL 1), so any other source is forged
// or routed. The tunnel's own subnet never counts, even inside a LAN that
// overlaps it. Addresses in network order.
static bool relay_sender_ok(uint32_t sender, uint32_t lan_ip, uint32_t lan_mask,
                            uint32_t tun_ip, uint32_t tun_mask) {
    return lan_mask && sender != lan_ip && (sender & lan_mask) == (lan_ip & lan_mask)
           && (sender & tun_mask) != (tun_ip & tun_mask);
}

// A LAN datagram on a relay port goes to the VM as sent from its LAN sender.
static void relay_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    relay_port_t *r = arg;
    struct netif *lan = lan_netif();
    if (s_tunnel && netif_is_link_up(s_tunnel) && port && IP_IS_V4(addr) && lan_has_address(lan)
        && relay_sender_ok(ip_2_ip4(addr)->addr, netif_ip4_addr(lan)->addr,
                           netif_ip4_netmask(lan)->addr, netif_ip4_addr(s_tunnel)->addr,
                           netif_ip4_netmask(s_tunnel)->addr)) {
        if (!s_deliver) s_deliver = udp_new_ip_type(IPADDR_TYPE_V4);
        if (s_deliver) {
            ip_addr_t vm = IPADDR4_INIT(s_vm.addr);
            s_deliver->local_port = port;
            udp_sendto_if_src(s_deliver, p, &vm, r->vm_port, s_tunnel, addr);
        }
    }
    pbuf_free(p);
}

static relay_port_t *find_vm_port(uint16_t vm_port) {
    for (int i = 0; i < RELAY_PORTS; i++)
        if (s_ports[i].pcb && s_ports[i].vm_port == vm_port) return &s_ports[i];
    return NULL;
}

// Binds pcb to a port of its own above NAPT's mapped ports: below them, or
// inside them, a NAPT mapping could already own a port no pcb holds, and LAN
// replies to it would go to that mapping's VM port instead.
static err_t relay_bind_free_port(struct udp_pcb *pcb) {
    static uint16_t next;
    const uint32_t first = IP_NAPT_PORT_RANGE_END + 1, count = 65536 - first;
    err_t err = ERR_USE;
    for (uint32_t i = 0; i < count && err != ERR_OK; i++) {
        next = (uint16_t)(first + (next - first + 1) % count);
        err = udp_bind(pcb, IP4_ADDR_ANY, next);
    }
    return err;
}

static relay_port_t *find_lan_port(uint16_t port) {
    for (int i = 0; i < RELAY_PORTS; i++)
        if (s_ports[i].pcb && s_ports[i].pcb->local_port == port) return &s_ports[i];
    return NULL;
}

// A pcb on the LAN for vm_port. A relay port is bound to vm_port when it's
// free and outside NAPT's mapped ports, else to one relay_bind_free_port()
// picks; a forward needs its exact `port`. Without a free slot, either
// replaces the relay port the VM used least recently, once its own pcb is
// bound; a forward is never replaced.
static relay_port_t *relay_open(uint16_t vm_port, uint16_t port, bool forward,
                                struct netif *lan, const char **error) {
    uint32_t now = sys_now();
    relay_port_t *r = NULL, *oldest = NULL;
    for (int i = 0; i < RELAY_PORTS && !r; i++) {
        relay_port_t *c = &s_ports[i];
        if (!c->pcb) r = c;
        else if (!ahead(c->forward_until, now)
                 && (!oldest || (int32_t)(c->relay_until - oldest->relay_until) < 0)) oldest = c;
    }
    if (!r && !oldest) {
        *error = "too many UDP forwards";
        return NULL;
    }
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        *error = "out of memory";
        return NULL;
    }
    ip_set_option(pcb, SOF_BROADCAST);
    udp_bind_netif(pcb, lan);
    err_t err;
    if (forward) {
        err = udp_bind(pcb, IP4_ADDR_ANY, port);
    } else {
        // Port 0 would let lwIP pick, possibly inside NAPT's mapped ports.
        bool napt_port = port == 0
                         || (port >= IP_NAPT_PORT_RANGE_START && port <= IP_NAPT_PORT_RANGE_END);
        err = napt_port ? ERR_USE : udp_bind(pcb, IP4_ADDR_ANY, port);
        if (err != ERR_OK) err = relay_bind_free_port(pcb);
    }
    if (err != ERR_OK) {
        udp_remove(pcb);
        *error = forward ? "port is in use on the device" : "no free port on the device";
        return NULL;
    }
    if (!r) {
        relay_close(oldest);
        r = oldest;
    }
    udp_set_multicast_ttl(pcb, 1);
    r->pcb = pcb;
    r->vm_port = vm_port;
    if (!forward) {
        // Live from here: a send that fails still leaves it to expire.
        r->relay_until = deadline(RELAY_IDLE_MS);
        relay_arm();
    }
    udp_recv(pcb, relay_recv, r);
    return r;
}

static void relay_send(relay_port_t *r, const relay_udp_t *u, relay_kind_t kind,
                       struct netif *lan) {
    if (kind == RELAY_MULTICAST) {
        ip4_addr_t group = { .addr = u->dst };
        // The station is always in the all-systems group (224.0.0.1).
        bool joined = ip4_addr_eq(&group, &(ip4_addr_t){ PP_HTONL(0xe0000001UL) });
        for (int i = 0; i < r->ngroups && !joined; i++) joined = ip4_addr_eq(&r->groups[i], &group);
        if (!joined && r->ngroups < RELAY_GROUPS) {
            if (igmp_joingroup_netif(lan, &group) == ERR_OK) {
                r->groups[r->ngroups++] = group;
            } else {
                // Out of memory: the send goes out, but replies to the group
                // aren't heard.
                static uint32_t join_failures;
                if ((join_failures++ & 63) == 0)
                    ESP_LOGW(TAG, "can't join a multicast group for VM port %u", r->vm_port);
            }
        }
    }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, u->data_len, PBUF_RAM);
    if (!p) return;
    pbuf_take(p, u->data, u->data_len);
    ip_addr_t dst = IPADDR4_INIT(u->dst);
    udp_sendto_if(r->pcb, p, &dst, u->dport, lan);
    pbuf_free(p);
    // Unicast keeps a relay port alive, but doesn't make one of a forward: a
    // forward ends when its own deadline does, however much the VM replies.
    if (kind != RELAY_UNICAST || r->relay_until) {
        r->relay_until = deadline(RELAY_IDLE_MS);
        relay_arm();
    }
}

static void relay_tick(void *arg) {
    (void)arg;
    s_armed = false;
    uint32_t now = sys_now();
    bool live = false;
    for (int i = 0; i < RELAY_PORTS; i++) {
        relay_port_t *r = &s_ports[i];
        if (!r->pcb) continue;
        if (!ahead(r->forward_until, now)) r->forward_until = 0;
        if (!ahead(r->relay_until, now)) r->relay_until = 0;
        if (r->forward_until || r->relay_until) live = true;
        else relay_close(r);
    }
    for (int i = 0; i < TCP_FORWARDS; i++) {
        if (!s_tcp[i].until) continue;
        if (ahead(s_tcp[i].until, now)) live = true;
        else tcp_forward_close(&s_tcp[i]);
    }
    if (live) relay_arm();
}

void tunnel_relay_init(struct netif *tunnel, const struct ip4_addr *vm) {
    s_tunnel = tunnel;
    s_vm = *vm;
}

bool tunnel_relay_from_vm(const uint8_t *pkt, size_t len) {
    relay_udp_t u;
    relay_kind_t kind;
    bool pass = false;
    if (len >= 20 && pkt[9] != 17) {
        // Only the source check applies: no lock for TCP and the rest.
        kind = relay_classify(pkt, len, s_vm.addr, 0, 0, 0, &u);
        pass = kind == RELAY_PASS;
    } else {
        LOCK_TCPIP_CORE();
        struct netif *lan = lan_netif();
        bool addressed = lan_has_address(lan);
        kind = relay_classify(pkt, len, s_vm.addr,
                              s_tunnel ? netif_ip4_netmask(s_tunnel)->addr : 0,
                              addressed ? netif_ip4_addr(lan)->addr : 0,
                              addressed ? netif_ip4_netmask(lan)->addr : 0, &u);
        relay_port_t *r = NULL;
        const char *error = NULL;
        if (kind == RELAY_UNICAST) {
            r = find_vm_port(u.sport);
            pass = !r;
        } else if ((kind == RELAY_BROADCAST || kind == RELAY_MULTICAST) && addressed) {
            r = find_vm_port(u.sport);
            if (!r) r = relay_open(u.sport, u.sport, false, lan, &error);
        } else {
            pass = kind == RELAY_PASS;
        }
        if (r) relay_send(r, &u, kind, lan);
        UNLOCK_TCPIP_CORE();
        static uint32_t refused;
        if (error && (refused++ & 63) == 0)
            ESP_LOGW(TAG, "no relay port for VM port %u: %s", u.sport, error);
    }
    if (kind == RELAY_DROP && (s_dropped++ & 63) == 0)
        ESP_LOGW(TAG, "dropped %u tunnel packets not from the VM, malformed, or DHCP for the LAN",
                 (unsigned)s_dropped);
    return pass;
}

void tunnel_relay_reset(void) {
    LOCK_TCPIP_CORE();
    sys_untimeout(relay_tick, NULL);
    s_armed = false;
    for (int i = 0; i < RELAY_PORTS; i++)
        if (s_ports[i].pcb) relay_close(&s_ports[i]);
    for (int i = 0; i < TCP_FORWARDS; i++)
        if (s_tcp[i].until) tcp_forward_close(&s_tcp[i]);
    if (s_deliver) {
        udp_remove(s_deliver);
        s_deliver = NULL;
    }
    UNLOCK_TCPIP_CORE();
}

static int forward_udp(uint16_t port, uint16_t to_port, uint32_t ttl_s,
                       struct netif *lan, const char **error) {
    relay_port_t *r = find_lan_port(port);
    if (!ttl_s) {
        // Closed at once, relaying or not: the VM's next broadcast from that
        // port opens a relay port again if it needs one.
        if (r) relay_close(r);
        return 0;
    }
    relay_port_t *other = find_vm_port(to_port);
    if (other && other != r) {
        *error = "to_port already relays through another device port";
        return -1;
    }
    if (r && r->vm_port != to_port) {
        if (ahead(r->relay_until, sys_now())) {
            *error = r->forward_until ? "port relays the VM's broadcasts from its current to_port"
                                      : "port is in use on the device";
            return -1;
        }
        r->vm_port = to_port;
    }
    if (!r && !(r = relay_open(to_port, port, true, lan, error))) return -1;
    r->forward_until = deadline(ttl_s * 1000);
    relay_arm();
    return (int)ttl_s;
}

static int forward_tcp(uint16_t port, uint16_t to_port, uint32_t ttl_s,
                       struct netif *lan, const char **error) {
    // A port map lets any LAN host reach NAPT's rewrite without knowing a
    // live mapping, so it waits for the input check that keeps short packets
    // from it (main/CMakeLists.txt).
#if IP_NAPT_PORTMAP && HOMEHUB_NAPT_INPUT_GUARD
    tcp_forward_t *f = NULL, *free_slot = NULL;
    for (int i = 0; i < TCP_FORWARDS; i++) {
        if (s_tcp[i].until && s_tcp[i].port == port) f = &s_tcp[i];
        else if (!s_tcp[i].until && !free_slot) free_slot = &s_tcp[i];
    }
    if (!ttl_s) {
        if (f) tcp_forward_close(f);
        return 0;
    }
    // lwIP's port map rewrites the VM's replies from to_port with the first
    // entry it finds, so one to_port serves one device port.
    for (int i = 0; i < TCP_FORWARDS; i++) {
        if (s_tcp[i].until && &s_tcp[i] != f && s_tcp[i].to_port == to_port) {
            *error = "to_port already forwards another device port";
            return -1;
        }
    }
    if (!f) {
        if (!free_slot) {
            *error = "too many TCP forwards";
            return -1;
        }
        // A pcb listening on the port both proves the device doesn't hold it
        // and keeps it that way while the forward lasts. Listening frees the
        // pcb for a small listening one, so a forward takes none of lwIP's
        // active connections, and the port map hands connections to the VM
        // before they could reach it.
        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) {
            *error = "out of memory";
            return -1;
        }
        if (tcp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK) {
            tcp_close(pcb);
            *error = "port is in use on the device";
            return -1;
        }
        // Only the tunnel side may reach the listener itself.
        tcp_bind_netif(pcb, s_tunnel);
        struct tcp_pcb *hold = tcp_listen_with_backlog(pcb, 1);
        if (!hold) {
            tcp_close(pcb);
            *error = "out of memory";
            return -1;
        }
        f = free_slot;
        f->hold = hold;
        f->port = port;
    }
    // ip_portmap_add() adds a second entry for a port it already maps.
    ip_portmap_remove(IP_PROTO_TCP, port);
    if (!ip_portmap_add(IP_PROTO_TCP, netif_ip4_addr(lan)->addr, port, s_vm.addr, to_port)) {
        tcp_forward_close(f);
        *error = "lwIP's port map is full";
        return -1;
    }
    f->to_port = to_port;
    f->until = deadline(ttl_s * 1000);
    relay_arm();
    return (int)ttl_s;
#else
    (void)port; (void)to_port; (void)ttl_s; (void)lan;
    *error = "TCP forwards need lwIP's NAPT port map and its input check";
    return -1;
#endif
}

int tunnel_relay_forward(bool tcp, uint16_t port, uint16_t to_port,
                         uint32_t ttl_s, const char **error) {
    int left = -1;
    LOCK_TCPIP_CORE();
    struct netif *lan = lan_netif();
    if (!s_tunnel || !netif_is_link_up(s_tunnel) || !noise_tunnel_is_connected()) {
        *error = "the tunnel is down";
    } else if (!lan_has_address(lan)) {
        *error = "the device has no LAN address";
    } else if (port == 0 || port >= FORWARD_PORT_LIMIT) {
        *error = "port must be from 1 to 49151";
    } else if (!tcp && (port == 67 || port == 68 || to_port == 67 || to_port == 68)) {
        // DHCP stays the home network's own, as with relay ports.
        *error = "UDP forwards can't use DHCP's ports (67, 68)";
    } else {
        left = tcp ? forward_tcp(port, to_port, ttl_s, lan, error)
                   : forward_udp(port, to_port, ttl_s, lan, error);
    }
    UNLOCK_TCPIP_CORE();
    if (left >= 0)
        ESP_LOGI(TAG, "%s forward %u -> VM %u: %s", tcp ? "TCP" : "UDP", port, to_port,
                 left ? "open" : "closed");
    return left;
}

bool tunnel_relay_lan_address(char *ip, size_t ip_len, int *prefix) {
    esp_netif_t *sta = wifi_mgr_get_netif();
    esp_netif_ip_info_t info;
    if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK || !info.ip.addr) return false;
    snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));
    *prefix = __builtin_popcount(info.netmask.addr);
    return true;
}
