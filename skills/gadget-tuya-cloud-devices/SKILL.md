---
name: gadget-tuya-cloud-devices
description: >-
  Read status and control the user's Tuya app devices through Tuya's cloud end-user API with a
  user-supplied API key: homes and rooms, device detail, thing-model property control,
  rename, weather, hourly statistics, self-send notifications and IPC cloud snapshots. Cloud
  only; prefer a device-specific or local-TCP skill when one matches.
---

# Tuya Cloud Devices: End-User API Control

Use this skill when the user supplies a Tuya end-user API key (`sk-...`) and asks
to control or query devices from their Tuya app account. Devices are reached in the cloud, so no
local keys or model-specific datapoint maps are needed. For direct local-TCP control with a
confirmed key and datapoint schema, prefer [Tuya Wi-Fi devices](../gadget-tuya-wifi-devices/SKILL.md).

## Prerequisites

Follow the shared HomeLink networking and safety rules in `home_link.md`.

- A user-supplied end-user API key, applied for at https://tuya.ai/. Treat it as a credential:
  pass it in a header, never log or echo it.
- Send `Authorization: Bearer <key>` on every call. Pick the base URL from the first two
  characters after `sk-`; it must match the account's region. If the prefix is not listed, ask
  the user to confirm the key was issued for an international account at https://tuya.ai/.

  | Prefix | Base URL |
  |---|---|
  | AZ | https://openapi.tuyaus.com |
  | EU | https://openapi.tuyaeu.com |
  | IN | https://openapi.tuyain.com |
  | UE | https://openapi-ueaz.tuyaus.com |
  | WE | https://openapi-weaz.tuyaeu.com |
  | SG | https://openapi-sg.iotbing.com |

- Responses share one envelope: `{"success": true, "result": ...}` or
  `{"success": false, "code", "msg"}`. On `1010` the key expired — ask for a fresh one. On `429`
  back off and honor `Retry-After`.

## Workflow

1. List homes with `GET /v1.0/end-user/homes/all`, and rooms with
   `GET /v1.0/end-user/homes/{home_id}/rooms` when the user names one.
2. Locate the device: `GET /v1.0/end-user/devices/all`, or scope to
   `.../homes/{home_id}/devices` or `.../homes/room/{room_id}/devices`. Match the user's words
   against `category_name` first, then fuzzy-match `name`. With multiple matches, list the
   candidates with their rooms and ask.
3. Read `GET /v1.0/end-user/devices/{device_id}/detail`. A `null` `result` means the device does
   not exist or the key has no access — stop. `online: false` means report the device as offline
   and command nothing. `properties` holds the current values.
4. Read the thing model `GET /v1.0/end-user/devices/{device_id}/model`. Its `result.model` field
   is a JSON **string** that needs a second parse. Honor `accessMode` (`ro` properties are
   read-only — say so) and each property's `typeSpec` `min`, `max` and `step`.
5. Issue `POST /v1.0/end-user/devices/{device_id}/shadow/properties/issue` with a body whose
   `properties` value is a JSON **string**, not an object — it is double-serialized:
   `{"properties": "{\"switch_led\":true}"}`. For relative asks ("a bit brighter"), adjust the
   current value by 10% of the typeSpec range for vague amounts or by the stated amount exactly,
   clamp to `[min, max]` and round to `step`.
6. Wait 1–2 seconds, re-read the detail and compare the mapped value before reporting success.
   An accepted write is not proof of the physical result.

## Also Available

- Rename: `POST /v1.0/end-user/devices/{device_id}/attribute`.
- Weather: `GET /v1.0/end-user/services/weather/recent?codes=[...]&lat=..&lon=..`. Home
  coordinates are shaped `{"Value": "30.3"}` — read the inner `Value`. Ask for a city if the home
  has none set.
- Hourly statistics: confirm capability with `GET /v1.0/end-user/statistics/hour/config`, then
  `GET .../hour/data?dev_id=..&dp_code=..&statistic_type=SUM&start_time=..&end_time=..`. Times
  are `yyyyMMddHH` and one request spans at most 24 hours; page longer ranges and aggregate.
- Notifications are self-send only — they reach the key's own user, nobody else:
  `POST /v1.0/end-user/services/{sms|voice|mail|push}/self-send`.
- IPC cloud snapshots: `POST /v1.0/end-user/ipc/{device_id}/capture/allocate`, then
  `.../capture/resolve`; with consent, resolve returns a decrypted media URL. Poll for readiness
  rather than assuming the upload finished.

## Limits

- Control only basic property types (bool, enum, integer, string). Never write `raw`, `bitmap`,
  `struct` or `array` properties, or anything absent from the thing model.
- No lock/unlock, live video streaming, firmware updates, pairing or device removal — say so and
  point to the Tuya app.
- Device listing takes one scope at a time (all, home or room). Rate-limit batch writes with a
  short delay between commands.
- Automation triggered from device events must cool down at least 30 minutes before sending
  another notification.
- These are the user's own account devices behind an end-user key; never attempt another user's
  devices.

## Sources

- [tuya-openclaw-skills](https://github.com/tuya/tuya-openclaw-skills) — upstream skill this was adapted from.
- [Tuya developer docs](https://tuya.ai/developer/docs)
