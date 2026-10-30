---
name: gadget-elgato-key-light
description: >-
  Control Elgato Key Light power, brightness, white temperature, and requested identification
  through Muse Home Link. Use with the local HTTP light API; the documented device is board
  type 53, not other light variants.
---

# Elgato Key Light: Power, Brightness and White Temperature

Use this skill when fresh discovery identifies an Elgato Key Light and the user requests its light controls.

## Identify the Device

- Match `_elg._tcp` with manufacturer Elgato and model/board metadata; the usual API port is 9123, but use the discovered port.
- GET `/elgato/accessory-info` and confirm `productName`, `hardwareBoardType` 53 and `lights` capability. Keep serial/MAC/network metadata private.
- Air, Mini and Light Strip are distinct variants; their additional capabilities are outside this skill.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The light must already be on Wi-Fi. The local HTTP API is unauthenticated; that does not waive host/action authorization.
- Confirm the requested lighting change will not unexpectedly disrupt a call, recording or live stream.

## Workflow

1. GET `/elgato/lights` and read `numberOfLights` plus the target light's current `on`, `brightness` and `temperature`.
2. PUT `/elgato/lights` with `{"numberOfLights":1,"lights":[{"on":1}]}` for on, or `on:0` for off; send only requested fields.
3. For brightness, send `brightness` as an integer percent within the supported range; 3–100 is the useful range for board type 53, while 0–2 may have no visible effect. Use `on:0` for off.
4. For white temperature, send mireds: `round(1000000 / kelvin)`, clamped to 143–344 for board 53. For example, 4000 K is 250 mireds; smaller values are cooler.
5. If explicitly asked to identify the panel, POST `/elgato/identify` once; it visibly blinks and may return an empty body.
6. Poll `/elgato/lights` to read back changes; serialize requests rather than simulating fades with rapid bursts.

## Verify the Result

- Check actual on/brightness/temperature values after a write; convert units clearly.
- An empty successful identify response is expected, but visible identification still needs observation.
- Restore temporary identification settings when appropriate to the request.

## Limits

- No power-on configuration, rename, restart or firmware-update operations under this light-control skill.
- Do not expose this unauthenticated API outside the trusted network or infer color/RGB support from a white-temperature panel.

## Sources

- [Elgato Key Light community API](https://github.com/adamesch/elgato-key-light-api)
