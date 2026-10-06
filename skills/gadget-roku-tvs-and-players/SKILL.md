---
name: gadget-roku-tvs-and-players
description: Use when a confirmed Roku TV, streaming stick/box or Roku-powered display answers the local External Control Protocol (ECP) on TCP 8060. Covers discovery, device info, installed apps, remote keys, text entry, app launch and deep links, search, and Roku TV volume, input and power.
---

# Roku TVs and Players (ECP)

Local HTTP control over the LAN using Roku's External Control Protocol (ECP).
No cloud account, pairing or client key; access is governed by a network-control
setting on the device (see "Network control setting").

## Before you act

- Confirm the target is a Roku: `GET http://<ip>:8060/query/device-info` must return XML
  containing `<vendor-name>Roku` and a `<model-number>`.
- Read `ecp-setting-mode` first. On a Roku TV running OS 16.0.4 the default is `limited`,
  where only `/query/device-info` and `/query/active-app` answer; everything else returns
  **403** (`ECP command not allowed in Limited mode.`). If the mode is not `enabled` or
  `permissive`, tell the user the setting must be changed on the TV and stop. Do not retry
  or look for a workaround. A **401** on `device-info` means the setting is `Disabled`;
  same advice.
- Also read `model-name`, `software-version`, `power-mode` and `is-tv`. Players and TVs
  differ in which keys they accept, and behavior varies by OS version.
- ECP has no per-request authentication. Only talk to a device the user named or approved,
  stay on the local network, and never expose port 8060 to the internet.
- Get user confirmation before anything with side effects: launching apps, power keys,
  input changes, typing text, Channel Store pages. Reads are always fine.
- Follow the shared Muse local-network safety rules when scanning; prefer a user-supplied
  IP over sweeping a subnet.

## Discovery

- Known IP: the user can find it under Settings > Network > About. A DHCP reservation keeps it stable.
- SSDP: multicast `M-SEARCH` to `239.255.255.250:1900` with `ST: roku:ecp`. The `LOCATION`
  header is the base URL (`http://<ip>:8060/`).
- mDNS: Roku TVs with AirPlay advertise `_airplay._tcp`, `_hap._tcp`, `_spotify-connect._tcp`
  and `_display._tcp`; the TXT record carries `manufacturer=Roku` and the A record the IP.
- Fallback: probe TCP 8060 across the local /24 and confirm each hit with `/query/device-info`.
- SSDP and mDNS replies are unicast from the Roku's port 1900 or 5353 to a random local port.
  A host with a default-deny inbound firewall (e.g. ufw) drops them as unsolicited even though
  the multicast query went out (seen as `[UFW BLOCK] ... SRC=<roku> SPT=1900`). Outbound
  TCP scanning of 8060 is unaffected, so use it when discovery returns nothing.

## Reads (GET, XML)

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

## Actions (POST, empty body)

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
- **Text entry:** one request per character, `/keypress/Lit_<char>`; URL-encode non-ASCII as
  UTF-8 (`Lit_%E2%82%AC`). Space is `Lit_%20`.
- **Deep links:** `contentID` (under 255 chars, URL-encoded) and `MediaType` (series, season,
  episode, movie, shortFormVideo, tvSpecial). Support is per app; an unsupported link just
  opens the app.
- **Search:** `keyword` is required; optional `title`, `type` (movie, tv-show, person, channel,
  game), `season`, `tmsid`, `match-any`, `provider`, `provider-id`, `show-unavailable`, `launch`.
  Reliability is limited (see Limits).
- `/install/<appId>` only opens the Channel Store page; the user confirms the install on screen.

## Recipes

- **What's playing?** `/query/active-app`, then `/query/media-player`.
- **Open an app by name:** `/query/apps`, match the name, `POST /launch/<id>`.
- **Navigate a menu:** one key per call. Keys sent about 170 ms apart worked reliably; re-query
  `active-app` when state matters.
- **Type a search string:** `Search` key, then `Lit_` per character, then `Select`.
- **Turn the TV on or off:** check `power-mode`, send the key, wait a few seconds, check again.

## Limits and gotchas

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
- **Search is best effort.** Roku's ECP reference says the `search` command was removed in
  OS 12.0, and a Roku community moderator said the firmware search behind it was replaced
  (reported for OS 12.5). On the OS 16.0.4 TV `/search/browse` still returned 200 and worked
  for some keywords, but one title that the on-screen search finds ("The Mentalist") never
  appeared through ECP in ten variants (plain keyword, `type=tv-show`, `show-unavailable`,
  `match-any`, "the " prefix, full title, `title=`, `launch`); only the shorter keyword "mental"
  gave a partial result. The cause is unknown. A 200 means the request was accepted, not that
  results appeared. Fall back to the `Search` key plus `Lit_` typing, or launch the app, and
  tell the user what to expect on screen.
- LAN only, plain HTTP, no CORS headers (a web page can fire keys with `no-cors` but cannot
  read responses). `ecp-tls-enabled` was `true` on the tested TV; plain HTTP on 8060 still worked.
- Developer-only endpoints (`chanperf`, `sgnodes`, `registry`, `exit-app`, `fwbeacons`, ...)
  need developer mode and are out of scope.
- No screenshots, no account sign-in, no sideloading, and no control of DRM playback beyond the
  normal remote keys. Do not change device settings other than through the normal remote keys
  the user asked for.
- Roku apps themselves cannot send ECP; this skill controls the device from outside.

## Sources

- Roku ECP reference: https://developer.roku.com/docs/developer-program/dev-tools/external-control-api.md
- Archived SDK ECP doc (apps, active-app, install, search/browse): https://sdkdocs-archive.roku.com/External-Control-API_1611563.html
- Roku Community, "Did 12.5 break search/browse ECP?": https://community.roku.com/discussions/developer/did-12-5-break-searchbrowse-ecp/912374
- Practical curl guide: https://dev.to/hisuperdev/every-roku-tv-ships-with-a-documented-rest-api-heres-how-to-use-it-from-curl-or-js-1jb1
