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

- Read `ecp-setting-mode` first. On a Roku TV running OS 16.0.4 the default is `limited`, where
  only `/query/device-info` and `/query/active-app` answer; everything else returns **403**
  (`ECP command not allowed in Limited mode.`). Continue if the mode is `enabled`, `permissive` or
  `default`. If it is `limited`, tell the user the setting must be changed on the device (which
  value to ask for: see Limits) and stop. For any other value, send one `GET /query/apps`:
  continue if it answers, otherwise give the same advice and stop. Only `limited`, `enabled` and
  `permissive` have been observed on hardware; `default` and other values have not. Do not retry
  or look for a workaround. A **401** on `device-info` means the setting is `Disabled`; same
  advice.
- ECP has no per-request authentication. Only talk to a device the user named or approved,
  stay on the local network, and never expose port 8060 to the internet.
- Get user confirmation before anything with side effects: launching apps, power keys,
  input changes, typing text. Reads are always fine.

## Workflow

1. Confirm the device and read `ecp-setting-mode` (see Identify the Device and Prerequisites).
2. Read the state the request depends on: `/query/active-app`, `/query/apps`, `/query/media-player`
   or `power-mode`.
3. Get user confirmation for anything with side effects, then send one request per call.
   For menu navigation, keys about 170 ms apart worked reliably.
4. Re-query the affected state, because an accepted request does not prove the screen changed.

Common cases:

- **What's playing?** `/query/active-app`, then `/query/media-player`.
- **Open an app by name:** `/query/apps`, match the name, `POST /launch/<id>`. If the app
  is not in the list, it is not installed: ask the user to add it from the Roku Streaming
  Store. ECP can't install it (see Limits).
- **Type a search string:** `Lit_` only works while an on-screen keyboard is showing. Send the
  `Search` key, then ask the user whether a search screen with a keyboard is showing: Roku's
  current reference says `Search` makes Roku Voice listen, so it may not open one. If no
  keyboard is showing, ask the user to open the search screen. Only after the user confirms the
  keyboard, send `Lit_` per character. Then stop and ask the user what is on screen: ECP can't
  read search results or keyboard focus. Do not send `Select` right after typing: focus is
  usually still on the on-screen keyboard, so `Select` may type the highlighted letter instead
  of opening a result. To open a result, move focus to the results first and confirm with the
  user.
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
```

- **Keys** (`/keypress/<KEY>`; `keydown`/`keyup` to hold): Home, Back, Select, Up, Down, Left,
  Right, Rev, Play, Fwd, InstantReplay, Info, Search, Enter, Backspace, FindRemote.
- **Roku TV only:** VolumeUp, VolumeDown, VolumeMute, PowerOff, ChannelUp, ChannelDown,
  InputTuner, InputHDMI1-4, InputAV1. `PowerOn` and `Power` are also accepted (see Power).
- **Text entry:** only while an on-screen keyboard is showing, one request per character,
  `/keypress/Lit_<char>`. Percent-encode the character as UTF-8 unless it is an ASCII letter,
  digit, `-`, `.`, `_` or `~`: space is `Lit_%20`, `#` is `Lit_%23`, `?` is `Lit_%3F`, `&` is
  `Lit_%26`, `€` is `Lit_%E2%82%AC`.
- **Deep links:** `contentID` (under 255 chars, URL-encoded) and `MediaType` (series, season,
  episode, movie, shortFormVideo, tvSpecial). Support is per app; an unsupported link just
  opens the app.
- **Search (legacy):** `POST /search/browse?keyword=<text>` comes from the archived reference.
  Roku's current reference says the search command is no longer available as of Roku OS 12.0, so
  use it only if the user has seen it work on this device, and otherwise type the search string
  (see Workflow). `keyword` is required; optional `title`, `type` (movie, tv-show, person,
  channel, game), `season`, `tmsid`, `match-any`, `provider`, `provider-id`, `show-unavailable`,
  `launch`. Percent-encode each value (`&` in a keyword is `%26`).

## Limits

- **Network control setting** (Settings > System > Advanced system settings > Control by mobile
  apps > Network access; Roku's support article calls the menu External Control). The values
  differ by device and OS version: the tested TV listed `Limited` (default), `Enabled`,
  `Permissive` and `Disabled`, while Roku's support article lists `Default`, `Permissive` and
  `Disable`, and describes `Permissive` as meant for publicly addressable networks. Ask the user
  for `Enabled` (or `Default`, where that is the name) first, and suggest `Permissive` only if ECP
  still refuses. Observed on an OS 16.0.4 Roku TV: `Limited` answers only device-info and
  active-app (403 elsewhere); `Enabled` and `Permissive` both allowed every read and keypress
  tried; `Disabled` returned 401. Differences in scope between `Enabled` and `Permissive` (such as
  which source addresses are allowed) were not tested. A Roku Streaming Stick 4K in `Limited`
  answered device-info and active-app and refused `/query/apps` with the same message (status code
  not recorded), so expect `Limited` on players too. The stick was then tested only in
  `Permissive`, where it answered `/query/apps`, keypresses and launches. Only the user can change
  the setting on the device. Afterwards re-read `/query/device-info` and report the new
  `ecp-setting-mode`.
- **No channel installs.** `POST /install/<appId>` returned **401** on a Roku Streaming Stick 4K
  set to `Permissive` and installed nothing. Roku's current ECP reference no longer lists
  `install`; the archived reference described it as opening the channel's store page for the
  user to confirm, and noted that `launch` fails for channels that aren't installed, which
  matched the stick. If a channel is missing from `/query/apps`, ask the user to add it from the
  Roku Streaming Store themselves.
- **Power** (Roku TV 65R8CX, OS 16.0.4): `PowerOff`, `PowerOn` and `Power` worked; `PowerToggle`
  did not. In standby the TV kept answering ECP and `power-mode` read `Ready` (on is `PowerOn`).
  `PowerOn` and `Power` are not in Roku's public key list, so treat them as observed behavior,
  not a guarantee. Other models, players and deeper standby states may not answer at all, and
  ECP cannot wake a device that is not listening.
- **Search is legacy.** Roku's current ECP reference says the search command is no longer
  available as of Roku OS 12.0, and `/search/browse` can return 200 with missing results. Prefer
  typing the search string (see Workflow).
- LAN only, plain HTTP. `ecp-tls-enabled` was `true` on the tested TV; plain HTTP on 8060 still worked.
- Developer-only endpoints (`chanperf`, `sgnodes`, `registry`, `exit-app`, `fwbeacons`, ...)
  need developer mode and are out of scope.
- No screenshots, no account sign-in, no sideloading, and no control of DRM playback beyond the
  normal remote keys. Do not change device settings other than through the normal remote keys
  the user asked for.

## Sources

- Roku ECP reference: https://developer.roku.com/docs/developer-program/dev-tools/external-control-api.md
- Archived SDK ECP doc (apps, active-app, launch, install, search/browse), Wayback Machine copy (the original site is offline): https://web.archive.org/web/20221129133317/https://sdkdocs-archive.roku.com/External-Control-API_1611563.html
- Roku support, "Can my Roku streaming device be controlled using an app?" (External Control > Network access values): https://support.roku.com/en-ca/article/can-my-roku-streaming-device-be-controlled-using-an-app
- Roku Community, "Did 12.5 break search/browse ECP?": https://community.roku.com/discussions/developer/did-12-5-break-searchbrowse-ecp/912374
