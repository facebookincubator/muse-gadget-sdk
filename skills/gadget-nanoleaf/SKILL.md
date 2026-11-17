---
name: gadget-nanoleaf
description: Control Nanoleaf Light Panels Shapes Canvas Lines Elements and Skylight power brightness hue saturation and color temperature through Muse Home Link after pairing by holding the controller power button and storing the returned token and does not cover effects events layout cloud account features firmware updates or device removal
---

# Nanoleaf Panels and Shapes

Use this skill when the user asks to read or change the power, brightness, hue, saturation, or color temperature of a Nanoleaf Light Panels, Shapes, Canvas, Lines, Elements, or Skylight controller on the local network through Muse Home Link.

## Identify the Device

Confirm the exact model before acting and never infer capability from the vendor name alone. Confirm the device is a supported Nanoleaf controller — Light Panels, Shapes, Canvas, Lines, Elements, or Skylight — on the local network, using fresh discovery rather than a remembered endpoint. Discovery is via mDNS `_nanoleafapi._tcp` or SSDP; Home Link's own discovery uses SSDP, not `_nanoleafapi._tcp`. The local HTTP API base is `http://<device-ip>:16021/api/v1/` where `<device-ip>` is the address returned by that discovery.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Use this API only on the local network or through the Muse Home Link tunnel, never over the open internet.
- Pairing returns an `auth_token` that is embedded in the URL path of every later call as `/api/v1/{token}/…`. Treat the token like a password. Keep it only in user-only approved storage, never paste it into chat, and never log it. Because the token appears in request paths and logs, use it only over the LAN or Home Link tunnel.
- The token lasts until the device is reset. Revoke a token with `DELETE /api/v1/{token}` using the full device URL for that token.
- Pair only with the user's own controller, using a token created for the user in that pairing. Tokens are personal — never use another person's token, and never share or publish your own.

## Workflow

1. Confirm the device is a supported Nanoleaf controller on the local network as described in Identify the Device, and confirm the exact model before acting.
2. Check user-only approved storage for an existing token for this device. If no stored token exists, pair first. Give the user the physical step instead of improvising around it — ask the user to hold the controller power button for 5–7 seconds until the LED flashes. Then, within 30 seconds, send `POST http://<device-ip>:16021/api/v1/new` with no token in the path. The response returns an `auth_token`. Store it as described in Prerequisites and do not show it in chat.
3. Read the current state before any write. Use `GET` on the full state at `http://<device-ip>:16021/api/v1/{token}/state` and on attribute sub-endpoints such as `http://<device-ip>:16021/api/v1/{token}/state/on`. Resolve every value you will use or report from the actual responses, not from memory or assumption.
4. Treat a state question as a question only. A state question is never permission to change anything — if the user only asked about state, report the values read in step 3 and stop.
5. If the values already read match the requested change, treat an already-set value as success and do not send a write.
6. Apply only the requested change with `PUT http://<device-ip>:16021/api/v1/{token}/state` and a JSON body for that change only, such as `{"on":{"value":true}}`, `{"brightness":{"value":50,"duration":0}}`, `{"hue":...}`, `{"sat":...}`, or `{"ct":{"value":...}}`, replacing the placeholder parts with the requested values. Brightness and saturation are 0–100, hue is 0–360 degrees and `ct` is in kelvin; keep each value within the `min` and `max` the state read reported. Elements panels are tunable white only: use brightness and `ct`, not hue or saturation. Success is HTTP 204.
7. Handle errors from the response, never by resending unchanged. Never resend a command automatically. A 401 means the token is no longer accepted — re-pair for a fresh token using step 2. A 403 from `POST /api/v1/new` means the pairing window was not open — ask the user to hold the power button again. A 422 means a malformed body — fix the body, do not resend it unchanged.

## Verify the Result

Read the same attributes back with the same `GET` state calls used in the Workflow, and compare them with the requested change. Report the values actually returned. If a read-back already shows the requested value, that is success. If it does not match, report what was returned and do not resend the write automatically.

## Limits

- No effects, events, or layout operations — those endpoints are not covered by this skill.
- No cloud account features.
- No firmware updates.
- No device removal or reset.
- No use over the open internet.

## Sources

- [Nanoleaf Open API documentation](https://forum.nanoleaf.me/docs/openapi) (needs a free Nanoleaf developer account) — discovery, authorization, state and the PUT bodies above
