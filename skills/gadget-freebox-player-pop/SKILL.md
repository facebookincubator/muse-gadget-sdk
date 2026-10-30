---
name: gadget-freebox-player-pop
description: >-
  Control Freebox Player Pop media through Google Cast or remote keys and app links through
  Android TV Remote v2 using Muse Home Link. Use whichever local service is actually
  available; not for the Freebox router/server.
---

# Freebox Player Pop

Use this skill when the confirmed Freebox Player Pop currently exposes a Cast receiver and/or Android TV Remote v2 service.

## Identify the Device

- Confirm Player Pop identity and firmware. A Freebox router/server is not the Player Pop.
- Treat Cast and Remote v2 as separate services with separate capabilities; do not infer one from the presence of the other.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The player is already set up and the requested interface is enabled and available.
- For Remote v2, a compatible `androidtvremote2` client routed through HomeLink, a persistent client certificate/private key, and the user's normal on-screen pairing code.
- For Cast playback, a suitable existing media source reachable by the receiver.

## Workflow

1. For media/status/volume through Cast, follow the [Google Cast skill](../gadget-google-cast/SKILL.md); apply the player's current format, application and standby limitations.
2. For local remote control, select the current Remote v2 endpoint and check the installed client's documented pairing/control service configuration. Pairing and control use distinct connections.
3. If credentials are absent, initiate normal Remote v2 pairing with the user, enter the displayed code and securely preserve the resulting certificate/private-key identity. Do not regenerate that identity on every connection.
4. Open the authenticated control connection and send only the requested supported remote key or documented app link using `androidtvremote2`. App links must target an installed handler and do not constitute package management.
5. Use the available remote-state updates or user confirmation to assess the result; use the shared skill separately for Cast result verification.

## Verify the Result

- A successful pairing is not proof of remote-command success, and an accepted key is not proof of the intended screen outcome.
- Verify the correct player/session before changing playback or waking a connected display.

## Limits

- No screenshots, package management, private setup endpoints, developer/debugging setup or fallback commands.
- No claim that Cast wakes the player or that every Player Pop firmware exposes all Remote v2 features.
- No new media server or automatic app entitlements.

## Sources

- [Shared Google Cast skill](../gadget-google-cast/SKILL.md)
- [Android TV Remote v2 client](https://github.com/tronikos/androidtvremote2)
