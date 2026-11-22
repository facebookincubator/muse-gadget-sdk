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

ESP-IDF's NAPT rewrites a packet to the station before lwIP checks that its
TCP, UDP or ICMP header is there, so the device drops a packet to the station
whose header isn't whole before NAPT sees it, a fragment included (a trailing
fragment shorter than the header too). It also drops a packet from the LAN
addressed to the tunnel's subnet (10.42.0.0/24; the station's own address
passes even on a LAN that overlaps it), which lwIP would otherwise route to
the VM with whatever source it claims; replies to the VM's own flows arrive
addressed to the station and still pass. Both checks are
compiled into lwIP on ESP-IDF v6.0.x; on another version the build warns, and
NAPT keeps idle UDP for lwIP's 2 s, since a live mapping is what lets a short
packet reach the rewrite.

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

## `device.discover`'s vendor source

`"vendor"` joins the other sources (in `sources`, and in the default of all
of them). For 6 s after the other sources it listens, sending each vendor's
read-only discovery packet at the start and again 1.5 s in:

| `protocol` | Sends | Fields |
|---|---|---|
| `kasa` | XOR `get_sysinfo`, broadcast to 9999 | `model`, `name`, `mac` |
| `tplink` | TP-Link discovery, broadcast to 20002 | `model`, `type`, `mac`, `encryption` |
| `govee` | `scan` to 239.255.255.250:4001, answered on 4002 | `model` (SKU), `id` |
| `miio` | hello, broadcast to 54321 | `id` (device id) |
| `wiz` | `getPilot`, broadcast to 38899 | `mac` |
| `lifx` | GetService, broadcast to 56700 | `mac` |
| `yeelight` | M-SEARCH to 239.255.255.250:1982 | `model`, `id`, `name` |
| `cs2` | CS2/PPPP LAN search, broadcast to 32108 | `id_prefix` |
| `tuya` | nothing: hears broadcasts on 6666, 6667 and 7000 | on 6666: `id`, `model` (product key), `version` |
| `bambu` | nothing: hears broadcasts on 2021 | `model`, `name` |

Each observation is `{"source": "vendor", "protocol", "ip", "port", ...fields}`,
once per device and protocol; `port` is the device's port for the protocol
(the one a heard broadcast went to). Up to 128 observations, inside the
result's 256 KiB budget. Tuya's 6667 and 7000 broadcasts are encrypted and
reported by port alone. An answer counts only when it carries its protocol's
fields (Tuya's encrypted broadcasts: its frame), so other traffic on these
ports is ignored. Kasa's location, the miIO token field, a camera's serial
and check code and TP-Link's owner hash are never reported.

While it listens the vendor source holds UDP 4002, 6666, 6667, 7000 and 2021.
A relay port the VM opens meanwhile from one of those ports gets another port,
and one already open on them keeps that protocol out of the scan, which then
reports itself truncated (as it does when any of its listeners can't open).
The mDNS source shares 5353 the same way: with a relay port there, the scan's
mDNS queries go from another port and it still hears unicast answers.

## Not here

IPv6 and Matter: lwIP has no NAT66.
