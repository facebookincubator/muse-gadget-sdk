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

struct netif;
struct ip4_addr;

// UDP the tunnel carries past lwIP's NAPT: broadcast and multicast relay to the
// LAN, VM-requested inbound forwards (net.forward), and the LAN address they
// are reached at. docs/home-link-network.md has the behavior the VM sees.

// The tunnel netif and the VM's address on it. Once, before packets flow.
void tunnel_relay_init(struct netif *tunnel, const struct ip4_addr *vm);

// One IPv4 packet from the VM. True: lwIP takes it (NAPT, the device itself).
// False: it went to the LAN through a relay port, or was dropped (a source
// other than the VM, or malformed).
bool tunnel_relay_from_vm(const uint8_t *pkt, size_t len);

// Closes every relay port and forward: the tunnel went down, or a new tunnel
// stream is opening.
void tunnel_relay_reset(void);

// net.forward: maps the device's LAN `port` to the VM's `to_port` for ttl_s
// seconds (0 closes it). Returns the seconds left, or -1 with *error saying why.
int tunnel_relay_forward(bool tcp, uint16_t port, uint16_t to_port,
                         uint32_t ttl_s, const char **error);

// The station's IPv4 address as text and its prefix length. False without one.
bool tunnel_relay_lan_address(char *ip, size_t ip_len, int *prefix);

#ifdef __cplusplus
}
#endif
