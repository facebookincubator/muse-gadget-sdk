---
name: gadget-weatherxm-wg1200
description: >-
  Read live outdoor LoRa station telemetry and indoor barometric pressure from a paired WeatherXM
  WG1200 console (`sensors.read`), cross-check sky conditions with an approved LAN camera over
  Muse Home Link, and render 480x480 baseline JPEG cards (`display.draw_file`) or alert overlays
  (`wg1200.notify`).
---

# WeatherXM WG1200: Local Weather Station, Sky Checks, and 480x480 Display

Use this skill when the user's paired Muse Gadget is a WeatherXM WG1200 (`muse-gadget-wxm-wg1200`) with `sensors.read`, `display.draw_file`, and `wg1200.notify`.

## Identify the Device

- Confirm `link.register` / device capabilities list `muse-gadget-wxm-wg1200` (`WeatherXM WG1200`) with `sensors.read`, `display.draw_file`, `wg1200.notify`, and `device.boot_weatherxm`.
- Note that the WG1200 has a `480x480` touchscreen, a top button, and a passive alert buzzer, **but no microphone**. Users interact from the Muse app or from one-tap actions on the WG1200 touchscreen.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md` whenever reaching another device on the WG1200's LAN.

- The WG1200 is online over Home Link (`sensors.read` and `display.draw_file` available). Onboard LoRa (`868`/`915` MHz) receives outdoor packets automatically from a nearby WeatherXM WS1001/WS1300 outdoor sensor array when present.
- If combining weather data with a LAN sky camera (for example via [`gadget-thingino-cameras`](../gadget-thingino-cameras/SKILL.md)), the user must approve that camera's IPv4 once in the Muse app. Because WG1200 has no on-device Home Link approval dialog, tell the user via chat (or a silent `wg1200.notify` card) if a touchscreen-triggered check is waiting on first-time camera approval in the Muse app.

## Workflow

1. **Read local microclimate (`sensors.read`).** Each metric object reports `{ "value": <number>, "unit": "<unit>", "age_s": <seconds> }`:
   - **Outdoor (`SX1262` LoRa from `WS1001`/`WS1300`):** `outdoor_temperature` (`C`), `outdoor_humidity` (`%`), `wind_speed` (`m/s`), `wind_gust` (`m/s`), `wind_direction` (`deg`), `rain_rate` (`mm/h`), `rain_daily` (`mm`), `rain_accumulated` (`mm`), `uv_index` (`UVI`), `illuminance` (`lx`), `solar_irradiance` (`W/m2`), `station_battery` (`mV`), `lora_rssi` (`dBm`), and `lora_snr` (`dB`), plus `lora_radio` (`sx1262_detected`, `region`, `freq_hz`, `rx_packets_total`, `rx_packets_valid`, `station_model`, `station_id`).
   - **Indoor (`BMP390`):** `barometric_pressure` (`hPa`) and `gateway_temperature` (`C`, console internal temperature; use `outdoor_temperature` for ambient air).
   - Report active rain only when `rain_rate.value > 0`; `rain_daily.value` is cumulative since reset/day boundary and stays positive after rain ends.
2. **Optional LAN sky-camera verification.** When asked to inspect the sky or combine a camera view with station telemetry, fetch a snapshot from the user's approved LAN camera over the Home Link tunnel directly into the VM (preferring sub-stream JPEGs such as Thingino `/x/ch1.jpg?token=...` for speed, or `-rtsp_transport tcp` single-frame capture).
3. **Render onto the `480x480` screen (`display.draw_file`).**
   - Always output a **`480x480` baseline (non-progressive) JPEG** on the VM (`Pillow` with `progressive=False` or `ffmpeg`/`jpegtran` without `-progressive`) before calling `display.draw_file`.
   - Avoid progressive JPEGs (`JDR_FMT3` error on ESP32 ROM `tjpgd`) and `16:9` frames (`1920x1080` scales by `1/4` to a letterboxed `480x270` strip). Center-crop or compose a `480x480` square card on the VM first.
4. **Short alerts (`wg1200.notify`).** Use `wg1200.notify` (`message`, optional `title`, `beep_ms` `0-1000`, `duration_s`) for concise text alerts or audible weather warnings when requested. Use `beep_ms: 0` unless an audible chime is appropriate.

## Verify the Result

- Check `outdoor_temperature.age_s` and `lora_radio.rx_packets_valid` before stating current outdoor conditions; if outdoor metrics are absent (no LoRa packet received yet) or `age_s` is stale (`> 300` s), say the outdoor LoRa array is not currently reporting rather than substituting an internet forecast silently.
- Confirm `display.draw_file` returns `ok: true`.

## Limits

- Read-only weather telemetry; only call `device.boot_weatherxm` when the user explicitly asks to switch the console back to WeatherXM OS.

## Sources

- [WeatherXM Weather Stations](https://weatherxm.com/stations/)
- [Muse × WeatherXM WG1200 Dual-Boot Web Flasher](https://muse.weatherxm.com/) ([WeatherXM/muse-gadget-sdk](https://github.com/WeatherXM/muse-gadget-sdk))
- [WeatherXM WG1200 setup guide](../../esp32/devices/wxm-wg1200.md)
