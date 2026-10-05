---
name: gadget-tuya-cloud-devices
description: >-
  Read status and control the user's Tuya app devices through Tuya's cloud end-user API with a
  user-supplied API key: homes and rooms, device detail, thing-model property control, rename,
  weather, hourly statistics, self-send notifications, IPC cloud snapshots and real-time event
  subscription. Cloud only; prefer a device-specific or local-TCP skill when one matches.
---

# Tuya Cloud Devices: End-User API Control

Use this skill when the user supplies a Tuya end-user API key (`sk-...`) and asks to control or
query devices from their Tuya app account. Devices are reached in the cloud, so no local keys or
model-specific datapoint maps are needed. For direct local-TCP control with a confirmed key and
datapoint schema, prefer [Tuya Wi-Fi devices](../gadget-tuya-wifi-devices/SKILL.md).

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
  back off and honor `Retry-After`. List endpoints return everything in one response; there is
  no pagination.

## Workflow

1. List homes with `GET /v1.0/end-user/homes/all`, and rooms with
   `GET /v1.0/end-user/homes/{home_id}/rooms` when the user names one.
2. Locate the device: `GET /v1.0/end-user/devices/all`, or scope to
   `.../homes/{home_id}/devices` or `.../homes/room/{room_id}/devices`. Each entry in
   `result.devices[]` carries `device_id`, `name`, `category`, `category_name`, `online` and,
   under a home scope, `room_id`. Match the user's words against `category_name` first, then
   fuzzy-match `name`. With multiple matches, list the candidates with their rooms and ask.
3. Read `GET /v1.0/end-user/devices/{device_id}/detail`. A `null` `result` means the device does
   not exist or the key has no access — stop. `online: false` means report the device as offline
   and command nothing. `result.properties` maps each property code to its current value.
4. Read the thing model `GET /v1.0/end-user/devices/{device_id}/model`. Its `result.model` field
   is a JSON **string** that needs a second parse. `services[].properties[]` then defines each
   property's `code`, `accessMode` (`ro` properties are read-only — say so) and `typeSpec`:
   - `value`: `min`, `max`, `step` and `scale` — the real-world value is the raw number divided
     by 10^scale.
   - `enum`: the issued value must be one of `range`.
   - `bool` / `string`: `true`/`false`, and `maxlen` respectively.
   Common codes are `switch_led`/`switch` (bool power), `bright_value`/`temp_value` (0–1000),
   `temp_set` (16–30) and `mode` (enum) — the model response is always authoritative.
5. Issue `POST /v1.0/end-user/devices/{device_id}/shadow/properties/issue` with a body whose
   `properties` value is a JSON **string**, not an object — it is double-serialized:
   `{"properties": "{\"switch_led\":true}"}`. An empty `result` object means the command was
   accepted, not that it took effect. For relative asks ("a bit brighter"), adjust the current
   value by 10% of the typeSpec range for vague amounts or by the stated amount exactly, clamp
   to `[min, max]`, honor `scale` and round to `step`.
6. Wait 1–2 seconds, re-read the detail and compare the mapped value before reporting success.

## Also Available

- **Rename**: `POST /v1.0/end-user/devices/{device_id}/attribute` with `{"name": "..."}`, max 50
  characters.
- **Weather**: `GET /v1.0/end-user/services/weather/recent` with `lat`, `lon` and `codes` — a
  URL-encoded JSON array of attribute codes such as `w.temp`, `w.humidity`, `w.condition`,
  `w.conditionNum`, `w.pressure`, `w.realFeel`, `w.uvi`, `w.windDir`, `w.windLevel`,
  `w.windSpeed`, plus `w.hour.N` to set the forecast horizon to the next N hours. Response keys
  are `{code}.{index}` with index `0` meaning now. Home coordinates are shaped
  `{"Value": "30.3"}` — read the inner `Value`; ask the user for their city if the home has
  none set.
- **Hourly statistics**: confirm capability with `GET /v1.0/end-user/statistics/hour/config`
  (items carry `dev_id`, `dp_code`, `statistic_type` of `SUM`/`COUNT`/`MAX`/`MIN`), then
  `GET .../hour/data?dev_id=..&dp_code=..&statistic_type=..&start_time=..&end_time=..`. Times
  are `yyyyMMddHH` and one request spans at most 24 hours; page longer ranges and aggregate.
  Values return as an array of `{time: value}` pairs with string values.
- **Notifications** are self-send only — they reach the key's own user, nobody else. SMS and
  voice take `{"message": "..."}`; mail and push take `{"subject": "...", "content": "..."}`.
  Rate and duplicate limits apply (per contact: 15–30 per day, identical content more than
  twice within 50 seconds is refused).
- **IPC cloud snapshots** (cameras): `POST /v1.0/end-user/ipc/{device_id}/capture/allocate`
  with body `{"capture_json": "<json string>"}` — double-serialized like property issue. The
  string carries `device_id`, `capture_type` (`PIC` or `VIDEO`), optionally `pic_count`
  (clamped to 1–5), `video_duration_seconds` (1–60, default 10) and `home_id`. Allocate only
  reserves an upload slot: `result.status` is `ACCEPTED` or `REJECTED` and `result` carries
  `bucket` and the object key(s) (`image_object_key`, and for VIDEO also `video_object_key` /
  `cover_image_object_key`) — never a media URL; the device still has to capture and upload.
  Then poll `POST .../capture/resolve` with a `resolve_json` string echoing `device_id`,
  `capture_type`, `bucket` and the object key(s) from allocate, and pass
  `user_privacy_consent_accepted: true`. Resolve returns the link to hand the user: the
  image/video cloud storage URL in `decrypt_image_url` / `decrypt_video_url`. `status:
  NOT_READY` means keep polling — for PIC sleep 2 s, then resolve every 2 s up to 30 s, then
  retry up to 3 times 3 s apart; for VIDEO sleep `max(5, video_duration_seconds_effective) + 2`
  s using the clamped duration from the allocate response, then resolve every 2 s up to 120 s,
  then retry up to 3 times 5 s apart. With `pic_count` above 1, allocate returns per-snapshot
  coordinates in `pic_slots[]` — resolve each slot's key separately. A video's cover image can
  lag the video itself — check `message_for_user`.

## Real-Time Subscription

Property changes and online/offline transitions are also pushed over WebSocket. This is a
long-lived process for an always-on host (a Linux gadget), not a one-shot query. Write a short
Python script using the `websockets` package:

- URI by key prefix (same mapping as the REST table): AZ `wss://wsmsgs.iot-wus.com`,
  EU `wss://wsmsgs.iot-eu.com`, IN `wss://wsmsgs.iot-ap.com`, UE `wss://wsmsgs.iot-eus.com`,
  WE `wss://wsmsgs.iot-weu.com`, SG `wss://wsmsgs.iot-sea.com`.
- Auth: an `Authorization: <key>` header on the handshake — the same key as REST but **without**
  the `Bearer` prefix.
- Frames are JSON with two top-level fields, `eventType` and `data`. A `devicePropertyChange`
  carries `data.devId` and `data.status[]`, each with `code`, `value` and `time` (ms); an
  `onlineStatusChange` carries `data.devId`, `data.status` (`"online"`/`"offline"`) and
  `data.time`.
- Codes match thing-model codes. Look their meanings up via the model endpoint instead of
  guessing from the code string.
- Reconnect with backoff on transient drops. Treat close codes 1002, 1003, 1008 and 1011 as
  fatal, and any frame containing `error`, a failing `errorCode`, or `"success": false` as a
  server error.
- For event-driven automation, on a matching trigger issue properties to the action device as
  in Workflow step 5, and confirm the trigger→action mapping with the user before running it.

## Limits

- Control only basic property types (bool, enum, integer, string). Never write `raw`, `bitmap`,
  `struct` or `array` properties, or anything absent from the thing model.
- No lock/unlock, live video streaming, firmware updates, pairing or device removal — say so and
  point to the Tuya app.
- Device listing takes one scope at a time (all, home or room). Rate-limit batch writes with a
  short delay between commands.
- Notifications triggered by device events must cool down at least 30 minutes — property events
  fire frequently.
- These are the user's own account devices behind an end-user key; never attempt another user's
  devices.

## Sources

- [tuya-openclaw-skills](https://github.com/tuya/tuya-openclaw-skills) — upstream skill this was adapted from.
- [Tuya developer docs](https://tuya.ai/developer/docs)
