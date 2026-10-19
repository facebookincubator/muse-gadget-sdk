---
name: gadget-ecowitt-gateways
description: >-
  Read live weather from an Ecowitt GW1100, GW1200, GW2000 or GW3000 gateway through Muse Home
  Link: temperature, wind, rain, humidity, pressure and UV at home. Read-only local HTTP API.
---

# Ecowitt Gateways: Live Weather Readings

Use this skill when the user asks about current conditions at home and the address identifies an Ecowitt gateway. These are the user's own sensor readings, not a forecast.

## Identify the Device

- Gateways do not advertise mDNS, so discovery shows them as unnamed hosts at best. If discovery does not identify one, ask the user for its IP; the Ecowitt and WSView Plus apps show it.
- GET `/get_version` and require `"platform":"ecowitt"` and a supported model in `version`, such as `GW2000B_V3.3.2`. Re-check after any address change.
- GW1000 and older consoles have no local HTTP API.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The gateway is already set up in the Ecowitt or WSView Plus app with its outdoor sensor paired. Setup and sensor pairing are outside this skill.
- The read endpoints need no credentials. If one returns a login page or an error instead of JSON, stop and tell the user.

## Workflow

1. GET `/get_livedata_info` and pick the requested fields (see Request Shape).
2. Report values with the units the gateway returns; convert only when asked.
3. For wind direction, use the 10-minute average `0x6D` when present; the instantaneous `0x0A` swings between reads.
4. Say it is raining only when rain rate or event total is above zero. `srain_piezo` can read `1` briefly with zero rain from mist or dew; call that possible drizzle.

## Request Shape

`common_list`, `rain` and `piezoRain` are arrays of `{"id","val","unit"?}` entries; `wh25` is a single object with named fields. Groups appear only for sensors that are reporting. Excerpt from a GW2000 with a WS90:

```json
{"common_list":[{"id":"0x02","val":"58.8","unit":"F"},{"id":"0x0B","val":"2.68 mph"}],
 "piezoRain":[{"id":"srain_piezo","val":"0"},{"id":"0x0E","val":"0.00 in/Hr"}],
 "wh25":[{"intemp":"74.8","unit":"F","inhumi":"49%","abs":"29.96 inHg","rel":"30.01 inHg"}]}
```

| Group | Field | Meaning |
|---|---|---|
| `common_list` | `0x02`, `0x07`, `3`, `0x03` | Outdoor temperature, humidity, feels-like, dew point |
| `common_list` | `0x0B`, `0x0C` | Wind speed, current gust |
| `common_list` | `0x19` | Today's maximum gust, not the current gust |
| `common_list` | `0x0A`, `0x6D` | Wind direction (from), instantaneous and 10-minute average |
| `common_list` | `0x15`, `0x17` | Solar radiation, UV index |
| `rain`, `piezoRain` | `0x0E`, `0x0D` | Rain rate, current event total |
| `rain`, `piezoRain` | `0x10`, `0x11`, `0x12`, `0x13`, `0x7C` | Rain today, week, month, year, last 24 hours |
| `piezoRain` | `srain_piezo` | Rain detected, `0` or `1` |
| `wh25` | `intemp`, `inhumi` | Indoor temperature and humidity at the gateway |
| `wh25` | `abs`, `rel` | Station and sea-level pressure; measured indoors, but report it as local pressure |

Do not guess the meaning of ids not listed here.

## Verify the Result

- If a requested field is missing, say that sensor is not reporting.
- Battery status can sit on a rain entry, such as `0x13` on a WS90; mention it only when low or asked.
- If the gateway is unreachable, say so rather than falling back to an online forecast.

## Limits

- Read-only: no `/set_*` calls, calibration, upload or Wi-Fi settings, firmware updates, reboots, or switching IoT devices paired to the gateway.
- Do not read `/get_network_info` or `/get_device_info`; they expose the MAC address and access-point settings.
- No history beyond rain totals and today's maximum gust, which has no timestamp; do not say when it occurred.
- Wind readings reflect the sensor's mounting; one next to a wall can read sheltered or reversed wind.

## Sources

- [Ecowitt Official Integration for Home Assistant](https://github.com/Ecowitt/ha-ecowitt-iot): local HTTP API and supported models
- [wittiot](https://pypi.org/project/wittiot/), the library it uses: endpoint names and field ids
