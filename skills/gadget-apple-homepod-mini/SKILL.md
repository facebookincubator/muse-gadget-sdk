---
name: gadget-apple-homepod-mini
description: >-
  Control HomePod mini volume, supported existing-session playback, and output groups through
  Muse Home Link. Use when the requested speaker exposes compatible local pyatv services; not
  for starting new audio streams.
---

# Apple HomePod mini

Use this skill when fresh discovery identifies a HomePod mini, commonly model identifier `AudioAccessory5,1`, with a compatible local control service.

## Identify the Device

- Match model identity, current AirPlay/RAOP service endpoints and the intended speaker or existing stereo-pair primary.
- Use pyatv device information and feature availability as the capability boundary. A service advertisement alone does not prove every control is available.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A compatible pyatv client with its control sockets routed over HomeLink, using discovery already supplied by Link rather than agent-side multicast.
- Honor the speaker's existing access/password/pairing requirements and preserve any credentials and session encryption. Do not weaken access settings to obtain a connection.
- Permission for the requested audio action and its target speakers.

## Workflow

1. Construct the client configuration from the current device identity and supported service endpoints. Connect only the local TCP control paths needed for the operation.
2. Read `device_info`, feature availability, volume and available output-device/session state. `Unavailable` during idle is different from permanently unsupported.
3. For a requested volume change, use the supported volume setter or step operation with the documented scale, then read volume back. Some firmware has acknowledged a setter without changing the level.
4. For transport controls, check that the existing session exposes the requested play/pause/next/previous capability. Do not assume this client can inspect or control all playback started elsewhere.
5. For a requested group change, use supported output-device operations with explicitly identified existing receiver IDs and confirm group membership afterward. Group changes can affect other rooms and are not part of discovery.

## Verify the Result

- Read volume and the relevant session/group state after an action. Report unavailable metadata honestly.
- Do not infer that a stereo pair can be created or dismantled through the output-device API.

## Limits

- No general RAOP/UDP audio streaming, media hosting on Link, signed-in Mac automation, Siri/Intercom, Home configuration or temperature/humidity access is promised.
- Do not recommend `play_url` for this device. Playback control of a supported existing session is distinct from creating a new media stream.
- Other HomePod generations and third-party AirPlay receivers require their own capability checks.

## Sources

- [pyatv supported devices, features and protocols](https://github.com/postlund/pyatv)
- [pyatv documentation](https://pyatv.dev/)
