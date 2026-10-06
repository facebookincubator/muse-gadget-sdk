---
name: gadget-espresso-bar
description: >-
  Drive a home espresso bar over the LAN through Muse Home Link: a
  grind-by-weight grinder and a brew-by-weight shot controller, both running
  open source ESP32 firmware with a small unauthenticated HTTP API. Dose the
  grinder, set shot yield targets, and read live weight and last-shot
  summaries. Use when the user asks to grind, dose, pull a shot, or check the
  state of their espresso setup.
---

# Espresso Bar (grind-by-weight grinder + shot controller)

Use this skill when `/status` on the LAN confirms one of the firmware ids below, not merely an ESP32 that answers on port 80.

## Identify the Devices

| Board | Firmware | mDNS | `/status` firmware id |
|---|---|---|---|
| Grinder (ESP32-S3, HX711 load cell, motor relay) | [MuseGrinder](https://github.com/avidan/MuseGrinder) `muse-gadget-wifi` branch | `musegrinder.local` | `musegrinder-muse/1.0` |
| Shot controller (ESP32, BLE link to an Acaia / Bookoo / Felicita scale) | [MuseCoffeeScale](https://github.com/avidan/MuseCoffeeScale) `examples/shotStopper` with `muse_wifi.ino` | `shotstopper-grinder.local` | `shotstopper-muse/1.0` |

- `GET http://<host>/status` must return the expected `firmware` id before any write. Either board may be absent; drive only the ones that answer.
- If mDNS does not resolve, use the address from the router's DHCP list; both boards print their IP on serial at boot.
- A board that never joined WiFi exposes a setup AP (`MuseGrinder-Setup` / `shotStopper-Setup`). Provisioning is the user's job, not this skill's.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Boards on the same LAN as the Home Link.
- Shot controller paired to its scale (`scale_connected: true`); grinder load cell calibrated.
- The HTTP APIs have **no authentication**. Treat them as LAN-only and never expose or forward them.

## API Reference

### Grinder — `http://musegrinder.local`

- `GET /status` → `{"grinding":bool,"weight_g":float,"target_g":float,"mode":"weight","last_result":"success|overshoot|max_pulses|timeout|error|unknown","simulated":bool,"firmware":"musegrinder-muse/1.0"}`
- `POST /target` body `{"g": 18.5}` → starts a weight-mode grind, 1–100 g.
  `200 {"target_g":18.5,"started":true}` · `400` missing body or out of range · `409` grind already active · `503` grinder refused (load cell fault or not calibrated).
- `POST /stop` → `200 {"stopped":true}` · `409` no grind active.
- `GET /last` → `{"valid":bool,"final_weight_g":float,"target_g":float,"result":"..."}`

When `simulated` is true the board is in simulation mode: the motor never runs and weights are synthetic. Say so in every report.

### Shot controller — `http://shotstopper-grinder.local`

- `GET /status` → `{"scale_connected":bool,"brewing":bool,"weight_g":float,"goal_g":int,"shot_timer_s":float,"firmware":"shotstopper-muse/1.0"}`
- `POST /target` body `{"g": 36}` → sets the yield goal, 10–200 g. `200 {"goal_g":36}` · `400` missing body or out of range.
  The firmware stores whole grams and **truncates** (36.9 → 36), so round before sending. The goal persists in EEPROM and is mirrored to the board's BLE characteristic. It does not start a shot.
- `GET /last` → `{"valid":bool,"final_weight_g":float,"goal_g":int,"duration_s":float,"end":"weight|time|button|disconnect|unknown|none"}`
  `valid:false` with `end:"none"` means no shot has finished since boot.

## Workflow

### Dose the grinder

1. `GET` grinder `/status`. Proceed only if `grinding` is false.
2. `POST /target` with the requested dose.
3. Poll `GET /status` about every 2 s until `grinding` is false. A grind normally takes 5–15 s; stop polling after 60 s and report a timeout.
4. `GET /last` and report `final_weight_g` against the target and the `result`.

### Pull a shot at a ratio

1. Get the dose from grinder `GET /last` (`final_weight_g`) or ask the user.
2. Yield = dose × ratio, rounded to whole grams (18.5 g at 1:2 → 37 g).
3. Check shot controller `/status`: `scale_connected` true and `brewing` false.
4. `POST` the yield to the shot controller `/target` and confirm `goal_g` in the response.
5. Tell the user it is armed: once they start the shot on the machine, the board cuts the pump at the goal (and, in the default build, tares and starts the scale timer). Muse cannot start the pump.
6. Afterwards, `GET /last` and report yield, duration and `end` reason.

### Check the bar

`GET /status` on both boards. Report grinding and brewing state, live weights, targets, scale link, simulation mode, and each board's last result.

## Verify the Result

- `200 {"started":true}` or `200 {"goal_g":…}` only means the command was accepted. Report the outcome from `/last`, not from the write response.
- `end` other than `weight` means the shot did not stop on weight: `time` hit the board's max shot duration (50 s by default), `button` was stopped by hand, `disconnect` lost the scale mid-shot.
- If the grinder reports `grinding: true` for more than 60 s, or `last_result` is `timeout` or `error`, report it and stop. Do not grind again until the user has checked the machine.

## Safety

- A grind spins a real motor and a yield goal arms a real pump cutoff. Actuate only on the user's explicit request; a board being reachable does not authorize it.
- Never change a target while `grinding` or `brewing` is true. The shot controller does not reject mid-shot writes, so check first.
- Retry a failed grind at most once, and only after the user has checked the hopper, chute and scale.

## Limits

- LAN only, no authentication, no TLS.
- Shot-controller goal: whole grams, 10–200 g. Grinder target: 1–100 g, one decimal.
- No pump start, no tare or timer control, no WiFi reprovisioning, no OTA or reflashing through this skill.
- Experimental hobby firmware: confirm behavior on the bench before using it in a routine.

## Sources

- [MuseGrinder firmware](https://github.com/avidan/MuseGrinder) (`src/muse/muse_wifi.cpp`)
- [MuseCoffeeScale shot-controller firmware](https://github.com/avidan/MuseCoffeeScale) (`examples/shotStopper/muse_wifi.ino`)
- [Upstream AcaiaArduinoBLE / shotStopper](https://github.com/tatemazer/AcaiaArduinoBLE)
