---
name: gadget-roku-tvs-and-players
description: >-
  Control Roku TVs, streaming sticks/boxes and Roku-powered displays that answer the local
  External Control Protocol (ECP) on TCP 8060 through Muse Home Link. Covers discovery, device
  info, apps, remote keys, text entry, deep links, search, and Roku TV volume, input and power.
---

# Roku TVs and Players (ECP)

Local HTTP control over the LAN using Roku's External Control Protocol (ECP), sent through
Muse Home Link. No cloud account, pairing or client key; access is governed by a
network-control setting on the device (see Limits).

## Identify the Device

- Confirm the target is a Roku: `GET http://<ip>:8060/query/device-info` must return XML
  containing `<vendor-name>Roku` and a `<model-number>`.
- Also read `model-name`, `software-version`, `power-mode` and `is-tv`. Players and TVs
  differ in which keys they accept, and behavior varies by OS version.
- Discovery:
  - Known IP: the user can find it under Settings > Network > About. A DHCP reservation keeps it stable.
  - SSDP: multicast `M-SEARCH` to `239.255.255.250:1900` with `ST: roku:ecp`. The `LOCATION`
    header is the base URL (`http://<ip>:8060/`).
  - mDNS: Roku TVs with AirPlay advertise `_airplay._tcp`, `_hap._tcp`, `_spotify-connect._tcp`
    and `_display._tcp`; the TXT record carries `manufacturer=Roku` and the A record the IP.
  - If SSDP and mDNS find nothing, ask the user for the IP. Do not scan the subnet.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Read `ecp-setting-mode` first. On a Roku TV running OS 16.0.4 the default is `limited`,
  where only `/query/device-info` and `/query/active-app` answer; everything else returns
  **403** (`ECP command not allowed in Limited mode.`). If the mode is not `enabled` or
  `permissive`, tell the user the setting must be changed on the TV and stop. Do not retry
  or look for a workaround. A **401** on `device-info` means the setting is `Disabled`;
  same advice.
- ECP has no per-request authentication. Only talk to a device the user named or approved,
  stay on the local network, and never expose port 8060 to the internet.
- Get user confirmation before anything with side effects: launching apps, power keys,
  input changes, typing text, Channel Store pages. Reads are always fine.

## Workflow

1. Confirm the device and read `ecp-setting-mode` (see Identify the Device and Prerequisites).
2. Read the state the request depends on: `/query/active-app`, `/query/apps`, `/query/media-player`
   or `power-mode`.
3. Get user confirmation for anything with side effects, then send one request per call.
   For menu navigation, keys about 170 ms apart worked reliably.
4. Re-query the affected state, because an accepted request does not prove the screen changed.

Common cases:

- **What's playing?** `/query/active-app`, then `/query/media-player`.
- **Open an app by name:** `/query/apps`, match the name, `POST /launch/<id>`.
- **Type a search string:** `Search` key, then `Lit_` per character. Then stop and ask the user
  what is on screen: ECP can't read search results or keyboard focus. Do not send `Select`
  right after typing: focus is usually still on the on-screen keyboard, so `Select` may type
  the highlighted letter instead of opening a result. To open a result, move focus to the
  results first and confirm with the user. On some voice-remote models the `Search` key may
  start voice input.
- **Turn the TV on or off:** check `power-mode`, send the key, wait a few seconds, check again.

## Verify the Result

- Re-query the state the action should have changed (`active-app`, `media-player`, `power-mode`).
  An HTTP 200 means the request was accepted, not that the screen changed.
- After the user changes the network-control setting, re-read `/query/device-info` and report
  the new `ecp-setting-mode`.

## Request Shape

### Reads (GET, XML)

| Endpoint | Returns |
| --- | --- |
| `/query/device-info` | model, software version, power mode, network type, `ecp-setting-mode`, `ecp-tls-enabled`, `supports-warm-standby`, `supports-wake-on-wlan`, and more. `supports-tv-power-control` and `supports-audio-volume-control` are documented for OS 15+ but were absent on an OS 16.0.4 TV, so do not depend on them. |
| `/query/apps` | installed channels with numeric app IDs |
| `/query/active-app` | foreground app; on the home screen it returns `<app>Roku</app>` with no `id` |
| `/query/media-player` | player state, plugin, position, duration |
| `/query/icon/<appId>` | app icon (binary) |
| `/query/tv-channels`, `/query/tv-active-channel` | tuner lineup and current channel (TV models only); empty elements are normal when no lineup is set up |

Treat `serial-number`, `device-id`, `wifi-mac`, `ethernet-mac`, `bluetooth-mac`, `user-device-name`,
`default-device-name` (contains a serial fragment), `network-name` (Wi-Fi SSID) and
`user-device-location` as sensitive. Do not echo them unless asked.

### Actions (POST, empty body)

```
curl -s -X POST http://<ip>:8060/keypress/Home
curl -s -X POST http://<ip>:8060/launch/<appId>
curl -s -X POST "http://<ip>:8060/launch/<appId>?contentID=<id>&MediaType=<type>"
curl -s -X POST "http://<ip>:8060/search/browse?keyword=<text>&launch=true"
curl -s -X POST http://<ip>:8060/install/<appId>
```

- **Keys** (`/keypress/<KEY>`; `keydown`/`keyup` to hold): Home, Back, Select, Up, Down, Left,
  Right, Rev, Play, Fwd, InstantReplay, Info, Search, Enter, Backspace, FindRemote.
- **Roku TV only:** VolumeUp, VolumeDown, VolumeMute, PowerOff, ChannelUp, ChannelDown,
  InputTuner, InputHDMI1-4, InputAV1. `PowerOn` and `Power` are also accepted (see Power).
- **Text entry:** one request per character, `/keypress/Lit_<char>`. Percent-encode the
  character as UTF-8 unless it is an ASCII letter, digit, `-`, `.`, `_` or `~`: space is
  `Lit_%20`, `#` is `Lit_%23`, `?` is `Lit_%3F`, `&` is `Lit_%26`, `€` is `Lit_%E2%82%AC`.
- **Deep links:** `contentID` (under 255 chars, URL-encoded) and `MediaType` (series, season,
  episode, movie, shortFormVideo, tvSpecial). Support is per app; an unsupported link just
  opens the app.
- **Search:** `keyword` is required; optional `title`, `type` (movie, tv-show, person, channel,
  game), `season`, `tmsid`, `match-any`, `provider`, `provider-id`, `show-unavailable`, `launch`.
  Percent-encode each value (`&` in a keyword is `%26`). Reliability is limited (see Limits).
- `/install/<appId>` only opens the Channel Store page; the user confirms the install on screen.

## Limits

- **Network control setting** (Settings > System > Advanced system settings > Control by mobile
  apps > Network access): `Limited` (default), `Enabled`, `Permissive`, `Disabled`. Observed on
  an OS 16.0.4 Roku TV: `Limited` answers only device-info and active-app (403 elsewhere);
  `Enabled` and `Permissive` both allowed every read and keypress tried; `Disabled` returned 401.
  Differences in scope between `Enabled` and `Permissive` (such as which source addresses are
  allowed) were not tested. Only the user can change the setting on the TV. Afterwards re-read
  `/query/device-info` and report the new `ecp-setting-mode`.
- **Power** (Roku TV 65R8CX, OS 16.0.4): `PowerOff`, `PowerOn` and `Power` worked; `PowerToggle`
  did not. In standby the TV kept answering ECP and `power-mode` read `Ready` (on is `PowerOn`).
  `PowerOn` and `Power` are not in Roku's public key list, so treat them as observed behavior,
  not a guarantee. Other models, players and deeper standby states may not answer at all, and
  ECP cannot wake a device that is not listening.
- **Search is best effort.** `/search/browse` can return 200 with missing results; fall back to
  typing with `Search` plus `Lit_` (see Workflow).
- LAN only, plain HTTP. `ecp-tls-enabled` was `true` on the tested TV; plain HTTP on 8060 still worked.
- Developer-only endpoints (`chanperf`, `sgnodes`, `registry`, `exit-app`, `fwbeacons`, ...)
  need developer mode and are out of scope.
- No screenshots, no account sign-in, no sideloading, and no control of DRM playback beyond the
  normal remote keys. Do not change device settings other than through the normal remote keys
  the user asked for.

## Sources

- Roku ECP reference: https://developer.roku.com/docs/developer-program/dev-tools/external-control-api.md
- Archived SDK ECP doc (apps, active-app, install, search/browse): https://sdkdocs-archive.roku.com/External-Control-API_1611563.html
- Roku Community, "Did 12.5 break search/browse ECP?": https://community.roku.com/discussions/developer/did-12-5-break-searchbrowse-ecp/912374
