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

# Waveshare ESP32-S3-ePaper-1.54G (experimental)

An ESP32-S3-PICO-1-N8R8 with a 1.54" 200×200 e-paper panel in four inks (black, white, yellow and red), an
ES8311 with one mic and a small speaker, an SHTC3, a green LED and a single-cell battery. It runs the status
firmware with voice.

## Screen

The reTerminals' status, redrawn at 200×200 in the four inks. While idle the character fills the screen with
"Hold BOOT to speak"; a reply shows a smaller character above seven lines of text.
Text uses GNU Unifont bitmaps for Latin, Greek, Cyrillic, Armenian and Georgian letters, see [`main/fonts/README.md`](../main/fonts/README.md).

Images from Muse map each pixel to the nearest ink without dithering, so text and flat shapes stay sharp; Muse
is asked for raw RGB565 at 200×200. A full refresh takes about 20 s and flashes. When only part of the screen
changes, the panel refreshes that rectangle through its partial window (R83H in the [panel
manual](https://files.waveshare.com/wiki/ESP32-S3-ePaper-1.54G/1.54inch_e-Paper_G.pdf)); a change over 75% of
the panel, and every tenth window, is a full refresh.

## Buttons

- BOOT: hold to talk once voice is ready; without a voice connection it keeps its setup role.
- PWR, short press: next page of a long reply.
- PWR, held for a second: rotate the screen by 90° (kept across restarts).
- PWR held, then BOOT: setup. Tap BOOT to confirm pairing, hold it for five seconds to reset setup.

PWR also turns the board on from the battery; it then stays on until the battery is unplugged.

## Voice

Replies come back as text, as on the Voice PE without a TTS API, and go on screen as soon as they arrive. The
green LED is steady while listening, blinks slowly while Muse thinks and flashes on errors. Short cues on the
speaker mark the start and end of a recording and the PWR presses. The codec, mic and amplifier are off between
turns.

## Sensors and battery

`sensors.read` returns the SHTC3's temperature and humidity, as on the SenseCAP Indicator; the sensor sleeps
between reads. `device.health` reports `battery_mv` and `battery_pct` from the divider on GPIO4, and a thin red
strip at the top of the screen shows the level. `charging` is null: the charger's status pin only drives its
LED.

## Power

The CPU light-sleeps between tasks and Wi-Fi wakes for every tenth beacon, so a command can wait about a
second; voice turns, image downloads and OTA use the normal Wi-Fi power save while they run. A USB host on the
serial port keeps the chip awake, so measure on a charger or the battery.

## Build and flash

Use ESP-IDF v6.0.1. The board enumerates as the chip's own USB serial port, which looks like a DevKitC-1's, so
check which board is on the port. Back up the stock firmware first; restore it with
`write-flash 0 epaper154g.bin`. From `esp32/`:

```sh
python -m esptool --chip esp32s3 -p PORT read-flash 0 0x800000 epaper154g.bin
tools/board.sh waveshare-s3-epaper-154g build
tools/board.sh waveshare-s3-epaper-154g flash PORT
```
