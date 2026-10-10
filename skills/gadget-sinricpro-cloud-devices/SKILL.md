---
name: gadget-sinricpro-cloud-devices
description: >-
  Read status and control the user's SinricPro devices through the SinricPro cloud REST API
  with a user-supplied API key: homes and rooms, device state, actions with confirmation over
  the event stream, scenes, schedules and a read-only automation list. Cloud only; prefer a
  device-specific or local skill when one matches.
---

# SinricPro Cloud Devices: REST API Control

Use this skill when the user supplies a SinricPro API key and asks to control or query devices from their SinricPro account. SinricPro devices are typically ESP32, ESP8266, Raspberry Pi Pico or Linux boards running the SinricPro SDK; they are reached through the cloud, so no local address or firmware-specific protocol is needed. For a device that also exposes a local API, prefer its own skill, such as [ESPHome devices](../gadget-esphome-devices/SKILL.md).

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A user-supplied API key, created at https://portal.sinric.pro/credential/new/apikey. It grants full access to the account and has no scopes: pass it in a header, never log or echo it.
- Send `X-SINRIC-API-KEY: <key>` on every call to `https://api.sinric.pro/api/v1`. Send request bodies as JSON.
- Every response carries `success`. Treat `success: false` as a failure even with HTTP 200; some update routes report validation errors that way. `401` means the key is wrong or was deleted — ask for a new one.
- Device reads are limited to 20 per minute per account, and sustained use at the limit triggers a 30-minute slow mode of 2 per minute. List devices once and reuse the result; on `429` wait the number of seconds in `resetTime` before any further read.

## Workflow

1. List devices with `GET /devices`. Each entry in `devices[]` carries `id`, `name`, `isOnline`, `room.name`, `room.home.name`, `product.code` (the type, such as `sinric.devices.types.LIGHT`) and `product.actions[]` (the actions the device accepts). Current state is in fields on the same object: `powerState`, `brightness`, `color`, `colorTemperature`, `powerLevel`, `temperature`, `humidity`, `targetTemperature`, `thermostatMode`, `rangeValue`, `volume`, `muted`, `lockState`, `garageDoorState`, `contactState` and `lastMotionState`, present only where the device has them. `GET /homes?includeRooms=true` and `GET /rooms` list homes and rooms.
2. Match the user's words against `name`, room and type. With multiple matches, list the candidates with their rooms and ask. `isOnline: false` means report the device as offline and command nothing. `deactivated: true` means the device has no active license and cannot be controlled.
3. Choose an action from the device's own `product.actions[]` and build its value:

   | Action | Value |
   |---|---|
   | `setPowerState` | `{"state":"On"}` or `"Off"` |
   | `setBrightness` / `adjustBrightness` | `{"brightness":50}` / `{"brightnessDelta":-25}` |
   | `setPowerLevel` / `adjustPowerLevel` | `{"powerLevel":50}` / `{"powerLevelDelta":-25}` |
   | `setColor` | `{"color":{"r":255,"g":0,"b":0}}` |
   | `setColorTemperature` | `{"colorTemperature":2700}` |
   | `targetTemperature` | `{"temperature":22}` |
   | `setThermostatMode` | `{"thermostatMode":"COOL"}` — `COOL`, `HEAT`, `AUTO`, `OFF`, `ECO` |
   | `setRangeValue` / `adjustRangeValue` | `{"rangeValue":3}` / `{"rangeValueDelta":-1}` — fan speed, blinds 0–100 |
   | `setVolume` / `setMute` | `{"volume":25}` / `{"mute":true}` |
   | `mediaControl` | `{"control":"Pause"}` — `Play`, `Pause`, `Stop`, `Next`, `Previous` |
   | `setMode` | `{"mode":"MOVIE"}`; on a garage door `"Close"` |
   | `setLockState` | `{"state":"lock"}` |

   For relative asks ("a bit brighter"), use the `adjust*` action with a delta when the device lists it. Custom devices with several range, mode or toggle capabilities also need the capability's `instanceId`, found under `product.deviceTemplate.capabilities`.
4. Open the event stream before sending: `GET https://sse.sinric.pro/sse/stream` with the same key header and `Accept: text/event-stream`. It is a long-lived outbound HTTP connection that delivers `data:` lines of JSON.
5. Send `POST /devices/{id}/action` with `{"type":"request","action":"...","value":{...},"clientId":"muse","messageId":"<new UUID>","createdAt":<unix seconds>}`, adding `instanceId` when needed. `createdAt` must be current; stale messages are discarded. A `200` with `success: true` means the command was queued, not that it took effect.
6. Wait for a stream event with `event: "deviceMessageArrived"` whose `message.payload.replyToken` equals the `messageId`. Its `payload.success` and `payload.value` are the device's own answer. Wait up to 8 seconds, or 20 seconds for locks, garage doors and blinds, then close the stream.

## Scenes, Schedules and Automations

- **Scenes**: `GET /scenes` lists scenes with their `deviceActions[]` (`device`, `action`, `actionValue`). Run one with `POST /scenes/test/{id}`; the response only confirms the actions were queued. Create with `POST /scenes` and `{"name","description","deviceActions":[{"device":"<id>","action":"...","actionValue":{...}}]}`, using the same actions and values as step 3; names are 3–50 characters and unique. `PUT /scenes` with `id` in the body replaces the whole scene. `DELETE /scenes/{id}` removes it. `actionValue` can come back as a JSON **string** that needs a second parse.
- **Schedules**: `GET /schedules` lists them. Create with `POST /schedules` and `{"name","enable":true,"scheduleType":"time","deviceId":"<id>","action":"...","actionValue":"...","weekdays":["monday",...],"hour":0-23,"minute":0-59}`. Schedule actions are labels, not device actions: `Turn On` and `Turn Off` (with `actionValue` `"On"` / `"Off"`), and `Set Brightness`, `Set PowerLevel`, `Set Blinds`, `Set Fan Speed` and `Set Temperature` (with a numeric string). Any other label is stored but never runs. Times are in the account's time zone. There is no enable or disable endpoint: `PUT /schedules` with `id` and every field replaces the schedule, so read it first and resend it with `enable` changed. `DELETE /schedules/{id}` removes it.
- **Automations**: `GET /automations` lists them. They are read-only here.

Read planned scene and schedule changes back to the user, and confirm before any delete.

## Verify the Result

- Report success only from the device's reply on the event stream. If none arrives in time, say the command was sent but not confirmed; the device may be offline or on weak Wi-Fi.
- Do not resend an unconfirmed action, scene run or toggle until its outcome has been checked. A later `GET /devices` shows the last state the device reported.
- A stream that closes at once with `Invalid API Key` in its body means the key was rejected, even though the status is `200`.

## Limits

- Never unlock a lock or open a garage door through this skill. Locking and closing are allowed after confirming the intended device; for unlocking and opening, say so and point to the SinricPro app. Do not run or create a scene that contains such an action.
- No camera live view, streaming or snapshots; never send `getWebRTCAnswer` or `getCameraStreamUrl`.
- No adding, removing or renaming devices, rooms or homes, and no creating or editing automations.
- Never read `/devices/{id}/credential`, and never repeat a device's `unlockPin` or `accessKey` fields to anyone.
- Only the user's own account devices behind their key. Send batch commands one at a time.

## Sources

- [sinricpro-agent-skills](https://github.com/sinricpro/sinricpro-agent-skills) — upstream skill and reference CLI this was adapted from.
- [SinricPro API guide](https://help.sinric.pro/pages/api-guide.html)
- [SinricPro API reference](https://apidocs.sinric.pro/)
