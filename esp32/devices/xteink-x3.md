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

# Xteink X3

The Xteink X3 is a pocket e-ink reader with an ESP32-C3, a 3.7 inch
black and white e-paper, six buttons and a battery. With this firmware it
becomes a Muse gadget: Muse fills pages of text, and the buttons scroll
through them. The picture stays on the screen without power.

This is a community port. It is not affiliated with Meta or Xteink.

## What you get

- A status screen: the agent's name, the character and the connection status.
- Up to six text pages that Muse fills: priorities, your day, a workout, a
  note, a list. They are kept across restarts.
- A single text message, or a black and white image, when Muse sends one.
- Fast page turns with no flash. Every eighth turn is a full refresh, which
  flashes and clears the faint trace that fast turns leave.

## Read this before you flash

- **This replaces the reader firmware**, including the bootloader and the
  partition table. Take the full backup below first. With it you can go back
  in about a minute.
- **Do not flash a USB-locked X3.** Some units sold through third-party
  marketplaces have USB flashing disabled. Run the `get-security-info` check
  below; if esptool cannot connect at all, stop.
- **Only the UC8253 panel controller is supported.** Units made after about
  July 2026 have a UC8279d. The firmware detects it, logs
  `this X3 has a UC8279d panel, which is not supported yet`, and leaves the
  screen alone.
- **There is no sleep yet.** The firmware stays connected and drains the
  battery in hours, not weeks. Keep it on the cable.

## What you need

- An Xteink X3 and its **4-pin** magnetic USB cable. A 2-pin cable only
  charges.
- A computer with ESP-IDF v6.0.1 (see [`../README.md`](../README.md)),
  installed with `./install.sh esp32c3`.
- An SDK token from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  and the Muse app with Developer mode on.

## 1. Back up the device

The reader firmware turns USB off a few seconds after the last button press.
If the port (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux) is
missing, press a page button and run the command straight away. Once esptool
connects, the port stays.

```sh
python -m esptool --chip esp32c3 -p PORT get-security-info   # Secure Boot: Disabled
python -m esptool --chip esp32c3 -p PORT --baud 921600 read-flash 0x0 0x1000000 x3_backup.bin
```

The backup is 16,777,216 bytes and takes about a minute. Keep it somewhere
safe; it is the only copy of your device's factory firmware.

## 2. Build and flash

From `esp32/`:

```sh
idf.py -B build-xteink-x3 -DIDF_TARGET=esp32c3 -DSDKCONFIG=build-xteink-x3/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.xteink-x3" menuconfig
# ESP32 Device SDK > Muse Gadgets SDK token
tools/board.sh xteink-x3 build
tools/board.sh xteink-x3 flash-monitor PORT
```

The screen shows the status screen within a few seconds. The log should have:

```
link.led: X3 panel probe: VER ff ff ff -> UC8253
link.led: LED status ready: Xteink X3 UC8253 528x792 e-paper, 1 bit per pixel
link.pages: 3 pages; up/down scroll, back returns to the status screen
```

## 3. Pair with Muse

In the Muse app: **Settings > Devices > Developer mode**, then **Add Device**
and pick `MuseGadget-X3-XXXXXX`. When the screen says "Press the power button
to confirm", press the X3's power button once. Then choose a 2.4 GHz Wi-Fi
network. The screen shows "Connected" when Muse can reach it.

Holding the power button for 5 seconds resets the setup.

## Buttons

| Button | What it does |
|---|---|
| Down, Right | Next page |
| Up, Left | Previous page |
| Back | The status screen |
| Power | Confirm pairing; tap to retry Wi-Fi; hold 5 s to reset setup |

The scroll wraps round: the status screen, then each page, then the status
screen again. A new device has three sample pages, which Muse replaces.

## Ask Muse

- "Put my priorities on my X3: finish the report, call the bank, buy milk."
- "Show my day on my X3."
- "Write a note on my X3: the door code is 4417."
- "Show the text 'Back in 10 minutes' on my X3."

Muse sees these commands:

| Command | What it does |
|---|---|
| `pages.set` | Create or replace a page by name (`page`, `text`, optional `title`) and show it |
| `pages.clear` | Remove a page |
| `display.show_text` | Show one text message, not kept as a page |
| `display.draw_url` | Draw a JPEG or raw image, dithered to black and white |
| `display.show_animation` | Back to the status screen |

Text is plain ASCII in a 5x8 pixel font: about 26 characters a line and 18
lines a page.

## How it fits

The ESP32-C3 has 321 KB of RAM and no PSRAM. Two things make room:

- [`sdkconfig.xteink-x3`](sdkconfig.xteink-x3) keeps Wi-Fi, lwIP, Bluetooth,
  flash-driver and heap code in flash instead of RAM. Measured on the device
  with the Muse session connected, free heap went from 59 KB (largest block
  11 KB, with the session buffers failing to allocate four times) to 144 KB
  (largest block 100 KB). Any ESP32-C3 board needs the same settings.
- [`../main/epaper_x3_status.c`](../main/epaper_x3_status.c) draws into one
  1-bit frame of 52 KB. Images are dithered with an ordered pattern as they
  arrive, and fast refreshes use the copy of the last frame that the
  controller keeps itself.

With the screen and pages running, about 74 KB is free (largest block 32 KB).

## Go back to the reader firmware

```sh
python -m esptool --chip esp32c3 -p PORT write-flash 0x0 x3_backup.bin
```

## Not done yet

- Sleep, with the page left on the screen and a button to wake.
- The UC8279d panel controller.
- An image that installs from the SD card without changing the partition
  table, which is what a USB-locked unit would need.
- Images from Muse (`display.draw_url`) are built in but have not been tried
  on this board.
- The battery gauge, clock and motion sensor are not used.
