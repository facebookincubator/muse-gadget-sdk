<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->
# WeatherXM WG1200

A WeatherXM D1 Wi-Fi Gateway (`WG1200`) running Muse Gadget SDK firmware in a
reversible dual-boot slot (`ota_1`) with a `480x480` touch display, indoor
barometer (`BMP390`), sub-GHz `SX1262` LoRa weather receiver, piezo buzzer, and
Home Link network tunnel.

## What it can do

- `sensors.read` returns live rooftop telemetry from the paired `WS1001` or
  `WS1300` LoRa weather station (`outdoor_temperature`, `outdoor_humidity`,
  `wind_speed`, `wind_gust`, `wind_direction`, `rain_rate`, `rain_daily`,
  `uv_index`, `illuminance`, `solar_irradiance`, `station_battery`) plus indoor
  `gateway_temperature` and `barometric_pressure`, each with `age_s`.
- `display.draw_file` and `display.draw_url` draw `480x480` **baseline** JPEG
  images on the screen and wake it if asleep. Transcode or crop camera
  snapshots and progressive JPEGs to `480x480` baseline JPEG before sending;
  call `display.show_animation` to return to Muse's face.
- `wg1200.notify` shows a dismissible on-screen alert card with an optional
  buzzer chime.
- `device.discover` scans the local Wi-Fi network (up to ~90 s; reuse recent
  results) and the Home Link tunnel routes approved unicast IPv4 HTTP or
  TCP-interleaved RTSP traffic between Muse and local LAN cameras or APIs. The
  user approves each LAN IP once in the Muse app.
- `device.boot_weatherxm` reboots into stock WeatherXM OS (`ota_0` or
  `factory`); call it only when the user explicitly asks to switch back.

## Controls

- Swipe down on the face for an hourly forecast card, or open `Settings >
  Weather` for live telemetry and one-tap actions.
- Top button (`GPIO38`): short press sleeps or wakes the screen (double press
  toggles BLE setup); hold 5 s and release to reset Muse; hold 10 s to reboot
  into WeatherXM without erasing Muse.

## Limits

- No microphone or speaker (piezo buzzer only; replies show on screen).
- Never modify stock WeatherXM partitions; commands work only while connected.
