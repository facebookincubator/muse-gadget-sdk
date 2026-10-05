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

# ESP32 Device SDK

Flash this open source firmware onto any ESP32-compatible board to connect
Muse to your home Wi-Fi. On boards with the home-network tunnel, Muse can reach
the devices you already own and anything you build with a local HTTP API.

Then hack on it: add a display, a button, or support for a board we haven't
tried yet, and build your own Muse gadget.

> **Note:** Built by hackers, for hackers, just for fun. Flashing custom
> firmware can brick boards and void warranties. Proceed at your own risk!

## What you need

- **An ESP32 board.** The quickest start is the **ESP32-C5 DevKitC-1**, which
  works as-is with its built-in status light and BOOT button. The other boards
  that already work are listed [below](#boards).
- **A USB cable that carries data**, not just power.
- **A computer** running macOS or Linux.
- **An SDK token** from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  (Account > SDK tokens). Every gadget needs one to pair, including ones you
  build for yourself. Read the [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms)
  before you use it.
- **The Muse app** on your phone, to set up the device once it's flashed.

## Get going with Muse Code

The fastest way to build is to let [Muse Code](https://developer.meta.com/ai/lp/muse-code/)
do it. Install it:

```sh
curl -fsSL https://dev.meta.ai/install.sh | sh
```

Then plug in your board, and start Muse Code from this directory:

```sh
git clone https://github.com/facebookincubator/muse-gadget-sdk
cd muse-gadget-sdk/esp32
muse --disable-sandbox
```

The first time, Muse Code asks you to trust the workspace and sign in.
`--disable-sandbox` lets it reach your board's USB serial port and download
the ESP-IDF toolchain. You still approve each command before it runs. Then ask:

> Build this firmware for my ESP32-C5 DevKitC-1 and flash it.

Muse Code reads [`AGENTS.md`](AGENTS.md), which has everything needed to set up
the toolchain, build for your board, flash it, and read its logs. From there,
keep going:

> Watch the serial log and tell me when it's ready to pair.

> I have a Waveshare ESP32-S3 AMOLED board. Build the UI for it.

> Add support for my board. It's an ESP32-S3 with 16 MB flash, a button on
> GPIO 0 and no PSRAM.

> Make the status light half as bright.

Other coding agents work too. Any agent that reads `AGENTS.md` can build and
flash from this repository. Flashing needs access to your board's USB serial
port, so if your agent runs in a sandbox, let it run the flash and monitor
commands outside the sandbox.

## Or do it yourself

### 1. Install ESP-IDF v6.0.1

This firmware is built with Espressif's ESP-IDF, **version 6.0.1**. Other
versions aren't supported. If you already have it installed, skip ahead.

On macOS, install the prerequisites first with `brew install cmake ninja
dfu-util python3`. On Linux, follow
[Espressif's prerequisites](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c5/get-started/linux-macos-setup.html).
Then:

```sh
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
~/esp/esp-idf-v6/install.sh esp32c5,esp32s3,esp32c6,esp32
. ~/esp/esp-idf-v6/export.sh
```

Run the last line in every new terminal you build from.

### 2. Build

From this directory, set your SDK token, then build:

```sh
idf.py menuconfig   # ESP32 Device SDK > Muse Gadgets SDK token
idf.py build
```

This builds for the ESP32-C5 DevKitC-1. The firmware lands in
`build/muse-gadget.bin`.

### 3. Flash

Plug in the board and flash it:

```sh
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

Replace the port with your board's. On macOS, `ls /dev/cu.usb*` lists the
connected ports; on Linux, look for `/dev/ttyACM*` or `/dev/ttyUSB*`. The
monitor shows the device's log; press `Ctrl-]` to quit. If flashing can't
connect, hold **BOOT**, tap **RESET**, release **BOOT**, and try again.

Reflashing keeps your pairing and Wi-Fi settings. To start completely fresh,
run `idf.py -p PORT erase-flash` first.

### 4. Set it up with Muse

Once flashed, the status light breathes **orange**: the device is ready for
setup. In the Muse app, turn on **Settings > Devices > Developer mode**, then
add the device (**Settings > Devices > Add Device**, the **+** icon in the top
right). It shows up as `MuseGadget-XXXXXX`. When the light breathes **blue**, press the **BOOT**
button to confirm it's really you. The light turns **green** when Muse is
connected.

| Light | What it means |
|---|---|
| Orange, breathing | Ready for setup |
| Blue, breathing | Press the button to confirm pairing |
| Blue | Joining Wi-Fi and connecting to Muse |
| Green | Connected |
| Yellow, blinking | Reconnecting |
| Purple | Not paired |
| Red, blinking | Something went wrong: check the log |

To reset the device and set it up again, hold the button for 5 seconds.

Pairing requires a press of the button on the device, and every setup creates
a fresh encrypted session. Because these are community devices, pairing has no
manufacturer verification and can't prevent an active man-in-the-middle
attack. Set it up on a network you trust.

## Boards

Boards marked UI run the full on-screen UI: an animated avatar, push-to-talk and
settings. Audio and image support vary by board, so check the feature table in
[`devices/`](devices). The others show status on a light, a ring or a simple
status screen.

| Board | What you get | Build it |
|---|---|---|
| ESP32-C5 DevKitC-1 | Status light and button | `idf.py build` |
| ESP32-C6 devkit without PSRAM | Status light and button | `tools/board.sh c6-nopsram build` |
| Espressif ESP32-S3-DevKitC-1 | Status light and button | `tools/board.sh espressif-s3-devkitc-1 build` |
| ideaspark ESP32 with 1.9" display | Status on screen, images | `tools/board.sh ideaspark build` |
| Waveshare ESP32-C6-LCD-1.47 | Status on screen, images | `tools/board.sh waveshare-c6-lcd-147 build` |
| Seeed SenseCAP Indicator | Status on a 4" screen, images | `tools/board.sh sensecap-indicator build` |
| Seeed reTerminal E1001 | Status on a 7.5" e-paper, black and white images | `tools/board.sh reterminal-e1001 build` |
| Seeed reTerminal E1002 | Status on a 7.3" e-paper, six-colour images | `tools/board.sh reterminal-e1002 build` |
| Home Assistant Voice Preview Edition | Status on the LED ring, push-to-talk, volume dial | `tools/board.sh home-assistant-voice build` |
| Seeed reSpeaker Lite with XIAO ESP32-S3 (experimental) | Single RGB LED, BOOT push-to-talk; 16 kHz XMOS I2S required | [Setup](devices/seeed-respeaker-lite.md) |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | UI, push-to-talk, settings, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | UI, push-to-talk, settings, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| Espressif ESP32-S3-BOX-3 | UI, touch, push-to-talk, settings, images | [BOX-3 setup](devices/esp32-s3-box-3.md) |
| AIPI Lite | UI, push-to-talk, two-button menu, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| Waveshare ESP32-C6-Touch-AMOLED-1.8 | UI, push-to-talk with text replies | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| Seeed SenseCAP Watcher | UI, push-to-talk, settings, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| M5Stack Cardputer ADV (experimental) | UI, GO/Space push-to-talk with text replies, Esc/Enter/arrow menu controls | `tools/muse/board.sh build cardputer-adv` |
| M5Stack StickS3 | UI, push-to-talk, two-button menu, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| M5Stack StopWatch | UI, push-to-talk, settings, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| M5Stack CoreS3 | UI, touch, PWR push-to-talk, settings, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| Guition JC3248W535 | UI, touch, settings, images, speaker; push-to-talk with an added I2S mic | `tools/muse/board.sh build jc3248w535` |
| Waveshare ESP32-S3-Touch-LCD-7 | UI, touch, settings, images; no built-in audio | `tools/muse/board.sh build lcd7` |
| M5Stack StickC Plus2 | UI, push-to-talk, two-button menu, images | see [`AGENTS.md`](AGENTS.md#boards-with-the-full-ui-by-hand) |
| M5Stack Core2 (v1.0) | UI, push-to-talk on the touch strip, touch menu, images | `tools/muse/board.sh build core2` |
| Freenove FNK0104B | UI, touch, BOOT push-to-talk, settings, images | `tools/muse/board.sh build fnk0104b` |

See [`devices/`](devices) for each board's hardware, features, and where to
buy one.

`tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]` builds each
board in its own `build-<board>` directory with the right chip and settings.
Boards that support images can show pictures Muse sends them:
`tools/image_for_display.py` prepares a picture for the screen size.

Boards without PSRAM, like the classic ESP32 and the ESP32-C6, run without the
home-network tunnel, which needs more memory than they have. Muse can still
reach the device and control it.

## Hack and extend it

Every board's settings live in a small `devices/sdkconfig.<board>` file loaded
on top of `sdkconfig.defaults`. To add a board, copy the closest one and change
the chip, button pin, status light, and flash size.
[`devices/README.md`](devices/README.md#add-a-board) covers the details, or ask
Muse Code to do it for you.

To put your own avatar on a board's screen, plug in the board and run
`python3 tools/muse/avatar.py`. It asks your Muse to redraw its avatar as the
board's pixel avatar, checks the result, then builds and flashes it. Your avatar
stays out of git. See [`tools/muse/AVATAR_RECIPE.md`](tools/muse/AVATAR_RECIPE.md)
for how it works and for boards that need the manual steps.

To give Muse a command of its own, such as reading a sensor or switching a
relay, advertise it in `link.register` and handle it in `main/app.c`.
[`AGENTS.md`](AGENTS.md#adding-a-command) walks through it.

To work on the UI without a board, use the
[`simulator/`](simulator/README.md) desktop preview. It runs the production UI
and avatar renderer in a 412 x 412 SenseCAP Watcher window, supports mouse and
keyboard input, and can render scripted screenshots without a display server.

Replies from Muse are text: push-to-talk sends your voice note, Muse
transcribes it and answers in writing, and boards with a screen show the
answer as captions (Voice PE and reSpeaker Lite replies show up in the Muse app). Two
things you can change:

- **Shorter answers.** Ask for them in the message itself, such as "Answer in
  one sentence."
- **Brief push-to-talk replies by default.** Enable
  `Muse > Request brief push-to-talk replies` in `idf.py menuconfig`
  (`CONFIG_MUSE_BRIEF_VOICE_REPLIES=y`). Each voice note includes a request
  for at most two short sentences, about 40 words, suitable for spoken
  playback. Say that you want more detail to override the request. This
  applies to both voice-note transports, including headless voice boards;
  typed messages and streaming dictation are unchanged. Muse may exceed
  the requested length. No response is truncated and no TTS is enabled.
- **Spoken answers.** Send each reply's text to a text-to-speech API of your
  choice and play the audio it returns. On boards with PSRAM, `start_tts` in
  [`components/muse/muse_chat_session.cpp`](components/muse/muse_chat_session.cpp)
  is the spot: it has the reply text, and the MP3 decoder, speaker and volume
  are already wired up there.

A few things worth knowing:

- Your SDK token ships inside the firmware, so treat it as an identifier
  rather than a password. If it leaks, revoke it on gadgets.muse.ai, generate
  a new one, and rebuild.
- **We strongly recommend enabling NVS encryption** if your board supports it.
  NVS stores Wi-Fi credentials and device tokens in flash; without encryption,
  anyone with physical access to the board can read them. Enable it with
  `CONFIG_HOMEHUB_NVS_ENCRYPTION` in `idf.py menuconfig` (under "ESP32 Device
  SDK"). The encryption key is derived from an HMAC key in eFuse, which is
  generated automatically on first boot if the eFuse block is available.
- Builds are version `999.0.0`. Over-the-air updates are off by default; the
  boards that run the full on-screen UI turn them on. Change it with
  `CONFIG_HOMEHUB_OTA_ENABLED` in `idf.py menuconfig`, and set a version with
  `idf.py -DPROJECT_VER=1.0.0 build`.
- Each build keeps its generated settings in its build directory
  (`build/sdkconfig`). If you change `sdkconfig.defaults` or a board file,
  delete the build directory so the change takes effect.
- Builds are signed with the included development key and never turn on
  Secure Boot, so you can reflash your board as often as you like.

## Tests

The tests run on your computer, with no board attached:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

Run `idf.py build` once first so the downloaded components are in place.

Two crypto tests are skipped unless the host has what they need. Install the
host Mbed TLS library (`brew install mbedtls pkgconf` on macOS,
`apt-get install libmbedtls-dev pkg-config` on Debian or Ubuntu), and run the
tests from a terminal where you ran ESP-IDF's `export.sh`, which sets
`IDF_PATH`. The last line of the output shows `skipped=` if any were left out.

## Community

Meet other hackers who are building and customizing Muse gadgets in our
community [Discord](https://discord.gg/3bhjCkZdd6). Get inspired, support each
other, and share what you make.

## License

The ESP32 Device SDK is licensed under the Apache License, Version 2.0, found
in [`LICENSE`](../LICENSE), except for these third-party files, which keep
their upstream licenses:

- [`components/minimp3/include/minimp3.h`](components/minimp3), the MP3
  decoder, is CC0-1.0. See [`components/minimp3/LICENSE`](components/minimp3/LICENSE).
- [`main/pixel_font.c`](main/pixel_font.c), the Adafruit GFX font, is
  BSD-2-Clause, as its header says.

ESP-IDF components fetched at build time (into `managed_components/`) are
under their own licenses.
