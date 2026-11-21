---
name: gadget-sony-bravia-tvs
description: Control Sony Bravia TVs to read power and volume and playing content and to set power and volume and mute and switch inputs and launch apps through Muse Home Link when the TV is on a trusted local network with IP Control enabled and a Pre-Shared Key available in user-only approved storage and do not use this skill for Cast media or firmware updates or service menu settings or control over the open internet
---

# Sony Bravia TVs

Use this skill when you need to control a Sony Bravia TV on the local network through its REST API, including reading power, volume, and playing content, and when authorised, setting power, volume, and mute, switching inputs, and launching apps.

## Identify the Device

Start every session with `getSystemInformation` on the `system` service. It returns the REST generation and model for that specific TV. Keep the serial number and MAC address it also returns private.

Method availability varies by generation. Never infer capability from the Sony or Bravia name or from a "smart TV" label. Resolve what this TV supports from its actual responses, not from memory or from another model's behaviour.

If `getSystemInformation` does not return a Bravia REST response, stop and confirm you are talking to the right device before continuing.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The TV is reachable on a trusted LAN or through a Home Link tunnel at `http://<tv-ip>/sony/<service>` on port 80. Use the placeholder `<tv-ip>` in examples and never record a real address in this skill.
- IP Control is enabled in the TV's settings.
- A Pre-Shared Key (PSK) is set in the TV's IP Control settings. The PSK must be one the user set on their own TV — never use a key from another person's TV, and never share or publish your own. The PSK lives in user-only approved storage, is never pasted into chat, never put in a URL, and never logged. Send it only as the `X-Auth-PSK` header on every request.
- Plain HTTP is not encryption. Use this API only on a trusted LAN or through a Home Link tunnel, never over the open internet.

## Workflow

1. Read state first. Start with `getSystemInformation` on the `system` service at `http://<tv-ip>/sony/system` with no trailing slash after the service name, as a trailing slash is invalid. Send requests as JSON-RPC-style POST bodies containing `method`, `params`, `version`, and an `id` between 1 and 2147483647, with the `X-Auth-PSK` header. Services include `system`, `audio`, `avContent`, `appControl`, `guide`, and `videoScreen`.
2. Resolve method availability from actual responses. Use the generation and model returned in step 1 and the success or error responses from this TV to decide which methods are available in this session. Do not assume a method exists because it worked on another Bravia.
3. Read the relevant current state before any write. Use reads such as `getPowerStatus`, `getSystemInformation`, `getVolumeInformation`, and `getPlayingContentInfo` as appropriate for the request. A state question is never permission to change anything, so answer read-only questions with reads only.
4. Handle a TV that does not answer without hammering it. When the TV is suspended, its HTTP server stops and reads fail: report it as off or unreachable and stop. Never wake it to answer a question. With Remote start (Control remotely) enabled, many models still answer in standby, so `setPowerStatus` can turn them on when the user asked for that. Do not repeatedly poll or hammer a TV that does not answer.
5. Check whether the desired value is already set. An already-set value counts as success, so make no write in that case and report the current state.
6. Ask for explicit confirmation before powering the TV off if someone may be watching. Do not proceed with `setPowerStatus` to off until that confirmation is given.
7. Act only within the confirmed scope. Writes include `setPowerStatus`, `setAudioVolume`, `setAudioMute`, and `setPlayContent` for input switching, with an input URI taken from this TV's `getCurrentExternalInputsStatus` response. For app launch, use `setActiveApp` on `appControl` with an app URI taken from this TV's `getApplicationList` response in this session, never from memory.
8. Space commands out. Rapid bursts can push the TV's API into a reloading state that returns errors for minutes, and the only cure is waiting. Never resend a command automatically. If errors indicate a reloading state, stop, wait, and report what happened.

## Verify the Result

Verify writes by reading the same property back with the corresponding read method. For example, confirm power with `getPowerStatus`, volume or mute with `getVolumeInformation`, and the input with `getPlayingContentInfo`. `getPlayingContentInfo` covers inputs and broadcast, not apps: while an app is in front it can return an error instead. For an app launch, report that `setActiveApp` was accepted and that the app itself could not be read back.

Report the value returned by the read-back, not just the value sent. If the read-back does not match the intended value, say so plainly, do not resend the command automatically, and wait before any further attempt the user authorises. If the read-back itself fails or returns nothing, report the change as unconfirmed rather than assuming it took effect.

For read-only requests, verification is the response itself from this TV in this session.

## Limits

- Method availability varies by REST generation and model. A method that is unavailable on this TV is out of scope for this session, even if it exists on other Bravia models.
- When the TV is suspended, the HTTP server stops and the REST API is unavailable. This skill has no UDP wake (Wake-on-LAN).
- Rapid command bursts can trigger a reloading state that returns errors for minutes. There is no reset for that state in this skill, only waiting.
- Cast media is out of scope. When a Cast receiver is advertised, route media playback to the shared Google Cast skill instead.
- Firmware updates, service-menu settings, and developer settings are out of scope.
- Control over the open internet is out of scope. Plain HTTP offers no encryption, so use is limited to a trusted LAN or a Home Link tunnel.
- Android TV Remote v2 is out of scope for this skill.

## Sources

- [Sony BRAVIA REST API structure](https://pro-bravia.sony.net/remote-display-control/rest-api/structure/) — services, URL form, JSON-RPC format and Pre-Shared Key authentication.
- [Sony BRAVIA REST API configuration and usage](https://pro-bravia.sony.net/remote-display-control/rest-api/guide/) — IP Control settings, the Pre-Shared Key and suspend behaviour.
