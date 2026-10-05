---
name: gadget-sonos-speakers
description: >-
  Control Sonos playback, volume, mute, and speaker grouping through Muse Home Link. Use when
  the intended speaker exposes compatible local UPnP services; resolve the room and group
  coordinator before acting.
---

# Sonos Speakers

Use this skill when fresh discovery identifies a Sonos speaker with a compatible local UPnP service.

## Identify the Device

- Read the advertised device description and service/control URLs to confirm model, player identity and supported services.
- Read zone/group topology and resolve the requested room, stereo pair and current group coordinator. Do not equate every network endpoint with a separately controllable room.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A compatible SoCo client or SOAP implementation using the actual device service URLs through HomeLink.
- Permission for the requested room/group and operation; grouping can interrupt or redirect audio in other rooms.
- For a new media item, an existing URL reachable by the speaker with the needed format and authorization. Do not upload private media or create a server implicitly.

## Workflow

1. Read current transport state, track/media information, volume/mute and group membership using the documented service actions.
2. For volume/mute, target the intended speaker/room or an explicitly requested group operation. Do not assume a per-speaker volume command changes every group member.
3. For playback, address the appropriate group coordinator and use the supported play/pause/stop/seek or existing queue action. Changing transport URI or queue contents is a separate user-authorized action.
4. For a group change, resolve the current coordinator and all intended room IDs, then use the documented join/unjoin operation. Preserve bonded stereo/home-theater configurations.
5. Poll transport, rendering and topology state after actions. Use polling rather than relying on device-initiated event callbacks to the agent.

## Verify the Result

- Distinguish accepted URI, buffering, playing and stopped states. Confirm that the expected room/group is actually the target.
- Read back volume/mute and group membership. Physical audibility may still require user confirmation.
- If a request times out, inspect current state before replaying a queue/group action.

## Limits

- No inbound GENA callback server, on-Link media/TTS server, private port-1443 requirement or cloud fallback.
- Model-specific capabilities and firmware differ; check the requested operation against the device's advertised services.
- No account/service provisioning, firmware updates or stereo/home-theater reconfiguration.

## Sources

- [SoCo Sonos client and documentation](https://github.com/SoCo/SoCo)
