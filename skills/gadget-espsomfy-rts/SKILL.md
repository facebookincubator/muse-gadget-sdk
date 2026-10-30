---
name: gadget-espsomfy-rts
description: >-
  Control commissioned Somfy RTS shades and existing groups through an installed ESPSomfy-RTS
  gateway using Muse Home Link. Use its local HTTP API for supported movement and positioning;
  reported positions are estimates.
---

# ESPSomfy-RTS Gateways

Use this skill when an already-installed ESPSomfy-RTS gateway controls the user's commissioned Somfy RTS shades.

## Identify the Device

- Confirm firmware identity and version through the discovered service and `/controller`. An Espressif MAC prefix is not proof of ESPSomfy-RTS.
- Use the installed API listener; documentation commonly uses port 8081, but its actual advertised/configured endpoint takes precedence.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- An existing gateway with its motors and groups already configured, plus authentication required by the installed firmware.
- User approval for the named shade or group and movement. Confirm an awning or window covering can move safely.

## Workflow

1. Read `/controller`, `/shades` and `/groups` using the documented API. Match stable shade/group IDs and determine supported tilt, positioning and configured movement direction.
2. Read the target's current tracked state. RTS is one-way radio, so position is a timed estimate rather than feedback from the motor.
3. For a requested open/close action, call the firmware's `/shadeCommand` with its actual `shadeId` and documented `up` or `down` command. The API may use HTTP GET for a state-changing command; treat it as a write.
4. Use `my` only after checking its context: it can stop a moving motor or send an idle motor to its favorite position. Do not present it as an unconditional stop.
5. For an existing group use `/groupCommand` with that group's ID. Position and tilt requests require the exact installed firmware's documented parameters and calibrated capabilities; do not guess parameter names or position orientation.

## Verify the Result

- Inspect returned controller/shade state and errors, but label position as estimated.
- Ask for physical confirmation when actual position or successful radio delivery matters. A successful HTTP response cannot prove the motor received the transmission.
- Before retrying, inspect current movement and allow it to settle; repeated radio commands can change the outcome.

## Limits

- No motor pairing/programming, rolling-code manipulation, flashing or new gateway deployment.
- Do not use `/shadeCommand?get=shades` for enumeration; `/shades` is the documented inventory path.
- Do not bypass rain/wind controls or infer that every Somfy motor supports this gateway.

## Sources

- [ESPSomfy-RTS integrations and HTTP API](https://github.com/rstrouse/ESPSomfy-RTS/wiki/Integrations)
