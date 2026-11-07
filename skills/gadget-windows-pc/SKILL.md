---
name: gadget-windows-pc
description: >-
  Wake a Windows PC remotely with Wake-on-LAN and verify it through its real
  services: the magic packet must be sent by a device already on the PC's LAN
  (broadcasts do not route), so a Muse Home Link-reachable always-on device
  sends it. Covers the target registry, sender lanes, Windows NIC/Fast Startup
  requirements, and verify-by-service discipline. Wake only — shutdown and
  sleep scheduling are separate concerns.
---

# Windows PC (Wake-on-LAN)

Use this skill when a confirmed Windows PC on the home LAN needs to be woken
remotely — because the user asked, or because a PC-hosted capability (a cast
helper, a build, a file service) is needed and the machine is asleep.

## Identify the Device

- Confirm the PC's **wired** Ethernet MAC address from the machine itself
  (`getmac /v`) — WoL identity is the MAC, not the (DHCP) IP. Keep the
  last-known IP only as a unicast copy destination.
- Record which low-power states are in play if known; sleep (S3) and
  shutdown (S5) have different WoL requirements (below).

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A **sender on the same LAN**: any always-on device Muse can drive —
  an Android TV box over ADB, another PC, an ESP32-class node, or the
  router's own WoL page where offered. A cloud VM cannot send a magic
  packet; do not attempt delivery from off-LAN and report success.
- On the PC: NIC properties "Allow this device to wake the computer" +
  "Only allow a magic packet to wake the computer" (vendor "Wake on
  Magic Packet" enabled). Verify once while the PC is awake rather
  than discovering it during a failed wake.

## Workflow

1. Build the 102-byte magic packet: 6× `0xFF`, then the target MAC
   repeated 16 times.
2. From the sender, blast it to the subnet broadcast (e.g.
   192.168.1.255) **and** the target's unicast IP, on ports 9 and 7,
   3 rounds. (On an Android sender, pushed-file + `nc -u` works;
   toybox `nc` blocks ~20 s per call after sending — slow is normal.)
3. **Verify by service, never by packet delivery:** poll the service
   the wake was for (SSH answering, the hosted app responding) for
   up to ~2 minutes. A sent packet proves nothing.

## Verify the Result

- The PC answers through its real service (e.g. SSH returns its
  hostname). Report wake state and service state separately.
- If it wakes for some requests but not from full shutdown, suspect
  Windows **Fast Startup** first (shutdown becomes a hybrid hibernate
  that commonly breaks WoL-from-off), then BIOS ErP/Deep Sleep.

## Limits

- WoL is **operator-invoked**: wake a machine because the user asked
  or a task needs it — never speculatively, and never just to poll or
  sample a sleeping machine that its owner prefers off.
- Wi-Fi WoL (WoWLAN) is a different, rarely-supported feature; this
  skill is wired-Ethernet only.
- No shutdown/sleep control here, and no BIOS changes without the
  user present — a wrong BIOS change can strand the machine.

## Sources

- [Microsoft: Wake-on-LAN for Windows](https://learn.microsoft.com/en-us/windows-server/administration/performance-tuning/energy-efficiency/wake-on-lan)
- [Wake-on-LAN magic packet format (AMD white paper, widely mirrored)](https://en.wikipedia.org/wiki/Wake-on-LAN#Magic_packet)
