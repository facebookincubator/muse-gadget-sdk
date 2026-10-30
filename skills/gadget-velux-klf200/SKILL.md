---
name: gadget-velux-klf200
description: >-
  Read positions and control commissioned windows, blinds, shutters, or existing scenes
  through VELUX KLF200 using Muse Home Link. Use the local binary TCP API with its API
  credential; not KLF150 or new motor pairing.
---

# VELUX KLF200

Use this skill when the device is a VELUX KLF200 gateway with commissioned compatible windows, blinds or shutters.

## Identify the Device

- Confirm KLF200 model and its configured local API service. A VELUX KLF150 is not covered by this API.
- Use node and scene enumeration to identify downstream actuators; the individual radio devices will not appear as independent LAN hosts.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- An existing commissioned KLF200 and its Wi-Fi/API password, which is distinct from its web-login password.
- A compatible binary-protocol client such as pyvlx, routed through HomeLink's TCP transport while preserving the device's TLS requirements.
- Explicit permission for window/shade movement and confirmation that the target area is safe.

## Workflow

1. Establish the documented encrypted local API session and authenticate with the gateway's API credential. Use the confirmed listener, commonly TCP 51200, not an assumed web endpoint.
2. Read gateway information, enumerate nodes/scenes and obtain the target's current position, movement and error state.
3. Use the exact node type's position semantics and supported functions. Send the requested position or stop command, or activate a specifically requested existing scene.
4. Track the API command/session identifiers and completion/status notifications over the outbound connection. Query node state again after movement completes.

## Verify the Result

- Separate command acceptance from completed motion and actual reported position.
- Report blocked movement, rain protection, unreachable actuators or unknown position without overriding protective behavior.
- Check scene membership before execution so the user knows which actuators will move.

## Limits

- No KLF150 support, native HAP assumption, new motor pairing or gateway reconfiguration.
- Do not assume a universal device capacity or single-session restriction. Handle the installed gateway's actual response.
- Preserve window safety, locks and rain protections.

## Sources

- [pyvlx KLF200 client and protocol documentation](https://github.com/Julius2342/pyvlx)
