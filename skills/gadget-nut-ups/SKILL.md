---
name: gadget-nut-ups
description: Read UPS status and battery information from a Network UPS Tools upsd server through a Linux Muse gadget and report only what upsd returns without changing any UPS or host state and without using Muse Home Link because the NUT protocol is raw TCP and is not tunnel compatible
---

## Network UPS Tools UPS Status

Use this skill when you want a read-only check of a UPS that is already served by Network UPS Tools, including whether it is on line power or on battery and what charge, runtime, load, and input voltage it reports. The gadget talks to upsd, never to the UPS directly.

## Identify the Device

Identify the UPS name and the upsd host from the user's own NUT configuration before making any request. Ask the user for those values if they are not already known from their configuration, and use only the name and host they confirm. Do not guess a UPS name, do not assume a default name, and do not scan the network looking for upsd servers.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

Those safety rules still apply, but the transport differs. This skill runs from a Linux Muse gadget on the same network as upsd, not through Muse Home Link. NUT serves UPS data through upsd using a raw ASCII line protocol on TCP port 3493. That protocol is not HTTP, so it cannot pass through a Muse Home Link tunnel.

Reads are anonymous by design, and upsd needs no credentials for read-only access. Do not ask the user for a password for a read. Note that upsd listens on localhost only by default, so LAN access requires an explicit LISTEN entry in upsd.conf. If the gadget cannot reach upsd from the LAN, tell the user that step instead of improvising around it, and never edit upsd configuration on the user's behalf without them asking.

A state question is never permission to change anything, and here nothing is ever changed. Never resend requests in a tight loop. Report exactly what upsd returned.

## Workflow

1. Confirm the UPS name and the upsd host from the user's own NUT configuration, as described under Identify the Device.
2. Discover first. Send `LIST UPS` to upsd on TCP port 3493 to enumerate the UPS names it serves, and confirm the user's UPS is in that list.
3. Send `LIST VAR <ups-name>` to discover the variables that UPS and driver actually expose. Work only from the variables in that response.
4. Prefer the `upsc` command line wrapper where it is available on the gadget, for example `upsc <ups-name>` for the full variable list and `upsc -j` for JSON output.
5. For a single value, use `GET VAR <ups-name> <variable>` or the equivalent `upsc` read for that one variable.
6. Read the useful variables when they were listed in step 3, such as `ups.status`, `battery.charge`, `battery.runtime`, `ups.load`, and `input.voltage`. Interpret `ups.status` as reported, where OL means on line power, OB means on battery, and LB means low battery.
7. Stop after the requested reads. Variable sets are driver and model dependent, so never hard-code a variable list and never invent a variable the response did not list.

## Verify the Result

Check every value against the `LIST VAR` response for that UPS before reporting it. Report exactly what upsd returned, with the UPS name the value came from.

If upsd reports a DATA-STALE or DRIVER-NOT-CONNECTED state, report the reading as stale or disconnected. Never present a stale or disconnected state as a live reading, and never present it as 0 percent.

If a variable is missing, say that the driver does not expose it. A missing variable does not mean the value is zero.

## Limits

This skill is read-only only, and the write and control commands SET VAR, INSTCMD and FSD are excluded and must never be used.

This skill does not shut down or start up any host, does not change upsd or driver configuration, does not talk to a UPS directly over USB or serial, and does not monitor a UPS over the open internet. It reads only from an upsd server reachable from the Linux Muse gadget on the local network, and it changes nothing.

## Sources

- Network UPS Tools documentation at networkupstools.org
- The NUT network protocol description from Network UPS Tools
- The upsc manual page from Network UPS Tools
