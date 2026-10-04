---
name: gadget-apple-tv-4k
description: >-
  Control Apple TV 4K remote keys, installed apps, keyboard input, and supported sleep/wake
  actions through Muse Home Link. Use with compatible local pyatv services and normal
  user-approved pairing.
---

# Apple TV 4K

Use this skill when discovery and device information identify an Apple TV 4K with supported local services.

## Identify the Device

- Confirm the generation, OS build and service endpoints. Example identifiers are `AppleTV6,2` for first generation and `AppleTV11,1` for second generation.
- Read the connected client's feature availability; do not assume all generations, apps and firmware expose the same controls.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A compatible pyatv client using HomeLink TCP transport and current discovered service data.
- Normal user-approved on-screen PIN pairing for each required protocol, typically Companion and/or AirPlay. Store the resulting credentials securely and retain session encryption.
- A person able to read a pairing PIN when needed. Do not print credential storage or loosen the TV's access settings as a workaround.

## Workflow

1. Reuse valid pairing credentials; if absent, request normal pairing with the user and complete the displayed PIN challenge for the chosen protocol.
2. Read device information, features, power state and available now-playing state before choosing a command.
3. Send only the requested supported remote key or sleep/wake action. Wake may also affect a connected display via HDMI-CEC.
4. For app launch, obtain app identifiers from `app_list` and launch the selected installed app. For keyboard entry, confirm text focus and send the user's intended text using the supported keyboard API.
5. Inspect available state after the action. The reported `app` is generally the now-playing app, not proof of which app is currently in the foreground.

## Verify the Result

- Confirm power or playback state where available. Remote-key success does not guarantee the intended on-screen navigation outcome.
- For text/app actions without reliable state feedback, ask for visual confirmation rather than claiming success from an accepted command.
- If pairing expires or is revoked, re-establish normal pairing with the user rather than repeatedly issuing controls.

## Limits

- Do not recommend `play_url` on tvOS 26.6; request acceptance does not ensure playback.
- No UDP media streaming, account switching, screenshots, configuration bypasses or signed-in Mac dependency.

## Sources

- [pyatv Apple TV client](https://github.com/postlund/pyatv)
- [pyatv pairing and feature documentation](https://pyatv.dev/)
