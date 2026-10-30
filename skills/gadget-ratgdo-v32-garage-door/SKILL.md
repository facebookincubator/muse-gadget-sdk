---
name: gadget-ratgdo-v32-garage-door
description: >-
  Read and operate installed ratgdo v32 garage-door, light, and lock entities through Muse
  Home Link. Use this device-specific skill when ratgdo firmware and capabilities are
  confirmed; require safe, authorized movement and preserve interlocks.
---

# ratgdo v32 Garage Door Controller

Use this skill when current discovery and entity metadata identify an already-installed ratgdo v32 controller, not merely an arbitrary ESP32.

## Identify the Device

- Confirm ratgdo model, installed firmware version and actual garage-door cover, light, lock and obstruction entities.
- Record entity IDs from the live API. Web-server paths and identifiers differ by ESPHome version; do not transplant another firmware's URLs.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- An already commissioned controller attached to a working opener, and credentials for its enabled HTTP or native TCP API.
- An explicit requested door/light/lock action. Confirm the intended door and that movement is safe; preserve all opener interlocks and obstruction protections.

## Workflow

1. Read the installed firmware's entity descriptions and current door, movement, lock and obstruction states using its enabled API. SSE may supply live state over an outbound HTTP connection; the native API can list entities and subscribe over TCP.
2. For HTTP, construct the entity read/action URL from that firmware's documented web API. For the native API, use a compatible `aioesphomeapi` client with the device's configured encryption key or credentials.
3. Use only the exposed cover open/close/stop, light or lock operation corresponding to the user's request. Do not infer position-setting or lock semantics from the entity name alone.
4. Monitor the door and obstruction states until the requested state is reached or movement stops unexpectedly. If the command result is uncertain, read state before considering any retry.

## Verify the Result

- Distinguish command acceptance, opening/closing and final open/closed states.
- Report obstruction, unknown position, offline sensors or a lock preventing movement as unresolved; do not override them.
- A light or lock response must be checked against its entity state, not taken as proof of door movement.

## Limits

- No retrofit installation, flashing, OTA updates, opener rewiring or safety bypasses.
- Only capabilities present in the installed firmware are supported. Prefer this device skill over the generic [ESPHome skill](../gadget-esphome-devices/SKILL.md).

## Sources

- [ESPHome web server API](https://esphome.io/web-api/)
- [ESPHome native API client](https://github.com/esphome/aioesphomeapi)
- [ratgdo ESPHome firmware](https://github.com/ratgdo/esphome-ratgdo)
