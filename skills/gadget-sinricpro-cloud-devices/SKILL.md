---
name: gadget-sinricpro-cloud-devices
description: >-
  Cloud only: needs internet access and a SinricPro account API key; does not use Muse Home
  Link or the local network. Read status and control the user's SinricPro devices through
  the SinricPro cloud REST API: homes and rooms, device state, actions with confirmation over
  the event stream, scenes, schedules and a read-only automation list. Device actions,
  scene runs and scene or schedule edits need the user's confirmation. Prefer a
  device-specific or local skill when one matches.
---

# SinricPro Cloud Devices: REST API Control

Use this skill when a SinricPro API key is available and the user asks to control or query devices from their SinricPro account. SinricPro devices are typically ESP32, ESP8266, Raspberry Pi Pico or Linux boards running the SinricPro SDK; they are reached through the cloud, so no local address or firmware-specific protocol is needed. For a device that also exposes a local API, prefer its own skill, such as [ESPHome devices](../gadget-esphome-devices/SKILL.md).

## Identify the Device

- List devices with `GET /devices`. Each entry in `devices[]` carries `id`, `name`, `isOnline`, `room.name`, `room.home.name`, `product.code` (the type, such as `sinric.devices.types.LIGHT`) and `product.actions[]` (the actions the device accepts). Current state is in fields on the same object: `powerState`, `brightness`, `color`, `colorTemperature`, `powerLevel`, `temperature`, `humidity`, `targetTemperature`, `thermostatMode`, `rangeValue`, `volume`, `muted`, `lockState`, `garageDoorState`, `contactState` and `lastMotionState`, present only where the device has them. `GET /homes?includeRooms=true` and `GET /rooms` list homes and rooms.
- Match the user's words against `name`, room and type. With multiple matches, list the candidates with their rooms and ask.
- `isOnline: false` means report the device as offline and command nothing. `deactivated: true` means the device has no active license and cannot be controlled.

## Prerequisites

Read and follow the safety rules in `~/docs/devices/home_link.md`; its local networking rules do not apply, because this skill reaches the devices through the SinricPro cloud.

- **Cloud only.** This skill needs a SinricPro account with the devices already added, and internet access from Muse to `https://api.sinric.pro` and `https://sse.sinric.pro`. Use the normal internet path, never the Home Link proxy; a Muse Home Link is not required. If the cloud is unreachable, report that rather than trying a local path.
- A SinricPro API key, created by the account owner in the [SinricPro portal](https://portal.sinric.pro/credential/new/apikey) and supplied through approved secret storage. It grants full access to the account and has no scopes. Send it only in the `X-SINRIC-API-KEY` header through a credential-aware client, never in a credential-bearing shell command, and never log or echo it. If no key is available, explain this setup instead of asking for the key in chat.
- Send `X-SINRIC-API-KEY: <key>` on every call to `https://api.sinric.pro/api/v1`. Send request bodies as JSON.
- Every response carries `success`. Treat `success: false` as a failure even with HTTP 200; some update routes report validation errors that way. `401` means the key is wrong or was deleted: stop and ask the owner to supply a new key securely.
- Device reads are limited to 20 per minute per account, and sustained use at the limit triggers a 30-minute slow mode of 2 per minute. List devices once and reuse the result; on `429` wait the number of seconds in `resetTime` before any further read.

## Workflow

1. Identify the device as above, and choose an action from its own `product.actions[]`. Build its value:

   | Action | Value |
   |---|---|
   | `setPowerState` | `{"state":"On"}` / `{"state":"Off"}` |
   | `setBrightness` / `adjustBrightness` | `{"brightness":50}` / `{"brightnessDelta":-25}` |
   | `setPowerLevel` / `adjustPowerLevel` | `{"powerLevel":50}` / `{"powerLevelDelta":-25}` |
   | `setColor` | `{"color":{"r":255,"g":0,"b":0}}` |
   | `setColorTemperature` | `{"colorTemperature":2700}` |
   | `targetTemperature` | `{"temperature":22}` |
   | `setThermostatMode` | `{"thermostatMode":"COOL"}` — `COOL`, `HEAT`, `AUTO`, `OFF`, `ECO` |
   | `setRangeValue` / `adjustRangeValue` | `{"rangeValue":3}` / `{"rangeValueDelta":-1}` — fan speed, blinds 0–100 |
   | `setVolume` / `setMute` | `{"volume":25}` / `{"mute":true}` |
   | `mediaControl` | `{"control":"Pause"}` — `Play`, `Pause`, `Stop`, `Next`, `Previous` |
   | `setMode` | `{"mode":"MOVIE"}`; on a garage door `{"mode":"Close"}` |
   | `setLockState` | `{"state":"lock"}` |

   For relative asks ("a bit brighter"), use the `adjust*` action with a delta when the device lists it. Custom devices with several range, mode or toggle capabilities also need the capability's `instanceId`, found under `product.deviceTemplate.capabilities`.
2. Open the event stream before sending: `GET https://sse.sinric.pro/sse/stream` with the same key header and `Accept: text/event-stream`. It is a long-lived outbound HTTP connection that delivers `data:` lines of JSON. Keep it running in the background, and send the action only once it has connected and stayed open.
3. Send `POST /devices/{id}/action` with `{"type":"request","action":"...","value":"<value as JSON text>","clientId":"muse","messageId":"<new UUID>","createdAt":<unix seconds>}`, adding `instanceId` when needed. `value` is the value object from the table encoded as a JSON string, as in the API guide: `"value":"{\"state\":\"On\"}"`. `createdAt` must be current; stale messages are discarded. A `200` with `success: true` means the command was queued, not that it took effect.
4. Wait for a stream event with `event: "deviceMessageArrived"` whose `message.payload.replyToken` equals the `messageId`. Its `payload.success` and `payload.value` are the device's own answer. Wait up to 8 seconds, or 20 seconds for locks, garage doors and blinds, then close the stream.

## Scenes, Schedules and Automations

- **Scenes**: `GET /scenes` lists scenes with their `deviceActions[]` (`device`, `action`, `actionValue`). Before running one, read its `deviceActions` (parsing any string `actionValue`) and refuse if it contains an action the Limits below forbid or a value that can't be parsed; then run it with `POST /scenes/test/{id}`. The response only confirms the actions were queued. Create with `POST /scenes` and `{"name","description","deviceActions":[{"device":"<id>","action":"...","actionValue":{...}}]}`, using the same actions and values as the workflow table; names are 3–50 characters and unique. `PUT /scenes` with `id` in the body replaces the whole scene. `DELETE /scenes/{id}` removes it. `actionValue` can come back as a JSON **string** that needs a second parse.
- **Schedules**: `GET /schedules` lists them. Create with `POST /schedules` and `{"name","enable":true,"scheduleType":"time","deviceId":"<id>","action":"...","actionValue":"...","weekdays":["monday",...],"hour":0-23,"minute":0-59}`. Schedule actions are labels, not device actions: `Turn On` and `Turn Off` (with `actionValue` `"On"` / `"Off"`), and `Set Brightness`, `Set PowerLevel`, `Set Blinds`, `Set Fan Speed` and `Set Temperature` (with a numeric string). Any other label is stored but never runs. Times are in the account's time zone. There is no enable or disable endpoint: `PUT /schedules` with `id` and every field replaces the schedule. Read it first with `GET /schedules/{id}` and rebuild the write body from it: the stored schedule names its device as `device` (an object with `id`, or the id itself), but the write needs `deviceId`, so map it; then send `name`, `description`, `enable`, `scheduleType`, `deviceId`, `action`, `actionValue`, `weekdays`, `hour` and `minute` with only the change applied. `DELETE /schedules/{id}` removes it.
- **Automations**: `GET /automations` lists them. They are read-only here.

## Confirm Before Acting

- Before any device action, scene run, or scene or schedule create, update or delete, read back the device and its room (or the scene or schedule), the action and the value, and wait for the user's yes. A yes covers that one action only.
- Exceptions: brightness, color, volume and media control on a device the user named need only the user's own request, and reads need no confirmation.

## Verify the Result

- Report a device action as done only from the device's reply on the event stream. If none arrives in time, say the command was sent but not confirmed; the device may be offline or on weak Wi-Fi.
- A scene run queues device actions: report it as started, not as done, unless a later `GET /devices` shows the expected state.
- Scene and schedule create, update and delete change stored configuration and get no device reply. Confirm them from `success: true` and a fresh `GET /scenes` or `GET /schedules` showing the change.
- Do not resend an unconfirmed action, scene run or toggle until its outcome has been checked. A later `GET /devices` shows the last state the device reported.
- A stream that closes at once with `Invalid API Key` in its body means the key was rejected, even though the status is `200`.

## Limits

- Never unlock a lock or open a garage door through this skill. Locking and closing are allowed after confirming the intended device; for unlocking and opening, say so and point to the SinricPro app. Do not run, create or update a scene that contains such an action, and do not create or update a schedule for a lock or garage door, because schedule labels such as `Turn Off` do not say which way it would move.
- No camera live view, streaming or snapshots; never send `getWebRTCAnswer` or `getCameraStreamUrl`.
- No adding, removing or renaming devices, rooms or homes, and no creating or editing automations.
- Never read `/devices/{id}/credential`, and never repeat a device's `unlockPin` or `accessKey` fields to anyone.
- Only the user's own account devices behind their key. Send batch commands one at a time.

## Sources

- [sinricpro-agent-skills](https://github.com/sinricpro/sinricpro-agent-skills) — upstream skill and reference CLI this was adapted from.
- [SinricPro API guide](https://help.sinric.pro/pages/api-guide.html)
- [SinricPro API reference](https://apidocs.sinric.pro/)
