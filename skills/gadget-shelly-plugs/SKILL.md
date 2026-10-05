---
name: gadget-shelly-plugs
description: >-
  Switch Shelly Gen 4 plugs and read supported power or energy measurements through Muse Home
  Link. Use the local Shelly RPC API after confirming the actual plug and attached load; not
  the Gen 1 protocol.
---

# Shelly Plugs (Gen 4): Switching and Power Metering

Use this skill when fresh discovery identifies a Shelly Gen 4 plug and the user requests switching or power readings.

## Identify the Device

- Match `_http._tcp` model/hostname metadata such as `ShellyPlugUSG4-<device-id>`; prefixes and names alone are insufficient.
- Read `/rpc/Shelly.GetDeviceInfo` and check `model`, `app`, `gen`, firmware and `auth_en`. Use the current discovered IPv4/port, never a remembered DHCP address.
- Gen 1 does not use this RPC path. Gen 2+ API similarity is not a claim that every device has the Gen 4 plug's capabilities.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Identify what is plugged in before switching, especially networking, heating, medical or other consequential loads.
- When `auth_en` is true, use the documented HTTP/RPC digest authentication with approved stored credentials; the device username is `admin`. Never place the password in a command.

## Workflow

1. Confirm identity, then read `/rpc/Shelly.GetStatus` and the intended switch component. Shelly Plug US Gen 4 uses switch ID 0; confirm component IDs on other compatible hardware.
2. Read `/rpc/Switch.GetStatus?id=0` before acting. Retain `output`, `apower` and relevant fault flags.
3. Request the desired state with `/rpc/Switch.Set?id=0&on=true` or `on=false`. A write remains a state-changing action even when the vendor uses an HTTP GET endpoint.
4. For an explicitly requested temporary action, `Switch.Set` accepts `toggle_after=<seconds>` so reversal runs on the plug; explain the timer and its consequences before setting it.
5. Read `Switch.GetStatus` again. The `Switch.Set` reply's `was_on` is the previous state, not the resulting state.

## Verify the Result

- Check `output` for relay state and `apower` for measured watts. Relay-on with zero watts may mean an idle or disconnected load, not failed control.
- Report actual voltage/current/energy fields and units when exposed; `aenergy` totals are in watt-hours.
- On an uncertain response, read current state before retrying. Do not use toggle as a retry of an explicit on/off request.

## Limits

- Do not create schedules, install scripts, reset or update firmware, or modify protection thresholds under a switching request.
- Gen 4 may expose illuminance, MQTT, BLE or Matter, but those are not assumed alternative runtime paths here.
- Prefer explicit on/off to `/rpc/Switch.Toggle`; persistent `Switch.SetConfig` changes need their own requested scope.

## Sources

- [Shelly RPC API](https://shelly-api-docs.shelly.cloud/gen2/)
- [Switch component](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Switch/)
