<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Home Link network: UDP and the LAN address

What the VM sees from a Home Link with `CONFIG_HOMEHUB_TUNNEL`. The tunnel is
the raw L3 stream on the Noise session: the VM is 10.42.0.2, the device
10.42.0.1/24, IPv4 only. `main/tunnel_netif.c` carries the packets and
`main/tunnel_relay.c` does everything below that lwIP's NAPT doesn't.

## Tunnel input

- The device takes only packets whose source is 10.42.0.2 and drops the rest.
- TCP, ICMP and unicast UDP go through lwIP's NAPT to the LAN and the
  internet, as before, except UDP from a relay port's source port to a LAN
  host (below).

## Unicast UDP

NAPT keeps an idle UDP mapping for `CONFIG_HOMEHUB_TUNNEL_UDP_IDLE_S` seconds
(default 120, 120 to 3600; lwIP's own default is 2). NAPT has 512 entries for
TCP, UDP and ICMP together; when it's full, a new mapping replaces the least
recently used one, which can be a quiet TCP connection. A reply gets back only
from the address and port the VM last sent to from that source port, so use
one source port per peer. Mappings aren't tied to the
session: one that's still alive when the session ends (TCP's last 30 minutes)
carries on for the next session's VM until it expires.

## Broadcast and multicast

The VM sends UDP to 255.255.255.255, to the LAN's broadcast address (such as
192.168.7.255 on a /22), or to a group in 224.0.0.0/4. lwIP doesn't forward
these, so the device sends the payload itself:

- from a relay port: the VM's source port when that port is free on the
  device and outside NAPT's mapped ports (49152 to 61439), otherwise one of
  the device's own from 61440 up;
- to the same address and port (255.255.255.255 is the LAN's limited
  broadcast), with multicast TTL 1. Multicast with TTL 0 is dropped.

DHCP stays the home network's own: a broadcast or multicast to or from port 67
or 68 is dropped, and unicast on those ports goes through NAPT. Packets for
the tunnel's subnet (10.42.0.0/24) stay with lwIP, even on a LAN that overlaps
it.

A relay port lives for 120 s after the VM's last send through it, and every
relay port closes when the tunnel goes down or a new tunnel stream opens.
While it lives:

- the device stays joined to every multicast group the VM sent to from that
  port (up to 4 per relay port; a join that fails for lack of memory is
  logged, and replies to that group aren't heard);
- every datagram that reaches the relay port from a host on the station's
  subnet (unicast replies, or traffic to its joined groups or to broadcast)
  goes to the VM as `src = <LAN sender ip:port>`,
  `dst = 10.42.0.2:<VM source port>`. Anything from another source (the
  station itself, the tunnel's addresses, a routed address) is dropped;
- unicast datagrams the VM sends from that source port to a host on the LAN
  (the station's subnet) go out through the relay port too, so the LAN sees
  the relay port, not a NAPT port. Unicast elsewhere still goes through NAPT.

There are 16 relay ports. When they are all taken, a new one replaces the one
the VM used least recently. Fragmented broadcasts and multicasts are dropped,
and a fragmented unicast datagram goes through NAPT even from a relay port's
source port. A LAN datagram larger than the tunnel's 1280-byte MTU reaches the
VM in IP fragments.

## The LAN address

`link.register`'s `metadata` carries `"lan_ipv4"` (dotted quad) and
`"lan_prefix"` (prefix length), both strings because the VM keeps only string
metadata; neither is sent without a station address. A new station address
aborts the session's TCP connection, so the next session registers the new
one.

## Not here

IPv6 and Matter: lwIP has no NAT66.
