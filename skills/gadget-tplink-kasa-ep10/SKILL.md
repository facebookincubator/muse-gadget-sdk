---
name: gadget-tplink-kasa-ep10
description: >-
  Read status and control relay or LED state on compatible TP-Link Kasa EP10 plugs through
  Muse Home Link. Use the confirmed legacy IOT local TCP protocol; EP10 has no energy metering
  and does not use the EP25 authentication path.
---

# TP-Link Kasa EP10: Relay and LED Control

Use this skill when fresh discovery and a scoped device read confirm a compatible Kasa EP10 and the user requests plug status or switching.

## Identify the Device

- Confirm exact model, hardware and firmware from legacy `system.get_sysinfo`, not a vendor prefix or open port.
- Use its discovered local TCP endpoint; the legacy service commonly uses 9999. Do not run client UDP discovery from the agent.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Know the connected load and confirm that switching it is intended.
- The legacy interface needs no account credentials. If current firmware identifies a different protocol, do not force legacy XOR or silently switch to another family's skill.

## Workflow

1. Use python-kasa's explicitly configured legacy IOT plug client through the shared network access; supply the known endpoint without broadcast discovery.
2. For an agent-authored transport, legacy TCP frames use a four-byte big-endian payload length and TP-Link's documented XOR-autokey JSON encoding; prefer the maintained implementation over inventing encryption.
3. Read `{"system":{"get_sysinfo":{}}}` and retain `relay_state` and `led_off`.
4. Set the relay using `system.set_relay_state` with `state` 1 or 0, or the client's `turn_on`/`turn_off`. Set `system.set_led_off` only when an LED change is requested; `off:1` disables the LED.
5. Refresh the device state after each action. Check device error codes, not merely receipt of a TCP frame.

## Verify the Result

- Confirm the desired relay state; LED state is separate and inverted by `led_off`.
- A relay acknowledgement does not prove the connected appliance performed its own task.
- If a response is uncertain, read state before retrying; do not retry with a toggle.

## Limits

- EP10 has no energy meter. Do not promise watts, energy history or EP25 authentication/features.
- No reset, network changes, credential updates, schedules or firmware actions.

## Sources

- [python-kasa](https://github.com/python-kasa/python-kasa)
- [Documented legacy command examples](https://github.com/softScheck/tplink-smartplug)
