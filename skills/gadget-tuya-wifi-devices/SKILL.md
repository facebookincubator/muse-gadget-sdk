---
name: gadget-tuya-wifi-devices
description: >-
  Read status and control mapped capabilities on compatible Tuya local-TCP devices through
  Muse Home Link. Use only with a known protocol version, device ID, local key, and exact
  datapoint schema; prefer a matching device-specific skill.
---

# Tuya Wi-Fi Devices

Use this skill when the exact device is confirmed to support Tuya local TCP and its protocol, key and model-specific datapoint map are known.

## Identify the Device

- Confirm device ID, model, firmware and protocol version from existing approved configuration/documentation.
- Do not infer device type or protocol version from a manufacturer prefix or discovery ports 6666/6667. The control endpoint is separate, commonly TCP 6668, and must be confirmed.
- Prefer a specific skill where available, such as [compatible eufy RoboVacs](../gadget-eufy-robovac-tuya/SKILL.md).

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Existing device ID, local key, protocol version and a trustworthy model/firmware datapoint schema, supplied securely.
- A compatible TinyTuya client with local TCP transport routed through HomeLink.
- Permission for the specific actuator/load. A raw datapoint's existence does not establish its meaning or authorize writing it.

## Workflow

1. Initialize the appropriate local device class using the confirmed identity, current endpoint, existing local key and explicit protocol version. Do not run a cloud setup wizard as part of control.
2. Read current status and map each returned datapoint through the known schema, including data type, units, range and enumeration values.
3. For a requested change, write only the mapped datapoint and valid desired value using the compatible client. Do not assume datapoint 1 is always an on/off switch.
4. Read fresh state afterward and compare the mapped value. Handle protocol errors, unavailable datapoints and optimistic devices explicitly.

## Verify the Result

- An encrypted connection or write acknowledgment is not proof of the intended physical result.
- Report unknown datapoints as unknown; never discover their purpose by trial-and-error writes.
- If the key no longer works, stop for updated authorized credentials rather than re-pairing automatically.

## Limits

- Not all Tuya-branded devices expose this interface; battery devices, gateways and new firmware can differ.
- No cloud wizard/login, key extraction, automatic re-pairing, arbitrary raw datapoint writes or firmware changes.
- Do not generalize a protocol version or datapoint schema across unrelated models.

## Sources

- [TinyTuya local protocol and device documentation](https://github.com/jasonacox/tinytuya)
