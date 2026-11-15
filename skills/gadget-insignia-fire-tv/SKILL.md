---
name: gadget-insignia-fire-tv
description: >-
  Control compatible Insignia Fire TVs through their PIN-paired local HTTP/HTTPS remote
  API using Muse Home Link. Covers installed apps, app launch, remote keys, volume,
  mute, supported playback toggles and normal-standby wake. Not for Insignia Roku TVs.
---

# Insignia Fire TVs

Local remote control through Muse Home Link: HTTP on port 8009 activates the remote
service; HTTPS on port 8080 handles pairing and controls. This community-observed API
requires no ADB or developer mode.

## Identify the Device

- Confirm an Insignia TV running Fire TV, its current address and firmware. Tested on
  `INSIGNIA_NS-55F501NA26`, Fire OS `8.1.8.5` (`RS8185/3885`); compatibility can vary.
- Use the supplied IP or Home Link discovery. If a DIAL description URL is advertised,
  use that URL rather than guessing paths. If discovery fails, ask for the IP; do not
  scan the subnet. The model and OS version were supplied by the user, not the API.
- Port 8009 is HTTP/DIAL here; it does not establish Google Cast support.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A configured TV and HTTP/HTTPS client routed through Home Link to ports 8009 and 8080.
- A person able to read the normal on-screen pairing PIN when credentials are absent.
- Securely retained per-TV client token and device-specific TLS trust. Use the runtime's
  existing private credential facility; do not assume storage on Home Link hardware.
  If files are used, use directory mode 0700 and file mode 0600, outside source control.
  Keep tokens out of chat, logs and command arguments; discard temporary PIN payloads.
- Permission for the requested screen/audio action. Launching an app or pressing Home
  can interrupt playback. Service activation can wake the display, so do not activate
  automatically for a status-only request.

## TLS and Pairing

1. If activation is needed for requested pairing, wake or control, send bodyless
   `POST http://<tv-ip>:8009/apps/FireTVRemote`. The tested response was `201 Created`.
   Activation restored port 8080 and woke the display from normal standby.
2. Preserve existing per-TV trust and credentials. The tested TV's certificate chain
   failed normal CA verification. For first enrollment, use an independently verified
   public key if available, otherwise explain trust on first use on the user's trusted
   local network. Capture the leaf public key without sending credentials and retain
   it per TV. Seeing a PIN on screen is not independent certificate verification.
3. Enforce that key on subsequent HTTPS requests, including pairing. Curl supports
   `--insecure` together with `--pinnedpubkey <saved-public-key-file>` for this device;
   never fall back to `--insecure` alone. A wrong pin failed before HTTP with curl error
   90. Stop on mismatch; do not automatically replace the pin or re-pair. Keep requests
   on the authorized TV origin and do not follow redirects to other hosts.
4. Request a PIN with `POST https://<tv-ip>:8080/v1/FireTV/pin/display`, headers
   `X-Api-Key: 0987654321` and `Content-Type: application/json`, and body
   `{"friendlyName":"Muse"}`. Ask the user for the displayed PIN without echoing it.
5. Submit `{"pin":"<displayed-pin>"}` to `POST /v1/FireTV/pin/verify` with the same
   headers and TLS trust. A successful response contains the token in `description`.
   An incorrect PIN returned HTTP 200 with `{"description":""}`: HTTP success is not
   pairing success. Require a nonempty token other than `OK`, then validate it with
   an authenticated app-list read before replacing stored credentials.
6. Reuse the saved token via `X-Client-Token`, alongside the API-key header. Both the
   token and pinned key survived normal standby and a user-performed TV restart.
   If a PIN prompt closes or returns no token, retain existing credentials and use a
   fresh user-assisted prompt. One fresh prompt recovered successfully in testing.

## Request Shape

Except activation, use `https://<tv-ip>:8080` with `X-Api-Key: 0987654321`,
`X-Client-Token: <saved-token>` and `Content-Type: application/json`.
Send no request body for the controls below, rather than an empty JSON object.
A custom User-Agent was not needed for the tested YouTube launch.

| Operation | Method and path | Behavior |
| --- | --- | --- |
| Installed apps | `GET /v1/FireTV/appsV2` | Array with `name`, `appId`, `isInstalled` and optional shortcut/icon fields. |
| Remote key | `POST /v1/FireTV?action=<key>` | `home`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `select`, `back`, `menu`. Menu returned OK with no visible effect in YouTube's sidebar. |
| App launch | `POST /v1/FireTV/app/<appId>` | Use a returned installed-app ID; encode it as a path segment. |
| Volume step | `POST /v1/FireTV?action=volume_up` or `volume_down` | On-screen volume changes confirmed, including one-step decreases. |
| Mute toggle | `POST /v1/FireTV?action=mute` | Confirmed mute/unmute toggle. `volume_mute` was rejected with HTTP 400. |
| Playback toggle | `POST /v1/media?action=play` or `pause` | In YouTube, **both actions toggled** playing/paused, including resuming when repeated while paused. Neither is a discrete play or pause operation. |

Missing and invalid client tokens returned HTTP 403 with descriptions identifying
`missing client token` or `contains invalid client token`. The app-list read accepted
an omitted API-key header with a valid token, but retain the header for compatibility.

## Workflow

1. Select the intended TV, establish trusted HTTPS and reuse valid credentials or pair
   normally. Keep service availability, authentication and visible control separate.
2. For app launch, query the app list and resolve the user's requested app. Confirmed
   launches: YouTube (`com.amazon.firetv.youtube`), Netflix (`com.netflix.ninja`),
   HBO Max (`com.hbo.hbonow`), Apple TV (`com.apple.atve.amazon.appletv`) and Disney+
   (`com.disney.disneyplus`). Prime Video was omitted from the list but launching
   `com.amazon.firebat` worked on the tested configuration. `com.amazon.cloud9` is
   Silk on this TV, despite an incorrect Prime Video label in a community reference.
3. Send the requested operation and allow app loading before further navigation.
   Launch may show a profile chooser or resume a profile. Do not automatically press
   Select: confirm the chooser and intended highlighted profile when selection is
   requested. Back is context-dependent; it opened YouTube's sidebar rather than exiting.
4. For explicit mute/unmute or pause/resume requests, establish the current state first.
   Send nothing if already in the desired state. Do not blindly repeat a toggle or
   volume step after an uncertain response.
5. For connection refusal/timeout, activate the remote service once when appropriate,
   then try at most three authenticated reads within a total 30-second recovery window.
   Use finite request timeouts. Stop on TLS mismatch; diagnose HTTP 401/403 separately.
   If fresh user-assisted pairing also fails, stop rather than looping.

## Verify the Result

- HTTP 200 and `description: OK` do not prove the screen changed. A nonexistent app ID
  also returned OK but produced an App not found popup. Use actual app IDs and inspect
  available state or obtain visual confirmation before claiming success.
- No reliable foreground-app, playback or mute-state read-back is established here.
  DIAL `FireTVRemote` state reported `stopped` even while controls worked; it is not
  display-power or foreground-app state.
- Preserve the user's intended final screen. Do not send Home immediately after a
  launch, select content, start playback or switch profiles without the corresponding request.

## Limits

- Normal-standby wake worked through service activation; unplugged power, deeper sleep
  and other model/firmware combinations may differ. Direct restart/power-off is not
  established. Do not substitute an unobserved Settings key sequence.
- App launch does not establish subscription entitlement. Playback toggles are confirmed
  in YouTube; other apps can behave differently. Hold/repeat timing, seeking, text/search,
  HDMI inputs, numeric volume setting/read-back and voice are outside this procedure.
- No cloud fallback, ADB fallback, pairing bypass, screenshots, sideloading, account
  setup, purchases, firmware changes or assumed Cast support.

## Sources

- [FireTVRest](https://github.com/SLC-Josh/FireTVRest) — PIN pairing, keys, media and app launch; resolve IDs against the TV.
- [HMS FireTV](https://github.com/hms-homelab/hms-firetv) — app discovery and remote protocol.
- [HMS mute routing](https://github.com/hms-homelab/hms-firetv/blob/master/src/api/CommandController.cpp) — `mute` action.
- [python-androidtv constants](https://github.com/JeffLIrion/python-androidtv/blob/master/androidtv/constants.py) — Prime Video's `com.amazon.firebat` ID.
- [Node-RED Fire TV remote](https://flows.nodered.org/flow/6cc777ad9aa152691273fe2b976f4b29) — volume actions; its `volume_mute` candidate failed on this TV.
- [curl public-key pinning](https://curl.se/libcurl/c/CURLOPT_PINNEDPUBLICKEY.html) — key matching and trust behavior.
- [Amazon developer response](https://community.amazondeveloper.com/t/remote-control-firetv-device-from-android-app/5865) — no public remote-control API was offered as of July 2024.
