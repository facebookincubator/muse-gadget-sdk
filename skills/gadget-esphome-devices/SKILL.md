---
name: gadget-esphome-devices
description: >-
  Read and control entities exposed by already-installed ESPHome firmware through Muse Home
  Link. Use with an enabled native TCP or HTTP API and its configured credentials; prefer the
  ratgdo skill for ratgdo garage controllers.
---

# ESPHome Devices

Use this skill when the device is confirmed to run ESPHome and already exposes a supported API; an Espressif manufacturer identifier alone is insufficient.

## Identify the Device

- Use the firmware's service metadata and API information to confirm the device, firmware version and enabled interface.
- Enumerate live entities and their stable identifiers, types and capabilities. For ratgdo garage controllers, use the more specific [ratgdo skill](../gadget-ratgdo-v32-garage-door/SKILL.md).

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Already-installed, commissioned firmware with native API or web server enabled.
- The existing native API encryption key/password or web authentication, if configured, plus a compatible client. Enabling an absent API by reflashing is not part of this skill.

## Workflow

1. Connect to the native API using a compatible `aioesphomeapi` client and the current endpoint, or use the installed firmware's documented HTTP API. Preserve native session encryption and configured authentication.
2. List entities, supported commands, ranges and current state. Prefer the native API for structured enumeration when available; web entity IDs and URL conventions are firmware-version dependent.
3. Read sensors or subscribe to state updates over the outbound native connection or HTTP SSE where supported.
4. For a requested change, call only the command advertised for that entity: for example a switch's on/off, a light's supported brightness or a cover's supported movement. Convert units and ranges using that entity's metadata.
5. Follow state updates and any action errors. Distinguish optimistic target states from measured feedback when firmware lacks a physical sensor.

## Verify the Result

- Confirm the intended entity changed and report whether the value is measured, inferred or optimistic.
- Do not retry a toggle, button action or movement command until an uncertain first outcome has been checked.

## Limits

- No universal entity names or URL templates across firmware versions.
- No firmware installation, OTA, arbitrary device services/scripts, generic Bluetooth proxy, network reconfiguration or safety bypasses.
- The entity's existence does not authorize its actuation. Apply the shared physical-action safety rules.

## Sources

- [ESPHome web server API](https://esphome.io/web-api/)
- [ESPHome native API client](https://github.com/esphome/aioesphomeapi)
