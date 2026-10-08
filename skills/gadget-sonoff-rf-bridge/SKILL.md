---
name: gadget-sonoff-rf-bridge
description: >-
  Capture or transmit user-approved fixed-code 433 MHz actions through a stock Sonoff RF
  Bridge R2 using Muse Home Link. Use compatible HTTP zeroconf firmware, existing device
  credentials, and a confirmed channel map; not rolling codes.
---

# Sonoff RF Bridge R2: Fixed-Code Capture and Transmit

Use this skill when fresh discovery identifies a compatible stock Sonoff RF Bridge R2 and the user requests an approved fixed-code RF action.

## Identify the Device

- Match `_ewelink._tcp`, exact bridge model and firmware. The HTTP zeroconf service commonly uses 8081; use the discovered endpoint.
- Firmware 3.x HTTP zeroconf is distinct from older WebSocket recipes. Do not infer compatibility from the radio frequency or brand alone.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The bridge must already have LAN operation configured and its device ID/devicekey supplied through approved credential storage; no cloud key retrieval is part of this procedure.
- Maintain a private, user-confirmed channel-to-action mapping. Use channels 0–15; the bridge does not provide a reliable local learned-channel inventory.
- Confirm the physical target, channel and effect before capture or transmission, particularly doors, alarms and power outlets.

## Workflow

1. Build a fresh request envelope for each command. Derive the 16-byte AES key as the raw MD5 digest of the devicekey; use a fresh random 16-byte IV, AES-128-CBC, PKCS#7 padding and base64 ciphertext/IV.
2. Encrypt the UTF-8 JSON command payload, then POST to the corresponding `/zeroconf/<command>` endpoint with `sequence` (millisecond string), `deviceid`, literal `selfApikey:"123"`, encrypted `data`, `encrypt:true` and `iv`. Keep all key material out of commands and logs.
3. For user-requested learning on a confirmed free channel N, send `{"cmd":"capture","rfChl":N}` to `/zeroconf/capture`, then ask the user to press the intended remote near the bridge. Do not overwrite an existing mapping without explicit authorization.
4. For a known learned action, send `{"cmd":"transmit","rfChl":N}` to `/zeroconf/transmit` once. Inspect the application response, including any device error, and decode an encrypted response according to its envelope.
5. Record a confirmed channel assignment privately after successful learning. Do not infer RF device state from the channel map or an uncertain transmit response.

## Verify the Result

- HTTP 200 is not physical proof. Confirm the intended device actually acted, using independent state feedback or the user.
- A parseable `/zeroconf/getState` response is not guaranteed; an unsupported-command error does not by itself mean the key is wrong.
- Do not automatically repeat an uncertain RF transmission: some learned actions toggle state or move a device.

## Limits

- Fixed-code 433 MHz only in this scope; no rolling-code replay, infrared or universal remote support.
- No companion client is required; the agent may implement the documented envelope. Do not install replacement firmware, extract keys from accounts or create a cloud fallback.

## Sources

- [cryptography symmetric encryption](https://cryptography.io/en/latest/hazmat/primitives/symmetric-encryption/)
