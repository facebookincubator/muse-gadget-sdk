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

# AGENTS.md

How to build this firmware and flash it onto your own ESP32 board. See
`README.md` for a shorter human overview.

## What this is

The ESP32 Device SDK: the ESP-IDF firmware (`muse-gadget`) powering Muse
Home Link. It pairs with a phone over BLE, joins Wi-Fi, and holds an encrypted
Noise session to a VM, with an optional home-network tunnel. `main/main.c` is the entry
point and `main/app.c` holds most of the logic. `components/muse` adds an
avatar/voice/settings UI on boards with a display.

## Prerequisites

- **ESP-IDF v6.0.3**, installed and activated (`. $IDF_PATH/export.sh`) so
  `idf.py` is on `PATH`. Other versions are unsupported. Errors about missing
  IDF headers such as `gcm.h` or `gpio_ll.h` usually mean the wrong IDF tag.
- Python 3 and a C/C++ compiler (`cc`, `c++`) for the host tests.
- A USB data cable. Flashing needs access to the serial port. A sandboxed
  agent usually has to run flash and monitor commands outside the sandbox.

## Supported boards

Board overlays live in `devices/`, which has a `README.md` listing each board's
hardware, features, and where to buy it, and an `AGENTS.md` recipe for adding
one. Its Vendor sources table lists each vendor's code for its boards: read it
before adding a feature to one.

| Board | Target | Overlay(s) after `sdkconfig.defaults` | Helper |
|---|---|---|---|
| ESP32-C5 DevKitC-1 (default) | `esp32c5` | none | `tools/board.sh devkit` |
| ESP32-C6 devkit without PSRAM | `esp32c6` | `devices/sdkconfig.c6-nopsram` | `tools/board.sh c6-nopsram` |
| Espressif ESP32-S3-DevKitC-1 v1.1 (N8R8) | `esp32s3` | `devices/sdkconfig.espressif-s3-devkitc-1` | `tools/board.sh espressif-s3-devkitc-1` |
| ideaspark ESP32 + 1.9" ST7789 | `esp32` | `devices/sdkconfig.ideaspark` | `tools/board.sh ideaspark` |
| Waveshare ESP32-C6-LCD-1.47 | `esp32c6` | `devices/sdkconfig.waveshare-c6-lcd-147` | `tools/board.sh waveshare-c6-lcd-147` |
| ESP32-2432S028 Cheap Yellow Display (experimental) | `esp32` | `devices/sdkconfig.esp32-2432s028` | `tools/board.sh esp32-2432s028` |
| ESP32-C3 with 0.42" OLED | `esp32c3` | `devices/sdkconfig.c3-oled-042` | `tools/board.sh c3-oled-042` |
| M5Stack CoreMatrix | `esp32c61` | `devices/sdkconfig.m5stack-corematrix` | `tools/board.sh m5stack-corematrix` |
| Waveshare ESP32-S3-1.54inch-ePaper V2 | `esp32s3` | `devices/sdkconfig.waveshare-s3-epaper-154` | `tools/board.sh waveshare-s3-epaper-154` |
| Waveshare ESP32-S3-ePaper-1.54G | `esp32s3` | `devices/sdkconfig.waveshare-s3-epaper-154g` | `tools/board.sh waveshare-s3-epaper-154g` |
| Seeed SenseCAP Indicator | `esp32s3` | `devices/sdkconfig.sensecap-indicator` | `tools/board.sh sensecap-indicator` |
| Seeed reTerminal E1001 | `esp32s3` | `devices/sdkconfig.reterminal-e1001` | `tools/board.sh reterminal-e1001` |
| Seeed reTerminal E1002 | `esp32s3` | `devices/sdkconfig.reterminal-e1002` | `tools/board.sh reterminal-e1002` |
| Seeed reTerminal E1003 | `esp32s3` | `devices/sdkconfig.reterminal-e1003` | `tools/board.sh reterminal-e1003` |
| Seeed reTerminal Sticky (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.reterminal-sticky` | `tools/board.sh reterminal-sticky` |
| Seeed reTerminal Sticky Link status (experimental) | `esp32s3` | `devices/sdkconfig.reterminal-sticky-link` | `tools/board.sh reterminal-sticky-link` |
| Home Assistant Voice Preview Edition | `esp32s3` | `devices/sdkconfig.home-assistant-voice` | `tools/board.sh home-assistant-voice` |
| Seeed reSpeaker Lite with XIAO ESP32-S3 (experimental) | `esp32s3` | `devices/sdkconfig.seeed-respeaker-lite` | `tools/board.sh seeed-respeaker-lite` |
| Seeed reSpeaker XVF3800 with XIAO ESP32-S3 (experimental) | `esp32s3` | `devices/sdkconfig.seeed-respeaker-xvf3800` | `tools/board.sh seeed-respeaker-xvf3800` |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c` | manual (below) |
| Waveshare S3 1.75C, Muse Gadget (W175: musegadgets BLE audio beside Wi-Fi) | `esp32s3` | the 1.75C's, then `devices/sdkconfig.muse-waveshare-s3-175c-gadget` | `tools/muse/board.sh build s3-gadget` |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175` | manual (below) |
| Waveshare ESP32-S3-Touch-LCD-1.85B | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b` | `tools/muse/board.sh build 185b` |
| Waveshare ESP32-S3-Touch-LCD-1.85C | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185c` | `tools/muse/board.sh build 185c` |
| Waveshare ESP32-S3-Touch-LCD-1.9 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd19` | `tools/muse/board.sh build lcd19` |
| Waveshare ESP32-S3-Touch-AMOLED-2.16 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-216` | `tools/muse/board.sh build s3-216` |
| Waveshare ESP32-S3-Touch-AMOLED-1.43C | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-143c` | `tools/muse/board.sh build 143c` |
| Espressif ESP-Mosaico (experimental; ESP-IDF master, see `devices/README.md`) | `esp32s31` | `devices/sdkconfig.muse;devices/sdkconfig.muse-esp-mosaico;devices/sdkconfig.muse-esp-mosaico-iris` | `tools/muse/board.sh build mosaico` |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-206` | `tools/muse/board.sh build s3-206` |
| OSTB EchoEar-2ST (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-ostb-echoear-2st` | `tools/muse/board.sh build echoear` |
| Espressif ESP32-S3-BOX-3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-espressif-box-3` | `tools/muse/board.sh build box3` |
| ALIENTEK ATK-DNESP32S3-BOX V1.1 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-atk-dnesp32s3-box` | `tools/muse/board.sh build atk-box` |
| ALIENTEK ATK-DNESP32S3-BOX2 (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-atk-dnesp32s3-box2` | `tools/muse/board.sh build atk-box2` |
| AIPI Lite | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-aipi` | manual |
| Waveshare ESP32-C6-Touch-AMOLED-1.8 | `esp32c6` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-c6-18` | manual |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-18` | `tools/muse/board.sh build s3-18` |
| Waveshare ESP32-C6-Touch-AMOLED-2.06 | `esp32c6` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-c6-206` | `tools/muse/board.sh build c6-206` |
| Waveshare C6 1.8, BLE-only musegadgets gadget | `esp32c6` | the C6's, then `devices/sdkconfig.muse-waveshare-c6-18-ble` | `tools/muse/board.sh build c6ble` |
| Seeed SenseCAP Watcher | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-sensecap-watcher` | manual |
| SenseCAP Watcher, Muse Gadget (SCW1: musegadgets BLE audio beside Wi-Fi) | `esp32s3` | the Watcher's, then `devices/sdkconfig.muse-sensecap-watcher-gadget` | `tools/muse/board.sh build watcher-gadget` |
| M5Stack Cardputer ADV (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cardputer-adv` | `tools/muse/board.sh build cardputer-adv` |
| M5Stack AtomS3 + Atomic Voice Base (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-atoms3` | `tools/muse/board.sh build atoms3` |
| M5Stack StickS3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-sticks3` | manual |
| Elecrow CrowPanel Advance 4.3 V1.3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-crowpanel-advance-43` | `tools/muse/board.sh build crowpanel-43` |
| M5Stack StopWatch | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch` | manual |
| M5Stack TriviaPOD (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-triviapod` | `tools/muse/board.sh build triviapod` |
| Seeed XIAO ESP32-S3 1.47 Touch Display | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-seeed-xiao-s3-147` | `tools/muse/board.sh build xiao147` |
| M5Stack CoreS3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cores3` | `tools/muse/board.sh build cores3` |
| M5Stack CoreS3 Lite | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cores3lite` | `tools/muse/board.sh build cores3lite` |
| Freenove FNK0104B | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-fnk0104b` | `tools/muse/board.sh build fnk0104b` |
| VN ESP32-S3 1.83-inch NV3023 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-vn-s3-183` | `tools/muse/board.sh build vn183` |
| Guition JC3248W535 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-guition-jc3248w535` | `tools/muse/board.sh build jc3248w535` |
| Waveshare ESP32-S3-Touch-LCD-7 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd7` | `tools/muse/board.sh build lcd7` |
| Waveshare ESP32-S3-Touch-LCD-1.28 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd128` | `tools/muse/board.sh build lcd128` |
| Waveshare ESP32-S3-Touch-LCD-1.46 (also 1.46B, 1.46C) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-146` | `tools/muse/board.sh build 146` |
| Waveshare ESP32-S3-Touch-AMOLED-1.43 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-143` | `tools/muse/board.sh build 143` |
| LilyGO T-Display-S3 AMOLED 1.75 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-lilygo-s3-amoled-175` | `tools/muse/board.sh build lilygo175` |
| Waveshare ESP32-S3-Touch-LCD-3.49 V2 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd349` | `tools/muse/board.sh build lcd349` |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 | `esp32p4` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-p4-lcd35` | `tools/muse/board.sh build p4lcd35` |
| SpotPear ESP32-AI-1.28 v1.0 (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-spotpear-esp32-ai-128` | `tools/muse/board.sh build spotpear` |
| VIEWE SmartRing-Plus | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-viewe-smartring-plus` | `tools/muse/board.sh build smartring-plus` |
| M5Stack StickC Plus2 | `esp32` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stickc-plus2` | manual |
| M5Stack Core2 (v1.0) | `esp32` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-core2` | `tools/muse/board.sh build core2` |
| FoloToy AI Passport (experimental) | `esp32c3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-ai-passport` | `tools/muse/board.sh build ai-passport` |
| Deotaland RoRoLee | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-deotaland-rorolee` | `tools/muse/board.sh build rorolee` |
| Waveshare ESP32-S3-Touch-LCD-3.5 | `esp32s3` | `devices/sdkconfig.waveshare-s3-lcd-35` | `tools/board.sh waveshare-s3-lcd-35` |
| Waveshare ESP32-S3-Touch-LCD-3.5 (UI) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd-35` | `tools/muse/board.sh build s3lcd35` |
| DFRobot UNIHIKER K10 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-dfrobot-unihiker-k10` | `tools/muse/board.sh build k10` |

The default profile expects the C5 DevKitC-1: an addressable status LED on
GPIO27, the BOOT button on GPIO28 (active low), 8 MB flash and quad PSRAM.
The DevKitC-1's LED takes red first (`CONFIG_HOMEHUB_LED_RGB_ORDER`, on by
default). If yours shows green as red and amber as green, it takes green first
like most WS2812s: turn the option off in `idf.py menuconfig`, or in
`build-devkit/sdkconfig`, and rebuild.

## Build

Every application build needs the user's SDK token (`mgst_…`, from gadgets.muse.ai >
Account > SDK tokens). Ask for it, then set `CONFIG_GADGET_SDK_TOKEN="mgst_…"`
in that build directory's `sdkconfig` (or with `idf.py menuconfig`) before
building. Without it the build warns, and the gadget will stop pairing once
Muse requires tokens. Never commit the token or print it in full. The firmware
logs no token characters, only whether it has one, its length and a SHA-256
prefix (`token_fp_text()`, `main/token_fp.h`), and the bench tools print every
record and every line of a gadget's console or log through `tools/redact.py`,
which masks secrets the same way.

The opt-in `CONFIG_MUSE_POWER_TEST` image is an isolated board-load
characterization harness, not a pairing application, and needs no SDK token or
network credentials. Power measurement starts at `docs/power/README.md`. Read
`docs/power/ppk2.md` before any PPK2 use (authorization and fixture gates,
continuous source ownership, USB unplug/replug), and
`docs/power/sensecap-watcher.md` for the Watcher's build and procedure. Never
enable source output with unknown wiring, a connected battery, or
USB-charger backfeed into PPK2. Other boards reuse the shared
`muse_power_test_run()` core through `boards/muse_power_test_board.h` hooks
and board-owned matrices, driven by `tools/power/power_sweep.py`; do not fork
or rename the core for the next board.

### DevKitC-1 (default)

```sh
idf.py build                    # -> build/muse-gadget.bin, config in build/sdkconfig
```

### Other boards, with the helper

`tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]` sets the
target and overlays and gives each board its own `build-<board>/` directory
and `build-<board>/sdkconfig`:

```sh
tools/board.sh sensecap-indicator build
tools/board.sh ideaspark flash-monitor /dev/cu.usbserial-110
```

Without a port it uses the only matching serial port, and fails if it finds
none or more than one. If `idf.py` is not on `PATH`, it sources `export.sh`
from `$IDF_PATH`, `~/esp/esp-idf-v6.0.3`, `~/esp/esp-idf-v6` or `~/esp/esp-idf`.
The Sticky's Muse and Link status profiles build in `build-reterminal-sticky/`
and `build-reterminal-sticky-link/`. Set the SDK token in the chosen build
directory through `idf.py -B <dir> menuconfig`.

### Home Assistant Voice Preview Edition

`tools/board.sh home-assistant-voice build` builds a status-and-voice gadget:
the LED ring shows the status colours, holding the centre button records a
voice note that Muse answers in the app, and the dial sets the speaker volume
(shown on the ring, kept across restarts). It advertises as
`MuseGadget-ha-voice-XXXXXX`.

- The console and flashing go through the S3's own USB-Serial-JTAG, which
  shows up as `/dev/cu.usbmodem*` like a DevKitC-1. With both plugged in, pass
  the port, and check it with `python -m esptool read-mac` before flashing.
- Flashing replaces the vendor firmware on the S3 only. The XMOS audio chip
  keeps its own firmware; never reflash it.
- While voice chat is ready, the centre button is push-to-talk. Turn the mute
  switch on to get its setup role back (pairing confirmation, the 5 s
  factory-reset hold).

### Seeed reSpeaker XVF3800 (experimental)

`tools/board.sh seeed-respeaker-xvf3800 build` builds a Link-only voice profile
for the XIAO ESP32-S3 SKU. XVF3800 needs its 16 kHz I2S-slave firmware at the
1.0.9 baseline. It uses XIAO BOOT for pairing and push-to-talk, with app prompts
and serial logs instead of a Muse status light. The vendor ring is unused. See
the [XVF3800 guide](devices/seeed-respeaker-xvf3800.md) for firmware files,
controls and unverified hardware behavior.

### Boards with the full UI, by hand

`tools/muse/board.sh build|flash <s3|s3-gadget|s3-video|s3n|s3-18|s3-216|s3-206|185b|185c|143c|143|146|lilygo175|mosaico|aipi|atk-box|atk-box2|box3|c6|c6-206|c6ble|watcher|watcher-gadget|watcher-video|sticks3|crowpanel-43|plus2|cardputer-adv|atoms3|stopwatch|stopwatch-video|triviapod|xiao147|cores3|cores3lite|core2|fnk0104b|jc3248w535|lcd7|lcd128|lcd349|spotpear|vn183|ai-passport|echoear|p4lcd35|rorolee|lcd19|smartring-plus|s3lcd35|k10> [SERIAL|PORT]`
builds one board in `build-muse-<profile>/`, logs to
`/tmp/muse_build_<board>.log`, and clears `managed_components/` before and
after so it doesn't clash with other boards. When flashing, it finds the
board's port with `tools/muse/ports.py`, by the USB device behind it rather
than the port name, which changes when boards are re-cabled. With several
boards of one kind attached, pass the device's USB serial number (on the chip's
own USB serial port, its MAC) or the port; `tools/muse/ports.py --list` shows
them. It finds ESP-IDF the way `tools/board.sh` does, trying
`~/.espressif/esp-idf-v6.0.3` first. For an install anywhere else, set
`IDF_EXPORT` to its `export.sh`, e.g. in your shell profile:
`export IDF_EXPORT=/path/to/esp-idf/export.sh`. `tools/muse/avatar.py` builds
through `board.sh`, so it needs the same.

`s3` and `watcher` build the board's Wi-Fi Muse image, the one the web
flasher ships; `s3-gadget` and `watcher-gadget` add its Muse Gadget overlay
(`devices/sdkconfig.muse-<profile>-gadget`: musegadgets BLE audio, the
catalog code, the BLE gadget block) in `build-muse-<profile>-gadget/`, and
`s3-video` and `watcher-video` build on that profile.

For bench testing, `MUSE_BENCH=1 tools/muse/board.sh build|flash ...` adds
`devices/sdkconfig.muse-bench` and uses `build-muse-<profile>-bench/`. Lab
and CI builds use it; every board overlay alone builds a release image. Bench
and test images go on bench units only: a gadget someone wears or carries runs
a release image, with its mic gates on. It
turns on the gadget automation tools (`CONFIG_MUSE_GADGET_AUTOMATION`: every
Nordic UART word open to any connection, Console words over Bluetooth below),
the sealed clear's cap (`CONFIG_MUSE_GADGET_SEALED_CLEAR`, off in release
images, which keep accepting the unsealed clear) and screenshots: `tools/muse/snap.py PORT KEYS OUT.png` sends bench keys
and saves the screen, and `>face=thinking` (or `idle`, `listening`,
`speaking`, `error`, `boot`, `off`, `happy`) in KEYS picks the avatar mode first.
On touch boards, `>ui.settings=wifi` (or `home`, `muse`, `ble`, `sound`,
`sleep`, `battery`, `power`) opens that settings page for the shot.
Screenshots are off in normal builds because each one takes a buffer the size
of the screen. The bench overlay also logs UI timing on `mg.perf` every 5 s
(`CONFIG_MUSE_PERF`, `components/muse/muse_perf.h`): frames drawn and their
rate, frame time split into render, panel wait and flush, pixels per frame,
the avatar's frame tick, free internal RAM and each task's CPU share, plus a
line per touch (interrupt, read, first flush, frame done) and per talk press
(mode change, UI tick, first flush, frame done). On a musegadgets BLE build
`tools/mg_ble_client.py log` shows them over the Nordic UART mirror. `>face=` works in any build. Or run `idf.py` directly:

```sh
idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" build
```

To flash, add `-p PORT flash` with the same `-B`, target and defaults
arguments, except on the SenseCAP Watcher: `idf.py flash` runs plain esptool,
which fails on its USB bridge (see "Flash it"). Build the Watcher with
`idf.py`, flash it with `tools/muse/board.sh flash watcher`, then
`idf.py … -p PORT monitor` as usual; reading from the bridge works. Muse builds use `partitions_muse.csv` and need 16 MB of flash or
more, except the StickS3, StickC Plus2, Cardputer ADV, AtomS3, AI Passport and Waveshare 1.43C, which have 8 MB and use
`partitions_muse_8mb.csv`.

On the Watcher, rotation opens and pages through the latest reply. A short
wheel click sleeps/wakes; a 300 ms hold starts push-to-talk. Pairing still
confirms on the initial press, never on a hold's push-to-talk.
With the camera built in (`CONFIG_MUSE_PHOTO_NOTES`) and Photo with voice
notes turned on in Settings, that hold also takes a photo, which
`muse_photo_note.c` sends with the note (`muse_chat_attach_jpeg()`); see
`devices/README.md`.
`muse_review.c` keeps up to 32 KB of the reply in PSRAM across the end of the
turn. `>review.next`, `>review.prev`, and `>review.close` exercise navigation
over the serial console.

All boards share `managed_components/` and `dependencies.lock` in this
directory. If the component manager fails after you switch between a board
with the full UI and one without (lvgl is the usual offender), delete both and build again.

### OSTB EchoEar-2ST (experimental)

`tools/muse/board.sh build echoear` uses the measured ESP32-S3 rev 0.2 unit
with 16 MB DIO flash and 8 MB octal PSRAM at 80 MHz. Native USB Serial/JTAG
(`303a:1001`) is shared by several boards, so it does not identify the model
on its own. [EchoEar setup](devices/ostb-echoear-2st.md) records its pin map
and limits. The bottom button controls physical power; GPIO0 is not a talk
button. Talking and pairing confirmation use the touch screen. There's no
software power-off, so Settings has no Power off option.

The final port was tested on hardware: pairing, touch, recording and
playback, sleep and wake, and reconnect. See its setup guide for what wasn't
checked.

### A new board

Follow `devices/AGENTS.md`, which covers boards with a status light, a status
screen or the full UI from the overlay to verification. In short: copy the
closest overlay to `devices/sdkconfig.<yourboard>`, load it last, and add the
board to `devices/README.md`. At minimum set `CONFIG_IDF_TARGET`,
`CONFIG_HOMEHUB_BUTTON_GPIO`, the LED backend (`CONFIG_HOMEHUB_LED_BACKEND_*`,
or `..._NONE`), and the flash size and mode. Without PSRAM, also set
`CONFIG_SPIRAM=n`, `CONFIG_HOMEHUB_TUNNEL=n` and the mbedtls
internal-allocation options, as `devices/sdkconfig.ideaspark` does. Signed
apps on a classic ESP32 need chip revision 3.0 or later
(`CONFIG_ESP32_REV_MIN_3=y`).

## Flash

### Identify the board first

When you're asked to flash, work out which board is attached instead of asking
which one it is or assuming the default. Say which board you found and what told
you, then flash that profile. Getting it wrong writes another board's pin map,
flash size and status backend.

1. **Ask the running firmware.** A board already running this firmware names
   itself, and this needs no guessing:

   ```sh
   python3 tools/muse/chat.py --status     # {"board": "M5Stack StickS3", ...}
   ```

   It also logs the name once at startup, as `muse: board: <name>`, which
   `tools/muse/monitor.py PORT` captures because it resets the board first.
   `components/muse/muse_app.c` logs it; each `components/muse/boards/board_*.c`
   sets its `.name`. Boards with the full UI only: the other overlays don't log a name.

2. **Read the USB descriptor.** This works with no write access to the port and
   whatever the board is running, including vendor firmware or nothing.
   `ioreg -p IOUSB -l -w 0` on macOS, `lsusb -v` or `udevadm info /dev/ttyACM0`
   on Linux:

   | Descriptor | Board |
   |---|---|
   | Espressif `303a:1001`, "USB JTAG/serial debug unit" | the chip's own USB: C3, C5, C6, C61, S3 and the S3 boards with the full UI. Its serial number is the MAC |
   | Espressif `303a:8120` | M5Stack AtomS3 still running its stock UiFlow firmware |
   | CH340 (`1a86:7523`) | ideaspark, CYD, SenseCAP Indicator, reTerminal E1001 and E1002 |
   | CH340 (`1a86:7522`) | Elecrow CrowPanel Advance 4.3 V1.3 |
   | CH9102 | M5Stack StickC Plus2, or an M5Stack Core2 |
   | CP2104 (`10c4:ea60`) | M5Stack Core2 (it shipped with either bridge) |
   | CH343 (`1a86:55d3`), "USB Single Serial" | Seeed reTerminal Sticky, Waveshare ESP32-S3-Touch-LCD-7, ESP32-S3-Touch-LCD-1.28 or ESP32-P4-WIFI6-Touch-LCD-3.5: `chip-id` and `flash-id` tell them apart (the P4 is an `esp32p4`; 8 MB of PSRAM on the Sticky and the 7-inch, 2 MB on the 1.28-inch; 32 MB of flash on the Sticky, which `flash-id` shows) |
   | CH342, two `usbmodem` ports | SenseCAP Watcher: the S3 console is the one ending in `3`, the other is the Himax camera |

3. **Ask the chip.** When you can write to the port (this resets the board):

   ```sh
   python -m esptool -p PORT chip-id      # read-mac for the MAC
   ```

   The target narrows it a long way: `esp32c5` is the DevKitC-1, `esp32c6` the
   Waveshare C6 or a C6 devkit, `esp32c3` the AI Passport or the C3 with a
   0.42" OLED (`flash-id`: 8 MB on the AI Passport, 4 MB on the OLED board),
   `esp32c61` the CoreMatrix, `esp32` the ideaspark, the CYD, the StickC Plus2
   or the Core2.

4. **Fall back to a read-only capture.** If the board is mid-run and you can't
   write to the port, the `## Monitor` recipe below reads it without resetting,
   and a talk press logs the board's own button name as `muse_input: waking
   (<hint>)`:

   | Hint | Board |
   |---|---|
   | `yellow top-left` (`top-right` or `yellow` on older firmware) | M5Stack StopWatch or TriviaPOD — tell them apart by the `muse: board:` line a reset logs |
   | `USR1` | Seeed XIAO ESP32-S3 1.47 Touch Display |
   | `power` | M5Stack CoreS3, CoreS3 Lite |
   | `M`, `R`, `L` or `Q` | ALIENTEK ATK-DNESP32S3-BOX2 |
   | `front` | M5Stack StickS3, AtomS3 or StickC Plus2 — the Plus2 is a CH9102 `usbserial`; tell the StickS3 and AtomS3 apart by the `muse: board:` line a reset logs |
   | `top` | Waveshare ESP32-S3-Touch-AMOLED-1.75C |
   | `key` | Waveshare ESP32-S3-Touch-AMOLED-2.16 |
   | `bottom right` | AIPI Lite |
   | `wheel` | Seeed SenseCAP Watcher |
   | `boot` (`BOOT` on the C6 1.8 since the v1.0 UI) | Waveshare ESP32-C6-Touch-AMOLED-1.8 or 2.06, the ESP32-S3-Touch-AMOLED-1.75, 1.8, 2.06, 1.43C or 1.43, the LilyGO T-Display-S3 AMOLED 1.75, the Elecrow CrowPanel Advance 4.3, the Guition JC3248W535, the Waveshare ESP32-S3-Touch-LCD-1.85B or 3.49, the VN ESP32-S3 1.83-inch NV3023, the Deotaland RoRoLee, the Waveshare ESP32-S3-Touch-LCD-1.9, the Waveshare ESP32-S3-Touch-LCD-3.5, the Espressif ESP32-S3-BOX-3, the Freenove FNK0104B or the SpotPear ESP32-AI-1.28 v1.0 — tell the C6 boards by their target (`esp32c6`), and each board by the `muse: board:` line a reset logs |
   | `BOOT` | Waveshare ESP32-S3-Touch-LCD-1.46 or 1.85C, or the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 — tell the P4 by its target (`esp32p4`), and the S3 boards by the `muse: board:` line a reset logs |
   | `screen` | Waveshare ESP32-S3-Touch-LCD-7 or ESP32-S3-Touch-LCD-1.28 — tell them apart by the `muse: board:` line a reset logs |
   | `screen microphone` | OSTB EchoEar-2ST or VIEWE SmartRing-Plus — tell them apart by the `muse: board:` line a reset logs |
   | `middle` | M5Stack Core2 |
   | `ok` | FoloToy AI Passport |
   | `AI` | Espressif ESP-Mosaico |
   | `K0` | ALIENTEK ATK-DNESP32S3-BOX |
   | `A` | DFRobot UNIHIKER K10 |
   | `GO/space` | M5Stack Cardputer ADV |
   | `AI button` | Seeed reTerminal Sticky |

Ask the user only when these come up empty or contradict each other, and say
what you found and what's ambiguous rather than asking from scratch.

### Flash it

```sh
idf.py -p PORT flash                           # default DevKitC-1 build
tools/board.sh devkit flash PORT               # same, in build-devkit/
cd build && python -m esptool --chip esp32c5 -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

Typical ports: `/dev/cu.usbmodem*` or `/dev/ttyACM*` for native USB (C3, C5, S3
and C6 boards), and `/dev/cu.usbserial-*`, `/dev/cu.wchusbserial*` or `/dev/ttyUSB*`
for CH340 bridges (ideaspark, CYD, SenseCAP Indicator, reTerminal E1001 and E1002,
and CrowPanel Advance 4.3). On Linux,
add yourself to the `dialout` (or `uucp`) group. If the chip won't enter the
bootloader, hold BOOT, tap RESET, release BOOT, and flash again.

The M5Stack StickC Plus2's CH9102 bridge shows up as `/dev/cu.usbserial-*` or
`/dev/ttyACM*`. It drops out above 230400 baud, so `tools/muse/board.sh`
flashes it at 230400.

The SenseCAP Watcher's bottom USB-C port has a CH342 bridge with two ports,
both `/dev/cu.usbmodem*` on macOS. The ESP32-S3 console is the second (ending
in `3`); the first is the Himax camera chip. Muse overwrites the Watcher's
factory data, so back up its `nvsfactory` partition first (see
`devices/README.md`).

The CH342 has no flow control and drops bytes when esptool sends a whole
packet at once, at any baud and on any USB port. Plain esptool then fails with
`Failed to write to target RAM (result was 0107: Checksum error)` while
uploading its stub, or `0105: The format of the received message is invalid`
on the first flash block, by which point it has erased the bootloader. That
leaves the Watcher unable to boot until a flash succeeds; the ROM loader still
answers, so it isn't bricked. `tools/muse/board.sh flash watcher` goes through
`tools/muse/paced_esptool.py`, which takes esptool's arguments and sends 64
bytes at a time at the line rate, at 115200 baud. Use it in place of
`python -m esptool` for anything that writes to the Watcher. A lower baud or
another USB port doesn't help. Pacing works with macOS's built-in driver, so
WCH's driver isn't needed.

`board.sh` prints only esptool's last three lines, so a Watcher flash shows
nothing for about three minutes. Don't interrupt it, which leaves the board
unbootable again. To watch the progress, run the wrapper from the build
directory:

```sh
cd build-muse-sensecap-watcher
python ../tools/muse/paced_esptool.py --chip esp32s3 -p PORT -b 115200 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

- `flash` writes the bootloader, the partition table (at `0x10000`, not the IDF
  default `0x8000`), otadata, phy_init and the app to `ota_0`. NVS is left
  alone, so pairing and Wi-Fi credentials survive a reflash.
- `idf.py -p PORT erase-flash` wipes everything, including pairing. Do this
  only when you mean to start from scratch.
- The app is signed with the committed `dev_signing_key.pem`, and Secure Boot
  is **not** enabled, so flashing never burns eFuses. Keep it that way: don't
  enable `CONFIG_SECURE_BOOT`, flash encryption, or
  `CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH` on a board you want to keep reflashing.

## Monitor

`idf.py -p PORT monitor` (Ctrl-] to quit) resets the board and needs an
interactive terminal. An agent without a TTY can do a read-only capture that
doesn't reset the board:

```py
import os, termios, time
fd = os.open("/dev/cu.usbmodem1101", os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[2] &= ~termios.HUPCL   # don't toggle DTR/RTS (which resets) on close
termios.tcsetattr(fd, termios.TCSANOW, attrs)
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    try:
        chunk = os.read(fd, 4096)
        if chunk:
            os.write(1, chunk)
    except BlockingIOError:
        time.sleep(0.05)
```

To capture from boot, use `tools/muse/monitor.py PORT [secs]`, which resets the
board first (it needs `pyserial`). A healthy boot logs
`link.main: Muse Gadget starting`. For a board with the full UI, `$(tools/muse/ports.py BOARD)`
gives the port, with BOARD as in `tools/muse/board.sh`.

## Update my avatar

When someone asks to put their own avatar on their board (or to update or
change it), run this from `esp32/` with the sandbox off (it uses the serial
port) and let it finish. It takes several minutes, mostly waiting for Muse:

```sh
python3 tools/muse/avatar.py                    # draw their avatar
python3 tools/muse/avatar.py --edit "CHANGE"    # change the one they have
python3 tools/muse/avatar.py --restore previous # swap back to the previous avatar
python3 tools/muse/avatar.py --restore default  # use the bundled default avatar
python3 tools/muse/avatar.py --status           # host selection, checked builds and flash history
```

It checks the board, asks their Muse for the renderer through the board,
checks it on the host and builds the firmware with it in a copy of the tree,
and only then saves it to `components/muse/avatar/muse_pixel.c` (gitignored)
and flashes. It doesn't rebuild their own `build-muse-*/`: flashing that with
`tools/muse/board.sh` before a `board.sh build` puts the previous avatar back.
`--restore` rechecks the selected source and builds it before changing the
selection. A previous default is remembered too. With `--no-flash`, only the
host selection changes; the checked build is retained and the device stays as
it was. `--status` needs no board and reports this computer's selection and
this tool's flash history; it cannot identify what the attached board runs.
Progress and errors go to stderr. Relay the line that starts with `Muse drew:`
and the GIF paths. On failure, pass the message on. The exit status says what
kind of failure it was:

- **2**: no board, or it can't do this (or another `avatar.py` is still
  running: let it finish). If it says the board doesn't answer,
  rerun with `--board s3` or `--board aipi` (from the board's name, or ask) to
  flash firmware that can. On the C6 boards, follow the manual steps in
  `tools/muse/AVATAR_RECIPE.md`.
- **3**: the board isn't on Wi-Fi, or it isn't paired in the Muse app and has
  no device token, so their Muse isn't connected. Tell them, and point them to
  the Muse app (or the setup page on test builds).
- **1**: Muse's file still fails after two fix rounds, or the build or flash
  failed. A file that fails a check or the build isn't saved: their avatar is
  as it was. If only the flash failed, the new avatar is saved (the one before
  is `muse_pixel.c.prev`) and its checked build stays in
  `components/muse/avatar/firmware-BOARD/`; the error says how to flash it.

Don't commit anything in `components/muse/avatar/`. To see what the board
says, run `python3 tools/muse/chat.py --status`. To ask their Muse something,
run `python3 tools/muse/chat.py "question"`.

The default avatar is in `avatar/`: its renderer (`muse_pixel.c`) and
its animation (`jollybot.gif`, and `happy_anim.c/.h` made from it by
`tools/gen_happy_anim.py`).

Third-party code keeps its upstream license and header: `minimp3.h` (CC0),
`main/pixel_font.c` (BSD-2-Clause, Adafruit),
`components/muse/boards/ostb_echoear_display_init.h` (MIT, 78/xiaozhi-esp32),
`components/muse/boards/viewe_smartring_display_init.h` (Apache-2.0, VIEWE),
`components/muse/boards/waveshare_s3_185b_init.h` (Apache-2.0, Waveshare),
`components/esp_lcd_gc9107` (Apache-2.0, Espressif),
`main/epd_ssd1677.c` (MIT, Seeed-reTerminal-E10xx-Firmware),
`main/epd_it8951.c` and `main/uc8179_gray4_lut.h` (MIT, Seeed_GFX2),
`../xplat/tinyh264/src` (Apache-2.0, the Android Open Source Project) and
`../xplat/libvpx_vp8/src` (BSD-3-Clause and `PATENTS`, the WebM Project).
Each third-party file is listed under License in `README.md` and the root
`README.md`; add a new one to both.
Don't restyle them or replace their headers with the Apache one; `components/minimp3/README.md` says how to
update minimp3.

The Apache License doesn't cover the Jollybot avatar in `avatar/`. Its files
carry only a Meta copyright line; don't add the Apache header to them.

## First boot and pairing

The status LED (or the edge bars or avatar on display boards) shows the state:

| Colour | Meaning |
|---|---|
| orange, breathing | BLE advertising, waiting for setup |
| blue, breathing | pairing needs an explicit device press (screen on EchoEar-2ST) |
| blue | Wi-Fi or VM session coming up |
| green | connected (tunnel up, or control session up when the tunnel is off) |
| yellow, blinking | VM switching or connection lost |
| purple | unpaired |
| red, blinking | error |

Button (BOOT on the dev boards):

- **short press**: confirm a pending pairing, or reopen the setup window if
  setup isn't complete
- **hold for 5 s**: reset setup (unpair and forget Wi-Fi)

Only a press that begins after the confirmation prompt confirms it, on every
board: a press already under way (or held) when it appears doesn't count, so
let go and press again (`link_pairing_confirm_active_session()`,
`tests/test_pairing_fresh_press.py`).

EchoEar-2ST confirms pending pairing with an explicit screen press; hold the
on-screen microphone to record and release it to send once paired. It has no
physical talk/setup button, so the GPIO button reset gesture does not apply.
The VIEWE SmartRing-Plus works the same way; its BOOT button is inside the case.

On boards with the full UI, a test rig confirms a pairing over the serial
console instead of a finger on the button: see Automated setup tests under
musegadgets BLE audio.

A product in the Muse Gadget catalog sets its model code,
`CONFIG_HOMEHUB_BLE_PRODUCT_CODE="SCW1"`, to advertise `MuseGadget-SCW1-XXXXXX`,
XXXXXX then the last three bytes of the Bluetooth address, as the catalog
names products (`CONFIG_HOMEHUB_BLE_NAME_BT_ADDRESS` asks for that tail
without a product code; the build checks the name fits). The ESP32 catalog
boards may keep the Wi-Fi MAC tail with their code instead
(`CONFIG_HOMEHUB_BLE_NAME_TAIL_WIFI_MAC`): the StopWatch advertises
`MuseGadget-M5SW-XXXXXX`, the ESP-Mosaico `MuseGadget-EMO1-XXXXXX`, the
SenseCAP Watcher `MuseGadget-SCW1-XXXXXX` and the Waveshare C6 1.8
`MuseGadget-WC6-XXXXXX` (`c6ble` too); the Waveshare S3 1.75C takes
the Bluetooth address tail, `MuseGadget-W175-XXXXXX`. Otherwise the
device advertises as `MuseGadget-XXXXXX` (`MuseGadget-Disp-XXXXXX` on the
ideaspark, CYD, SenseCAP Indicator, Waveshare S3 LCD 3.5 status-screen, Waveshare S3 ePaper 1.54 and 1.54G, reTerminal E1001, E1002 and E1003, and both reTerminal Sticky overlays, `MuseGadget-ha-voice-XXXXXX` on the
Voice PE, `MuseGadget-respeaker-XXXXXX` on the reSpeaker Lite,
`MuseGadget-xvf3800-XXXXXX` on the reSpeaker XVF3800,
`MuseGadget-Oled-XXXXXX` on the ESP32-C3 OLED, which shows the status as words). Its
node id, the name Muse knows it by, ends in the same six hex digits in
lowercase: setup offers `gadget-XXXXXX` with `CONFIG_MUSE_GADGET_NODE_ID_PREFIX`
(off by default; bench builds turn it on), else `homelink-XXXXXX`, and stores
it with the device tokens. A device keeps its stored id, and one set up by earlier firmware, which
stored none, keeps `homelink-XXXXXX`, until it is set up again
(`main/identity.h`, `tests/test_link_node_id.py`). It uses
**community pairing v5**, so the phone app must support v5
and list community devices. Community pairing needs the explicit device press but has no
manufacturer attestation, and it doesn't stop an active man-in-the-middle.

Until it's paired, a board with the full UI shows that name on its screen, dim under the
state, so you can tell which gadget to pick in the Muse app. A screen too narrow
for the whole name shows the `XXXXXX` tail on its own, and a square 128 px
screen (AIPI Lite) leaves it out, the same as it leaves out the state. Once it's
paired (or has a token set by hand), the name goes, and the mic icon
appears. It's hidden until then, since a press can't reach Muse. On the v1.0
home, while no device token is stored, "Open Muse app" (small, dim) takes the
time's place and `<PRODUCT>-<XXXXXX>` (white) the date line, and the Speaker
and a dimmed Call button stay where they are (the "Not set
up" home). The StopWatch is an exception:
its dedicated microphone, speaker/mute and power rim icons stay visible before
pairing as well, identifying its three physical controls.

A held talk key talks from a dark panel too: the press wakes the screen and, still held 250 ms after the press (`HOLD_MS`, every path: a voice note, a BLE client's turn, push-to-talk in the StopWatch's realtime voice call), starts the turn as on a lit screen; a shorter press only wakes. The threshold counts from the press (`muse_input_event_t.at_us`), so resting codecs that take a while to open never eat it.

Talking: on a board with a talk button and
the home's hold region (StopWatch yellow, ESP-Mosaico AI, Watcher wheel
press, C6 BOOT), Settings > Talk defaults to Button only: a screen touch
never opens the mic. A tap on the idle home, on Muse or anywhere but Speaker
and Call, plays the happy bounce once (the avatar's hop and hearts,
1.6 s; it sends nothing and opens no mic) in place of the old "Hold to talk"
hint; during a turn, a call (a Live call or a realtime voice call) or a setup
prompt a tap does what that screen does.

A swipe that starts on a control is a swipe. Once the finger moves past the slop (`muse_swipe.h`: 12 px on either
axis, the touch slop, or a page or list scrolling with it), the control
under its start gets neither its tap nor its hold, however long the finger
stays down: `muse_ui.c` stops LVGL's clicks and long presses at the indev for
every control (Speaker, list rows, settings controls, buttons), and the
controls that time their own holds ask `muse_ui_touch_swiped()` (the home's
Call button, and its hold region: a drag before 250 ms never becomes a talk
press). Once a talk turn has begun, movement doesn't end it and swipes are off
until the lift, which ends it (once talking, drift is fine). The call's
own touch path (`muse_call.c`, the video's polls too) takes the same slop: a
press past it is no tap.

Muse's own phone setup service (`components/muse/muse_ble.c`, the Wi-Fi
builds with the full UI; Settings > Bluetooth > Phone setup) needs a
passkey-authenticated LE Secure Connections bond (the code on the screen)
for every write. A
release build answers the writes that change the gadget's Wi-Fi or Muse
account (`wifi.*`, `hatch.host`, `hatch.vm`, `hatch.token`) with "not in
this build". A test build (`CONFIG_MUSE_GADGET_TEST_BUILD`, on with
`MUSE_BENCH=1`) takes them, for `tools/muse/ble_setup.html`, only while it
isn't set up (no device token, no Muse token set by hand) or for 10 minutes
after the user turns Phone setup on; once set up they're refused until Reset
pairing (`muse_ble_gate.c`, `tests/test_muse_ble_gate.py`). The USB console
(the one wired origin, `muse_line_origin.h`) takes them in every build with
no window, set up or not: the wired console is fully open, so an automated
run never waits on a person. Every unpair and reset also clears the bonded phones
and the Muse server, VM and device token set by hand
(`tests/test_link_reset_forgets_pairings.py`). The factory-test service
(`main/factory_test.c`) answers only while the gadget isn't set up or the
owner's setup window is open, and on a release build it notifies the
gadget's presses only then (`tests/test_factory_test_access.py`).
Link setup (community pairing v5 and `provision_v2`, with the press) is the
way a phone sets Wi-Fi up.

To skip BLE Wi-Fi provisioning while you iterate, set
`CONFIG_HOMEHUB_WIFI_SSID` and `CONFIG_HOMEHUB_WIFI_PASSWORD` in `menuconfig`.
The device still needs to be paired once for its token.

## Live voice calls (experimental)

`CONFIG_MUSE_REALTIME_VOICE` (off by default, "Live voice calls with Muse"
under Muse in menuconfig) adds calls beside voice notes on boards with Muse's
own voice session: the full-UI boards with PSRAM and the Voice PE (and
reSpeaker Lite) builds. On StopWatch (and the TriviaPOD, which shares its
board file and controls), hold yellow to talk or interrupt, release
to keep the call open, and tap blue to hang up; voice notes are an explicit
Sound setting. The StopWatch's yellow key is its talk button and blue its
speaker button in every build (`board_m5stack_stopwatch.c`): outside a
call blue still mutes, and during one, or once a press that will start one is queued, its press hangs up (`muse_input.c`). A
hang-up that can't be queued moves the input epoch, which ends the call. Other boards retain tap-to-call and hold-for-note. Link-only
builds, the DevKitC-1 default included, don't have the option and don't change.

- The call is a `POST /voice/realtime` over the voice session's Noise
  connection: `components/muse/muse_voice_call_core.c` has the protocol (a
  bounded JSON scanner, the 40 ms appends, the audio decode, the call's
  lifecycle and deadlines) with no IDF in it, and `muse_voice_call.cpp` runs
  it on the session task. `tests/test_muse_voice_call.py` covers both on the
  host with synthetic audio.
- The session connects with `app_id` (`CONFIG_MUSE_REALTIME_VOICE_APP_ID`,
  `hatch-web` by default): the VM takes the app identity from the connection,
  not from a request header, and refuses calls without one. With the option
  on, this applies to everything on that connection, voice notes and typed
  turns included.
- The voice session never sends `link.register`: Link's own control
  connection keeps the device's registration.
- Muse's greeting is off by default (`CONFIG_MUSE_REALTIME_VOICE_GREETING`).
- Every way out of a call sends `POST /voice/end-call` while the connection
  is up: closing the media alone doesn't end a call. Server policy may also
  end a call before the client's own limits. If the connection drops instead, the
  VM keeps the call through a disconnect grace (about 45 s, then up to 35 s
  more) before ending it, and in that window another connection of the same
  account can take the call over; end calls explicitly rather than by
  dropping. So a call whose end wasn't confirmed (the connection dropped
  first, or Muse didn't confirm the end-call) is kept as one pending cleanup:
  its end-call, unchanged, goes again when the session reconnects, before a
  new call starts (which waits for one try, at most 10 s), up to 3 tries and
  for at most 10 minutes; a refused (4xx) end-call isn't retried. It names the old call's id, so it can't end a new
  call, and nothing else of the old call is sent again. A new call's admission
  replaces the old one at Muse, which also ends the cleanup. The pending
  cleanup is in RAM only: if the gadget loses power or restarts first, the
  call stays until Muse's disconnect grace runs out. If Muse doesn't confirm the end-call
  (`{"ok":true,...}`), the session drops its connection once the media is
  down, which makes the VM end the call. The gadget hangs up after
  `CONFIG_MUSE_REALTIME_VOICE_IDLE_S` quiet seconds, and always at
  `CONFIG_MUSE_REALTIME_VOICE_MAX_S`. Held input, a caller mid-sentence (until
  that speech item's own stop) and a turn Muse is still working on aren't idle;
  silent keepalive audio is. Turns are tracked by `submission_message_id` and
  input `item_id`: a `turn_settled` retires its submission and the earlier ones
  for that item, never another item's or a later alias.
- On push-to-talk calls only replies to your latest hold play
  (`muse_vc_reply_gate_t`). The one exception: once that reply has finished,
  older work from before your last press reporting back (a delegated task's
  result, or the outcome after you answer a spoken approval) plays after it,
  with captions. The audio of a reply you cut off with a press never comes
  back, and older work arriving while a newer reply is still going is
  dropped. Approvals work by voice: Muse asks in the call and you hold to
  answer; the gadget has no approval card.
- On boards with the full LVGL UI (the e-paper UI keeps THINKING), a call
  shows WORKING (the thinking avatar's working variant) when nothing is
  playing, the caller isn't holding or talking, and Muse still has a caller
  turn open
  (`hatch.voice.turn_started` without its `turn_settled`; Muse keeps a
  delegating turn open until its result has been spoken). A settlement with
  `async_handoffs_pending` of 1 or more is a conversational boundary, such as
  a spoken approval prompt, where Muse waits on the caller: not WORKING
  (READY on push-to-talk, LISTENING hands-free) until a new caller turn
  starts, even though such a settlement may name none of the caller's turns.
  One reporting 0 retires every open turn, since Muse doesn't
  settle each caller turn by name, except the latest caller turn while its
  reply hasn't finished (an interrupted reply's settlement can arrive after
  the next caller turn started), and that only once. A repeat of an earlier
  settlement changes nothing. Its reply is a response
  whose `response.submission_message_id` names a turn for its item, or one
  that names none (early media the turn adopts, older VMs); older work's
  responses don't count. Only plain digits count: a string, sign, fraction
  or exponent, or no count, changes nothing. `muse_vc_call_working()` decides it; talking,
  Muse speaking and a pending reply still take precedence. On a hands-free
  call it waits for the turn's first reply to finish (before that the call
  shows LISTENING, as it always has), the mic stays open while it shows, and
  speech the server hears brings back LISTENING. The avatar gets
  `muse_pose_t.working`: the default one types on a little keyboard with a
  spinner beside its head in a warm scheme; an avatar that ignores the field
  shows THINKING.
- Nothing plays and the mic doesn't open until `hatch.voice.session` names this
  call's exact `call_id` and a `session_id`; a missing or wrong one fails the
  call. Before that, an `error` event fails it too; after, only `session_dead`
  does.
- Caller and reply text go to captions only, never the log.
- Hands-free calls shut the mic while Muse is audibly talking, plus 300 ms.
  The Voice PE keeps it open (full duplex) when its XMOS reports echo-cancelled
  outputs at start-up.
- The VM must accept calls from the gadget's credentials. A `session_control`
  stop on this call's realtime stream ends the call: the client drains queued
  playback and explicitly ends it. The admission body's second line,
  `MUSE_VC_CAPABILITY_OFFER` (`hatch.voice.capability.offer` with
  `end_call_stop: 1`), tells Muse the gadget ends calls that way, so it can
  hang up when the caller says goodbye; the gadget has no channel for
  `ui.set.voice.end_call`. A VM that doesn't support the offer ignores it and
  sends no stop on goodbye, so a press, the idle limit or the length limit is
  what reliably ends a call there.

## Requests to Muse's VM from your code

Link's control stream, tunnel and agent-name fetch use the VM's Gadget API
(`ServiceType::Gadget`: `/api/gadget/control`, `/api/gadget/tunnel` and
`/api/gadget/me`). The device session over BLE (BLE-only gadgets, through the
phone's relay) keeps its control stream on the daemon's `POST /link-control`.
The requests below go to the VM's daemon.

`CONFIG_HOMEHUB_DAEMON_REQUESTS` (on by default, on every board, Link-only
builds included) lets firmware open its own HTTP requests to the VM's daemon
on Link's Noise session, with `noise_ctrl_req_open()`, `noise_ctrl_req_send()`
and `noise_ctrl_req_cancel()` in `main/noise_control.h`. Muse's own voice notes
and chat use the same calls. Each request gets a stream of its own; a refused,
reset or cancelled request never touches Link's control stream.
`tests/test_link_daemon_requests.py` runs them on the host with the Muse UI off.

Routes a gadget's credentials can reach, besides Link's own and the voice
call's `POST /voice/realtime` and `POST /voice/end-call`:

- `GET /chat/history`: the chat's messages. Ask for one at a time
  (`GET /chat/history?limit=1`) so the response stays under the frame bound
  below. A single message larger than that bound can't be fetched this way.
- `GET /fs/raw/<path>`: a file on the user's VM, as its raw bytes. The path
  is relative to the VM's file root, percent-encoded except for its slashes.
  Ask for it in parts with a `Range: bytes=<first>-<last>` header of at most
  `noise_ctrl_req_max_body()` bytes: the reply is a `206` whose body is that
  range (or a `200` with the whole file), and it has to fit the frame bound
  below. `main/image_fetch.c` (`vm_get_range()`, for `display.show_image`)
  and `main/mg_face.c` (the avatar face, `CONFIG_MUSE_AVATAR_FACE`: off by
  default, on in the BLE gadget overlays; it only reads its own face.jpg)
  read VM files this way.
- `POST /fs/upload`: writes a file, as a `multipart/form-data` body with the
  text fields `path` (required), `overwrite` (`true` or `1` replaces an
  existing file; otherwise it is kept) and `create_parent` (missing parent
  folders are made unless it is `false` or `0`), then the file's bytes in a
  `file` part. Send the body in pieces (below); the `file` part has no size
  cap of its own, and each text field is capped at 64 KB. There's no append:
  a file is written in one upload.
- `GET /identity`: the user's Muse identity, whose avatar the avatar face and
  avatar sync follow (`main/mg_face.c`, `main/avatar_sync.c`).

`POST /fs/read` and `POST /fs/write` are owner-only: a gadget's credentials
get 403 there. There are no list or stat routes for gadgets.

Keep within the session's bounds:

- Up to 3 requests at once; another is refused (its callback gets -1).
- Send a body in pieces of at most 8 KB (4 KB on Muse boards without PSRAM
  and the CYD, 2 KB on the Cardputer ADV, AtomS3 and AI Passport): a larger piece ends
  that request.
  Queued body bytes are capped at 16 KB with PSRAM and 4 KB without, and
  `noise_ctrl_req_send()` waits up to `wait_ms` for room.
- Each response frame has to fit the session's inbound buffer (12 KB, 17 KB on
  Muse boards without PSRAM other than the AtomS3 and AI Passport, and the CYD). A larger one ends the whole session,
  which reconnects, and the request with `NOISE_CTRL_REQ_TOO_LARGE`. Ask for
  responses that stay under it: read large files in ranges
  (`noise_ctrl_req_max_body()` bytes each).
- The callback runs on the session task: copy what you need and return. Do
  slow work, and any request that depends on its result, in a task of your
  own, as `on_ws_command()`'s asynchronous commands do.

## Slow replies

On a board with the full UI and Muse's own voice session (PSRAM), a voice-note
turn whose request Muse took and still says it's working on (`agent.status` /
`task.status`) after 20 s adds a notice to the turn ("Muse is working on it.
I'll tell you when it's done."), lets go of the turn so you can ask something
else, and remembers the request. If Muse isn't working on it, the turn waits
and fails with NO REPLY FROM MUSE as before. Turns on the Voice PE and
reSpeaker builds, whose voice task doesn't play late replies, wait as before
too. Streamed pieces name no request, so the late reply is known by its
finished `message.assistant` row, whose `reply_to_message_id` names the
request. With no turn running, that row is held until the voice task is idle
(`muse_hatch_incoming()`), then played as a turn of its own that wakes the
screen. Reply text may be in `display_text`, `content`, or canonical
`transcript.messages[].content[]` text blocks; from a transcript, only the
message's own assistant text is used, without commentary. This applies to
replies during a turn and late replies.
Messages the turn already played aren't announced again, and rows that
arrive together play in one turn. The request is forgotten once its reply
plays, 10 s after Muse stops saying it's working, or 30 minutes after it was
remembered. A battery board resting when the reply comes plays it when it
wakes, if its connection survived. A late reply that arrives during another
turn follows that turn's usual rules, and only one request is remembered at a
time.

## Video from a phone

`MUSE_VIDEO=1 tools/muse/board.sh build|flash <board>` (overlay
`devices/sdkconfig.muse-video`) adds video streaming (`CONFIG_MUSE_VIDEO`,
`CONFIG_MUSE_VIDEO_WIFI`): Muse Gadgets Test (`../android/musegadgetstest` for
Android, `../ios/musegadgetstest` for iPhone) on a phone on the same Wi-Fi streams
its camera, or a clip, as H.264 or VP8
(`CONFIG_MUSE_VIDEO_VP8`, on by default), and the board decodes it and
draws it full screen. `../protocols/VIDEO.md` has
the design, the Wi-Fi protocol and the numbers.

- **Boards:** those whose panel driver has `draw_frame` and that have PSRAM:
  the Waveshare S3 1.75C (tested) and 1.75, the StopWatch, the
  ESP-Mosaico and the SenseCAP Watcher (over BLE). Another QSPI panel driven
  by `boards/muse_lcd_bands.c` gets it with `.draw_frame =
  muse_lcd_bands_frame` and its panel gap (`muse_lcd_bands_set_gap`);
  `muse_lcd_bands_set_te` if the TE line is wired. A panel that takes
  windows only on whole groups of columns says so in `.frame_x_align`
  (the Watcher's SPD2010: 4).
- **Round panels cover the circle:** on a board with
  `muse_board_t.round` (the Watcher, the StopWatch, the Waveshare S3 1.75C)
  a camera picture is two times upscaled and drawn over the whole
  panel, centered: the square's corners, which the round mask hides, take
  the crop, so no black column or row is left inside the circle. A picture
  whose 2x falls short of the panel either way (an app still sending the old
  200 or 232, a non-square or smaller picture) is cover-scaled by the fill
  instead (nearest pixel, any scale, the overflow cropped).
- **No black bars:** a
  camera picture covers the whole screen on every board, whatever
  size or aspect the app sends: the two times cover or the plain one to one
  / two times placement where that already reaches the panel both ways,
  else the fill's cover-scale (`muse_video_fit.c`; the fill takes up to 512
  px wide, `MUSE_YUV_FILL_MAX_SRC`). `tests/test_muse_video_fit.py` sweeps every video
  board with 128x158, 158x128, 240x240, 320x240, 240x320, 480x480,
  176x144 and more (up to each board's max, as the stream's start allows).
  Where the fill can't take the picture (the S31's two-band 480 pictures in a
  non-square aspect, or wider than the fill), the plain placement is drawn
  with its edge rows and columns extended to the panel's edges
  (`fit.extend`, `fill_extend` in `muse_video.c`). UI streams are outside
  the rule (one to one by design). `muse_video_fit.c` chooses the window (x 0,
  the panel's width, on whole `frame_x_align` columns) and the crop, and
  converts two source rows at a time with the SIMD kernel into a block of
  four 2x rows (3.8 KB, main SRAM while 32 KB stays free past it, else
  PSRAM; freed when the video tasks retire), copied out cropped. The
  suggested size there is the smallest multiple of 16 at least half the
  panel: 208 on the Watcher's 412 (2 px a side cropped), 240 on the 466 px
  panels (7 a side). Square panels (the ESP-Mosaico, 480 at one to one) and
  UI streams (one to one) are placed as before. `tests/test_muse_video_fit.py`
  renders each round board's picture and checks the circle, the centering,
  the window's alignment, the suggestions and the square panels.
- **Pieces:** `../xplat/tinyh264` and `../xplat/libvpx_vp8` (the
  decoders, behind `muse_vdec.c`; `components/tinyh264` and
  `components/libvpx_vp8` build them),
  `components/muse_vstream` (the receive ring and frame format any transport
  shares), `components/muse/muse_video.c` (decode and show tasks, pacing,
  bands, the view), `muse_video_fit.c` (where the picture goes: fit, or a
  round panel's cover), `muse_yuv*.c/.S` (colour conversion and scaling),
  `muse_video_wifi.c` (TCP on port 7766, mDNS `_musevideo._tcp`) and
  `muse_video_wire.c` (its messages). A new transport fills a
  `muse_video_transport_t` and calls `muse_video_begin()`.
- **Using it:** Phone camera in Settings (touch boards) or the menu opens the
  view and asks the phone for a stream; any button closes it. The console's
  `>video` / `>video=0` do the same. The board takes a stream only while the
  view is open: the port is unauthenticated (`../protocols/VIDEO.md`, Trust).
  `CONFIG_MUSE_VIDEO_WIFI_START_ANYTIME` lifts that for automated benches;
  never ship it. The port is announced over mDNS only while the view is
  open, and refused starts pause further ones (1 s doubling to 30 s,
  `muse_video_wifi_gate.c`, `test_video_untrusted.WifiPortGate`). Stats every 2 s on `muse_video`: decode time,
  received-to-shown, kb/s, shown/dropped/skipped, each core's load; and,
  while a video stream (not a UI stream) runs, one line a second for A/V checks
  (`muse_video_tick.h`): `av video: t=MS span=MS rx=N shown=N drop=N skip=N
  decerr=N fps=N int=FREE low=LOWEST` (frames taken from the ring, drawn,
  dropped, decoded but not drawn, refused by the decoder; internal RAM free
  now and its lowest since boot).
- **Stats overlay on the call video:** five taps
  on the call video (each within 400 ms of the last) show it: the first tap
  still toggles the controls, taps 2-4 do nothing, the fifth hides the
  controls; a tap on a shown control button is the button's (quick + / -
  presses never count). One tap hides it (consumed); it starts hidden in
  every call. No Developer mode. Refreshed once a second, fixed-width
  (unscii 16, or 8 under 300 px), in a half-dark box at the top, clear of the
  mic ring (the subtitles band and the controls are at the bottom): codec and
  coded > shown size; video kb/s and pictures shown over the last second;
  decode errors, drops and keyframe requests; the link interval and mode;
  the reply depth and underruns (call totals, from the call's start; a field
  a board can't measure is left out, never 0). It reads the counters
  `muse_video` (`muse_video_stats`) and mg.play / the link
  (`mg_ble_call_stats`) already keep, renders a 1-bit picture of the box on
  the input task (double-buffered), and the show task blends it into each
  frame's pieces like the subtitles band: no redraw, no wait, nothing on the
  audio path (`muse_call.c`; rules in `muse_call_stats.c`,
  `test_muse_call_stats`). Console word (an automation word): `>vstats`
  toggles it, `>vstats=1` / `>vstats=0`, `>vstats?` reports (`@vstats on|off`);
  while it's up the same lines print once a second on the USB console as
  `@vstats a | b | ...`, never on the Nordic UART.
- **Changed-only redraw:** on
  by default (every `muse_lcd_bands` board). `>vdirty=0|1` either way,
  RAM only. The `vdirty:` lines log internal free / largest when its block
  is taken and at the stream's end. On a
  board whose panel keeps its own memory and that has
  `muse_board_t.draw_windows` + `panel_writes` (every
  `muse_lcd_bands` board: the StopWatch, the S3 1.75C, the Watcher, the
  ESP-Mosaico), a streamed
  picture goes to the panel as windows over only what changed since the
  picture the panel holds; the panel's memory keeps the rest
  (`muse_vdirty.c`, the pure part; `vd_*` in `muse_video.c`). What changed:
  each macroblock's 64-bit digest of its Y, U and V against the last
  picture's (`muse_vdirty_digest`, in the decode task, before the picture is
  done), which sees the decoded pixels, so deblocking, VideoToolbox's coded
  but unchanged blocks and skips with a motion vector are exact; where the
  PSRAM decoder's own map (`muse_vdec_same`) applies, those macroblocks
  aren't hashed. Pictures skipped by the show task fold into the next one
  drawn. Where it lands: the output is cut into cells at the pixels where
  the scaler's source macroblock changes, on the panel's alignment
  (`muse_vdirty_axis`), and windows are whole cells. Overlays (subtitles,
  call controls, the stats box, the bench mark) give their rectangles and a
  gen (`muse_subs_overlay_rect`, `muse_video_set_overlay_rects`): when a gen
  changes, where it draws now and where it drew are redrawn; unchanged, it's
  drawn into whatever window crosses it. A repeated or held picture with no
  overlay change writes nothing. Adaptive: after 2 s of pictures drawn whole
  because most of them changed (a phone camera's video can change most
  macroblocks), the digests stop and every picture is drawn whole at no
  cost until a keyframe or a retry (4 s, doubling to 16 s while it keeps
  failing); `>vdirty?` says `(idle ...)`. With `>vdirty=0` nothing is
  allocated. Whole picture: a keyframe, a failed decode,
  any other write to the panel (LVGL, a still, the ring's or dot's repaint,
  a piece given up: the board's `panel_writes` count), a mic ring change, a
  new place or size, windows over 80 % of the picture or more than 24.
  Scales: the fill (round panels' cover-scale, the Mosaico's
  non-square pictures), the round panels' two times cover (windows at any
  column, the source columns they show converted a pair at a time:
  `muse_video_cover_fill_win`), two times (windows on whole fours) and one
  to one; a round panel's cells wholly outside its circle are never pushed
  again. `muse_lcd_bands_windows` writes each window as its own CASET /
  RASET window, in pieces of whole fours of rows, and holds each group of
  touching windows for the panel's scan (TE boards: the W175, the Mosaico),
  with its own `video window group` tearing
  line. Two bands, UI streams (their own changed rectangles, `muse_ui_fb`),
  3/2 and the scaled path draw whole. RAM: one block a stream (logged as
  `vdirty: N bytes`), taken at the first picture after the decoder
  and its picture, freed with the decoder; under 100 B static. Console (automation words, RAM only): `>vdirty=0|1` (A/B),
  `>vdirty.tune=COST,PCT,MAX`, `>vdirty?`, and **`>vdirty.shadow=N`, the
  check a release needs first (>= 300 real-stream pictures, 0
  different):** after each of the next N windowed pictures the picture is
  converted whole with the overlays, the ring and the dot and compared with
  a per-cell digest of everything the panel was sent (no frame copy);
  differences are logged with their cells and pixels, and
  the compares after an overlay, ring or size change are tagged with it
  (`after controls off`). A 2 s `vdirty:` line on `muse_video`: pictures
  windowed / unchanged / whole and why, MBs changed, cells and pixels drawn,
  windows per picture, decode, digest, convert and panel write ms (windowed
  / whole), the shadow so far. `tests/test_muse_vdirty.py` checks the
  digests on real streams, the cells and the windows.
- **Radio:** while a stream is on, Wi-Fi stays out of modem sleep and BLE
  stops advertising (`muse_wifi_keep_awake`, `muse_link_ble_quiet`): with BLE
  advertising, the radio's scheduler gives BLE half of every beacon period
  and the stream's packets wait out that half (50 ms round trips instead of
  15, and the phone's bitrate falls to its floor).
- **Power management:** the S3 video boards
  keep `CONFIG_PM_ENABLE` and tickless idle (their battery's screen-off
  light sleep needs them); with the screen on, `muse_input.c` runs the CPU
  at its full clock with light sleep off, streams included. On the W175
  (s3-video, a Mac client's H.264 240x240/30, vdirty on, 3 runs each) PM costs
  little: panel write 10.5-10.9 / 22.8-23.1 ms (windowed / whole) with it,
  10.3-10.4 / 22.3-22.5 without, 0 dropped either way, same core load. A
  stream-scoped `ESP_PM_CPU_FREQ_MAX` + `ESP_PM_NO_LIGHT_SLEEP` lock changed
  nothing (10.7-10.8 / 22.9-23.0), nor did `PM_PROFILING` off with the PM
  idle code in IRAM (10.6-10.8 / 22.5-22.7), so neither is in the tree. The
  ESP-Mosaico's 25-30 % (34-35 vs 25-29 ms a picture) is the S31's own;
  it has no PM.
- **Memory:** the ring (128 KB) and the decoders' memory are in PSRAM, and
  the decode tasks' stacks in main SRAM (`MALLOC_CAP_DMA`): the S31 also
  hands out LP RAM as heap once main SRAM runs low, several times slower, so
  keep anything per-frame out of plain internal `malloc`. The decode and show
  tasks take about 9 KB of internal RAM once the first stream starts (they
  stay for good), and the second band's task 6 KB more once a two-band stream
  first asks for it. The decoders' two small per-macroblock blocks go in main
  SRAM only while 32 KB stays free past them, else PSRAM (no measurable cost:
  30.7 ms for a small UI P frame either way). Each stream logs `memory
  decoding` and `memory after the stream` on `muse_video` (free, largest,
  lowest ever): a StopWatch soak of 10 UI streams with reconnects, touches,
  HID maps and two-band streams held at 24.2-24.4 KB free throughout.
  `CONFIG_MUSE_VIDEO_DECODER_IRAM` (off) puts the decoders' hot code in IRAM:
  44 KB (and 33 KB for VP8; `CONFIG_MUSE_VP8_IRAM` picks less or more) that a
  Wi-Fi + BLE + UI build on the S3 can't spare (LVGL's task then fails to
  start).
- **Test** with `tests/test_muse_video.py` (the ring and the Wi-Fi messages,
  and the wiring under the options), `tests/test_video_untrusted.py`
  (hostile sizes, the port's guards, every band seam and scale drawn with
  guard bytes after each piece, every converter row written exactly),
  `tests/test_tinyh264_pie.py` and
  `tests/test_libvpx_vp8.py`; on a board, with Muse Gadgets Test (Android:
  `adb logcat -s MuseCam`; iPhone: Xcode's console).
- **Frame cropping:** an H.264 picture is its
  SPS's frame cropping window, all four offsets (crop units of 2 x 2 for the
  4:2:0 frames tinyh264 takes; `muse_h264_crop_window()` has the general
  rule): `muse_vdec.c` hands the show task that window (planes from its top
  left, its width and height, decode progress in its rows), and the show
  task draws it. start_streaming's width and height are the picture after
  cropping: a cropped picture larger than them in whole macroblocks is
  refused, a cropped SPS is never refused for its padding (the coded size
  may be up to 2 macroblocks a side larger, and at least 192 x 96, the
  iPhone encoder's floor: an iPhone asked for 112 codes 192x112 and crops),
  and a picture that differs from them is shown (at most the stream's size
  from its top left, or a smaller one whole) and logged once a stream
  (`pictures are 112x96 after cropping, the stream said 112x112`). The
  "unchanged macroblocks" map comes only with no left or top offset (the
  grid would shift). `tests/test_video_crop.py` decodes real VideoToolbox
  captures (`vt_112_crop.264`: coded 192x112, right 80 px; `vt_80_crop.264`:
  right and bottom) and the 112 one with left and top offsets, byte for byte
  against ffmpeg and the coded pictures.
- **A call's pictures stopping:** a
  live call's picture stays through a blip and, once pictures have stopped
  for 2.5 s with the stream open (`STALL_US`), Muse animates in its place;
  the call screen adds "No picture from the phone" 5 s after the last
  picture, and the call goes on. The client's video stop_streaming while the
  call goes on (an app giving up on video) holds the last picture 1 s after
  the stop (`CALL_STOP_HOLD_US`; unless the mic ring changes: the UI takes
  the screen to draw it, the decoder's pictures being gone), then Muse; the
  line comes 5 s after the stop (`muse_video_call_stalled_since()` returns
  the stop's time for a stopped stream, else the last picture's). A size
  change (stop, then start_streaming at the new size with a keyframe, under
  1 s) shows the new size with no face, black or calling screen in between.
  The call keeps its own picture clock across its streams
  (`s_call_shown_t`): a new start after the stop goes back to the picture
  clock's stall (Muse 2.5 s after the last picture) and keeps the stop as
  the line's reference until a picture comes, so a silent restart gets Muse
  and then the line 5 s after the stop (line after Muse either way).
- **Untrusted input:** everything from the network is. Keep the checks in
  `../protocols/VIDEO.md`'s Trust section when changing the transport, the
  decoders' glue (`muse_vdec.c`) or the drawing (`fill()` in `muse_video.c`).
  The show task draws while the bands decode, so the drawing can't rely on
  the decode task's checks: each converter (`muse_yuv.h`) writes exactly
  the rows it's given, and two bands meet on the converter's row unit
  (`muse_video_band_seam()`).
- **Recovery (a keyframe repairs the decoder, whatever came before):**
  each frame decodes on its own. A picture tinyh264 couldn't
  finish (an error, slices missing) is dropped before the next frame
  (`h264bsdAbandonPicture()`), never concealed and returned as that frame's
  picture: it was, and the frame went undecoded, so after one damaged frame
  (a push-to-talk barge-in's stray reply audio in the ring) every requested
  keyframe "decoded" in 2-4 ms and the next frame failed, for 22 s on a
  Watcher. A keyframe that decodes keeps the stream's SPS and PPS and locks
  its size and profile; after an error they come back
  (`h264bsdRestoreParamSets()`), parameter sets for another size or profile
  are refused, and other ones with the stream's ids only beside a keyframe.
  A lost frame is never decoded around: a jump in the frame header's number
  (`muse_vgate.h`) or in a slice's frame_num (tinyh264, gaps not allowed)
  waits for a keyframe instead of a smeared picture, and an access unit
  with no picture that isn't parameter sets alone is an error. The decode
  task's gate (`muse_vgate.h`): a decode error, a keyframe that made no
  picture, a ring gap or a frame number skipped waits for a keyframe, and
  request_keyframe goes at once on the first one after a picture, then at
  most every 300 ms. `tests/test_video_route.py` replays the barge-in through
  `mg_proto.c`, the ring, the gate and the real decoder on a real clip
  (`tests/vectors/h264/testsrc2_208.264`), every picture checked byte for byte:
  no stray byte in the ring, a damaged frame repaired by the next keyframe,
  damaged parameter sets that don't stick, a frame lost before or after
  numbering repaired the same way, under ASan and UBSan.

## musegadgets BLE audio

**Experimental:** BLE gadget support is experimental while the Muse apps
gate it.

`components/muse_gadget_ble` implements the musegadgets BLE protocol
(`../protocols/mgcommands.h`): a phone, browser or gateway connects over BLE,
turns on push-to-talk, and gets each utterance as SBC (the default) or LC3
(10 ms frames, 16-96 kb/s; the client's start_mic rate and bitrate, settings
13 and 14 (kept on the device with setting 7, reset by a factory reset), and the capture list are as in
`../protocols/mgcommands.h`; captures are 16 kHz until a board's mic
runs at 24 kHz, `mg_proto_config_t.capture_24k`).
Its `README.md` covers the design. Include the protocol header; never copy
its constants.

- **Turn it on** with a board's gadget overlay, loaded after its own
  (`devices/sdkconfig.muse-waveshare-s3-175c-gadget`, the Watcher's, the
  C6's BLE-only one): `CONFIG_MUSE_GADGET_BLE_AUDIO=y` with both watchdogs on
  (`cmake/validate_config.cmake` refuses BLE audio without them), ATT MTU
  247 and first-boot volume 50. Off, nothing changes. It needs a board with a
  mic to be useful: the full-UI boards with PSRAM, and the Voice PE.
- **Options:** the routing policy (`CONFIG_MUSE_GADGET_BLE_ROUTE_AUTO`,
  `_BLE_ONLY`, `_WIFI_ONLY`), `CONFIG_MUSE_GADGET_BLE_LC3` (on everywhere:
  Google's floating-point liblc3 where the chip has
  an FPU, the fixed-point `../xplat/lc3fx` on the C3, C5 and C6 and on the
  ESP-Mosaico, `CONFIG_MUSE_GADGET_BLE_LC3_FIXED`; where its code and tables
  live, `CONFIG_MUSE_GADGET_BLE_LC3_PLACE_*`: all in internal RAM on the
  C6's BLE-only profile, 0.54 ms to encode and 0.29 ms to decode a 10 ms
  frame there),
  `CONFIG_MUSE_GADGET_BLE_QUEUE` (offline clips; needs the `mg_queue`
  partition in `partitions_muse.csv`; cleared when the pairing changes hands,
  at a setup reset and at a new setup's commit, with the passcode and the
  face: `passcode_forget()` in `main/app.c`, `mg_ble_queue_forget()`; a
  commit marks them first, `mg_q_forget`, so boot erases them if a power cut
  came between; a rollback keeps them; the log goes at once, then every
  sector of the partition that isn't blank is erased in the background, an
  NVS mark `q_wipe` kept until done and a boot resuming it, no clips taken
  meanwhile; on a set-up gadget `clip_info` / `read_clip` need this
  connection's proof and `status` shows no clips before it), and
  `CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS`.
- **It shares Link's BLE server** (`main/ble_server.c`) as a companion, like
  Muse's phone setup: keep `ble_server_set_companion()` to two companions, and
  keep Link's setup advertising and `ble_server_full_shutdown()` behaviour
  unchanged for builds without it.
- **Setup and device tokens** go through Link's BLE setup, not mgcommands
  (`../protocols/README.md`, Gadget setup over Muse Link). Gadget builds add
  `wifi` (`"optional"`, or `"none"` BLE-only) and `mgcommands: 1` to
  `device_info` (`link_pairing_set_gadget()`); `provision_v2` without an ssid
  is token-only: the pair in Link's `access_token` / `refresh_token`, the
  token proof key `mg_proof_k` (hex), `setup_wifi=skipped` and
  `setup_complete`, all in one commit under the provisioning session
  (`commit_setup()` in `main/app.c`), `auth_ok` only after it, then Link
  setup advertising stops while musegadgets advertising goes on. A Wi-Fi
  setup on a gadget stores the key too, in the same commit, and sends
  `auth_ok` after it as well. Boot recovery keeps a token-only setup; reset
  (hold, the Muse menu, or the token proof's clear) erases all of it. Never
  log any characters of the tokens or the key: whether there's one, its length
  and a SHA-256 prefix (`token_fp_text()`) at most. Without an SDK token the gadget
  still pairs, logs a warning at `pairing_confirmed`, and the app shows its
  "SDK token missing" error. Builds without gadget support behave as before
  (no `wifi` field, Wi-Fi required).
- **Gadget setup v2** (`../protocols/README.md`): Link setup's manufacturer
  data flags byte sets bit 1 (0x02, a Muse Gadget over Bluetooth) on every
  gadget build (`link_pairing_mgcommands()`; Wi-Fi builds keep 0x00 / 0x01).
  A set-up gadget can be set up again at any time: the setup service stays,
  `pairing_client_hello` is answered (Wi-Fi builds still say
  `error_pairing_unavailable`), with the same handshake. ONE press confirms
  a setup: a tap on the prompt's on-screen Confirm (touch boards: "Pair with
  Muse", "Tap Confirm to pair"; a generous hit area, the control and a tenth
  of the screen's shorter side around it; a press and its release on it, so
  one that slides off confirms nothing; on the e-paper boards, which draw
  no control, the whole screen), a press of a second key (the aux key,
  Enter on a keyboard) or, for a first setup, the talk key (the Link button
  on boards without the full UI, taken at its down edge,
  `button_set_confirm()`): no hold and no minimum or maximum press time, but
  a key or touch that went down before the prompt showed (armed at its first
  frame on a screen) never confirms it, not even at its release; the next
  press does (an order check with no timing, `muse_link_prompt_seq()`). A
  scripted or bench key press confirms as a real one does (on release builds
  the console words that change state stay refused over Bluetooth); on a
  release build a scripted or bench touch on the Confirm control never does,
  over any link (`touch_confirms()`), and neither does a Nordic UART line's
  `key=`: during a setup or re-setup prompt it answers `real press needed`. **The talk key never confirms a
  re-setup** on a set-up gadget's release build: its press while the prompt is up only
  dismisses it ("Not now", no talk turn; `app_confirm_pairing_press()`'s
  `talk_key`, `muse_link_confirm_press()` vs `muse_link_confirm_key_press()`).
  A board whose only key is its talk key (no touch screen, no menu keys, no
  aux key: `muse_glue_talk_key_only()`; every gadget build without the Muse
  UI) takes a re-setup only in **setup mode**: the key pressed at power-on
  (seen up first, within the 3 s SAFE entry window) and let go before the
  10 s SAFE hold, on a normal boot (`muse_bootguard_setup_press()`); it
  lasts until the confirmed re-setup or 5 min after that boot
  (`setup_mode_now()`, `main/resetup_gap.h`), and there the talk key
  confirms. SAFE of either kind is never setup mode, and no gadget takes a
  re-setup in SAFE (`app_resetup_open()`). Outside it a re-setup hello gets
  `error_pairing_unavailable` in plaintext and nothing shows. Test and bench
  builds are always in setup mode. The prompt has no timer on a gadget
  build: it stays until the press, a dismiss, or the phone giving up
  (`link_pairing.c` `CONFIRM_ONE_PRESS`; Home Link keeps its press window).
  While it is up the key belongs to it (no talk turn starts). A tap
  anywhere but Confirm does nothing; the back gesture (a swipe on a touch
  board, Escape on a keyboard) ends it, "Not now"
  (`app_dismiss_pairing_prompt()`: `pairing_confirm_timeout`, then the
  session's disconnect; the phone can start it again). **The re-setup
  prompt's rate**, per connection (`resetup_hello()`): a hello on the
  prompt's own connection takes it over; another connection's gets
  `error_operation_in_progress` in plaintext while it is up; and on a
  release build a hello on a connection within 10 s of its dismiss is
  refused the same way (test and bench builds are exempt from that rule
  only). Known limit: the re-setup prompt is limited per connection, so a
  nearby device can raise it again by reconnecting; the talk key never
  confirms it, so the owner can only dismiss it. The bench's
  `>pair.confirm` and auto-confirm press as the key does
  (`muse_link_bench_confirm()`). The
  provisioning first keeps the old setup aside (`config_reprovision_begin()`:
  the tokens, the key, the Wi-Fi-skipped marker, username, endpoints,
  `vm_url` and the SDK token hint copied to `rp_*`, then the marker
  `rp_open`); the commit's last write erases `rp_open`. A failure before
  that (`setup_fail_for_session()`) copies the old setup back and the gadget
  stays set up and connected; boot does the same after a power loss
  (`config_reprovision_recover()`), so it never holds parts of two setups.
  After the commit `mg_glue_setup_changed()` drops a token proof made with
  the old key, and on Muse boards the VM session reconnects with the new
  pair. A committed re-setup is an owner change: the last owner's passcode,
  face and offline clips go (`passcode_forget()`), and so do their apps,
  the Muse server, VM and token set by hand, and the earlier phones' bonds
  (`owner_change_forget()`; the phone that just set the gadget up keeps its
  own, `ble_server_forget_bonds_but_connected()`). The owner change's mark
  (`owner_wipe`, written before the commit point) is retired only once the
  face record's erase, the passcode store and the server's erase are
  verified (`owner_wipe_retire()`); until then the next boot does the
  cleanup again, every bond included. A failed setup erases the mark with
  its rollback: the owner who keeps the gadget keeps all of it. The session that committed is used up (`s_consumed_session`):
  another `provision_v2` on it gets `error_pairing_confirm_required`, so a
  new setup takes a new hello and press; a failed attempt's session may
  retry. Re-provisioning ignores `ota_url` (logged; `ble_server.c`). An abandoned setup on a set-up
  gadget puts its screen or light back as it was (no setup prompt left on);
  a phone that only reads `device_info` doesn't change it.
- **Token proof:** `mg_command_token_proof` (34) on every gadget build
  (`mg_token_proof.c`, PSA HMAC): the client proves it holds the key setup
  left, without sending the token. It gates push-to-talk's enable, the
  device session's hello and mic and camera output (below): `clear`
  after a match resets setup and restarts the board into Link setup
  advertising. The sealed clear's answer (the erase's own result) goes out
  first: the restart waits until it's out, the link has gone, or 5 s passed
  (`CLEAR_ANSWER_WAIT_MS`), and credit keeps counting while the erase
  runs. A new setup's commit ends a match made with the
  old key. A challenge on a link (an iOS relaunch
  can keep the link up) clears that connection's push-to-talk enable: a
  live turn ends, and no mic output leaves the gadget before VALID;
  the mic may capture into a hold before the proof
  (below: a press is a held live turn on a set-up gadget that can hold it,
  gesture-only otherwise); an enable sent between the challenge
  and the result counts at the result; the capture codec choice stays.
  Keyed on the challenge, not request_status. Logged on `mg.ble`
  (`push-to-talk off: a new token proof ...`); `test_mg_ble`
  (`test_proof_clears_ptt`). A challenge is served at once:
  its answer goes ahead of everything else on
  Control (a capture's held burst, the session's data, any reply; their
  order otherwise kept); one the stack refuses waits at the front
  (`mg_proto_t.proof_out`, retried by every pump and before any other
  Control message), and if it hasn't left in 1 s (`MG_PROOF_ANSWER_MS`) it
  gives way to `mg_command_error` (token_proof, busy) so the client asks
  again at once; the exchange is reset with it, so no stale answer follows
  (`test_proof_answer_first`).
- **Stored settings need the proof:** on a set-up gadget a `set_settings`
  that names a setting the gadget keeps in NVS (the buzz, `audio_codec`,
  `audio_queue_enabled`, `speaker_volume`, the capture rate and bitrate) is
  answered `proof_required` and nothing in it applies, unless this
  connection's proof matched; per-connection settings (push-to-talk, haptics,
  link mode, client app) and reads stay open. A gadget that isn't set up is
  open; a set-up one (its setup marker set or unreadable) whose K is missing
  or unreadable has no owner: no proof can match, so these stay refused.
  `set_unix_time` sets the time from any connection, but a time before
  2024-01-01T00:00:00Z (`MG_CLOCK_FLOOR_MS`) is refused with `invalid_value`
  and the clock keeps its time; its UTC offset and 12/24-hour flag reach NVS
  only where a stored setting would be taken (RAM only otherwise). UI input's `hid_map` (kept in NVS) and power off and the
  offline queue's erase follow the same rule (`mg_proto_owner_ok()`;
  `test_mg_ble`).
- **Mic and camera output only to a proven central:** on every gadget
  build (`cfg.output_proof`, set
  by `mg_ble.c` whether or not the board holds a proof key) the capture's
  outbox sends nothing of a capture (change_data_type, Data, the gestures and
  stop_mic queued behind it) to a central that hasn't passed this
  connection's token proof. A capture before the proof (a talk press right at
  connect, a client's start_mic, a Live call's talk hold) is held whole, the
  bench guard's size (`MG_CAPTURE_GUARD_S`, 16 s of its format, in PSRAM;
  without PSRAM, the C6, up to `CONFIG_MUSE_GADGET_BLE_CAPTURE_GUARD_INTERNAL_KB`
  (32 KB: 4.3 s of 60 kb/s SBC, 8.1 s of 32 kb/s LC3) of internal RAM in one
  allocation at the capture's start, only while 32 KB stays free past it,
  never per frame), and goes in order right after the proof's result once it
  matches. A hold that fills refuses the next frame: the capture stops (the
  mic ring with it) and what's held is kept; nothing more starts until it's
  sent. A wrong confirm, a challenge on a gadget with no key, a reset that
  leaves none, or the link's end discards it unsent and ends the capture. A
  re-proof's challenge holds output from the challenge on.
  **Staleness bound** (`MG_OUTPUT_STALE_MS`): held
  output not released within 3 s of the capture's end (or of a re-proof's
  challenge, for a capture already over) is discarded, logged and counted
  (checked at every pump, the worker looking every 20 ms while a hold is
  armed, and before any release). **A client's start_mic before the
  proof** opens the mic only while a token-proof challenge is outstanding
  on this connection (its output held); otherwise it's answered
  `proof_required` and the mic stays closed. With no proof key (not set up)
  no capture starts for a central, and `start_mic` and `read_clip` are
  answered `proof_required`; a saved clip waits in the queue for a proven
  central. **Nothing about a held capture reaches an unproven central**,
  neither its format nor that one exists:
  request_status reports the data type from before it (the capture's own
  change_data_type goes only at release or go-live, `mg_proto_capture_visible()`);
  the Nordic UART's `status` line shows `live=0` and only `output:
  unproven`; the capture doesn't move the link (no "push-to-talk capture"
  activity, no link_mode report). On a release build of a set-up gadget the
  Nordic UART log mirror goes only to a connection whose proof matched (a
  new link waits for its own), except the answer to an identity or status
  read (`status`, `version`, `guard?`, `ota?`), which goes to the asking
  connection; test, bench and automation builds mirror to any connection.
  The gadget's own screen and mic ring stay as they are. The bench switch
  lifts the output hold. **Streams to a set-up gadget** need the proof too:
  audio playback's and video's `start_streaming` answer `proof_required`
  from an unproven connection (automation and test builds: also with the
  bench override on); `haptic_play` does on a release build. Control and the
  session stay open; data past the granted credit is dropped and counted. Camera output (`camera.capture`, the
  Watcher with `CONFIG_MUSE_WATCHER_CAMERA`) goes over the device session,
  whose hello needs the proof; on BLE-session builds `main/app.c` checks
  `mg_ble_output_ok()` at the request and again before the frame goes. The
  bench switch `bench.out=1|0` (automation; `bench.out?` a diagnostic; RAM
  only, off at every boot, never stored) lets a Mac bench client without a
  proof key receive output; on, a held capture goes at once. On the USB
  console it's kept across connections. **Over the Nordic UART** the
  same words switch it for that
  connection only (`mg_proto_set_output_bench_conn`): off at every boot
  and every connect, ended by the disconnect, one `mg.ble` line for each on
  and off naming the connection (`bench.out on (Nordic UART, connection N,
  handle H) ...`, `bench.out off (...): bench.out=0` / `: disconnected`).
  While either switch is on for the connected central the gadget shows
  **"Bench"** (amber, the small type) in the page dots' place on every
  screen LVGL draws (the home and its talk screen, the calling screen, the
  call's face; one line above the lock glyph on the locked face; LVGL's
  top layer, `muse_home_bench_tick`), drawn into a call's video frames too
  (`bench_overlay` in `muse_call.c`), and a board with no screen gives a
  blue double flash 30/100/30 ms every 4 s on its status light at the
  lowest priority (the shared LED table: only over a steady
  connection color, never over a voice state, the volume or a blinking or
  breathing state), an e-paper one "Bench - " before its status text
  (`led_status_set_bench`).
  `tools/mg_ble_client.py --mic-bench {esp32,xn54}` sends
  that board's bench word on the Nordic UART right after connect, before
  the first command, and `--bench-word CMD` (repeatable) any word
  (`test_mg_ble_client`'s `test_mic_bench_words`). Both need a bench build
  (`MUSE_BENCH=1`) or a proven connection: a release build answers a
  state-changing word from an unproven one `locked`.
  Logged on `mg.ble` (`capture held for the token proof: proof matched, N
  bytes held go first` / `no valid proof, N bytes discarded unsent`) and
  `mg.voice`; the BLE console's `status` adds `output: unproven|proven|bench
  held= full= fills= releases= discards= discarded= stale= stale_bytes=
  no_key= start_refused= held_turns live= queued=` (counts only).
  **Held live turn:** on a set-up gadget a talk press while a client's link is up
  but the client isn't proven or ready yet (the connect burst: no proof, not
  subscribed, or push-to-talk not enabled) routes `MG_ROUTE_HELD`:
  `mg_voice_held_begin()` holds the turn
  before its button_down, so its gestures, its SBC 16 kHz audio (the clip
  format, whatever audio_codec says; its own change_data_type announces SBC,
  and both apps decode by the announced type) and its end
  all wait. Once the client is proven (or the bench switch is on) and ready
  (subscribed, push-to-talk enabled) it goes live with everything held, in
  order. Not within 3 s of the press (`MG_HELD_TURN_MS`), or the link drops,
  the held audio starts an offline clip and the turn goes on into it (the
  voice task), or is saved whole (the worker, for a turn already over). The
  choices are one switch each in `mg_config.h`: a failed proof discards it
  (`MG_HELD_TURN_FAIL_QUEUES 0`), a dropped link queues it
  (`MG_HELD_TURN_DROP_QUEUES 1`), no queue at the deadline (setting off, no
  partition, no room) keeps holding under the rule above, staleness bound
  included: discarded 3 s after its capture's end
  (`MG_HELD_TURN_NO_QUEUE_HOLDS 1`), held turns record SBC
  (`MG_HELD_TURN_SBC 1`). Only on a board that can hold it: a clip
  queue, or a hold of at least 2 s of its SBC (`MG_HELD_TURN_MIN_HOLD_MS`,
  `mg_ble_can_hold()`: PSRAM, or the C6's internal cap with 32 KB kept free,
  checked at the press); otherwise the burst's press stays gesture-only with
  "Connecting". Logged on `mg.voice` and `mg.ble` (`held live
  turn: ...`). A gadget not set up keeps "Open Muse app"; with no link a
  press still goes to the queue as before.
  `test_mg_ble` (mg_proto_harness's `test_output_proof` and
  `test_held_turn`, and `test_output_proof_wiring`).
- **The Control reply outbox** (added after one more request_status
  line overran a gadget's 5-reply reserve): a one-shot Control reply the
  stack refuses (request_status's lines, a call start, settings, errors,
  status reports) waits whole in `mg_proto_t.ctlq` (`MG_CTLQ_MAX` 16
  records in `MG_CTLQ_RING` 1024 bytes), in order, behind the token proof
  answer, and goes as the stack takes it (every pump; the worker looks every
  5 ms while one waits). Sized for the worst request_status (11 lines:
  change_data_type, the command list, `10 FE 89`, start_mic's codecs,
  change_data_type's formats, settings, device actions, UI input, a pending
  video request, a pending call start, `10 FE 85`) plus the proof result and
  spare; one that finds no room is counted (`ctlq.dropped`, never expected).
  **Reply sizes** (a gadget's reply slot once silently dropped
  an over-long capabilities reply): every reply the firmware builds is at
  most `MG_CTL_REPLY_MAX` (`MG_MIN_ATT_MTU` - 3, 97 B), one notification at
  the protocol's MTU floor. Each typed list's maximum with every option on
  is a `MG_REPLY_*_MAX` in `mg_proto.h` (stream_video's capabilities sized
  for still, though no ESP32 build lists it), and each
  buffer has a `_Static_assert` beside it; capability counts are counted as
  the entries go, never by hand. A get_settings answer goes in pieces of at
  most min(MTU - 3, 97), each an answer of its own (at most 8, else
  invalid_length). **Never cut short:** NimBLE truncates a notification over
  MTU - 3 without a word, so `raw()` refuses one (`link_refused`, never
  expected); a reply longer than MTU - 3 (before the exchange the MTU is
  23) waits whole in the outbox, with everything after it in order, until
  the MTU rises (`held_mtu`); the token proof answer (50 B) likewise. The
  counts (outbox drops, MTU holds, length refusals, commands past the
  command queue, capture controls past the capture outbox, device session
  answers dropped, touch events refused) are on the BLE console's `status`
  (`replies: ...`) and the USB `status` (`"replies":{...}`), counts only.
  `test_mg_ble` (`test_reply_sizes`: every list at its maximum; `test_reply_mtu_hold`; `test_reply_outbox_whole`). What a
  refusal makes stale goes now or not at all (`send_control_now()`: stream
  and video credit, touch events), and the capture's outbox and the
  session's ports keep their own queues. `test_mg_ble`
  (`test_reply_outbox`: request_status with the stack refusing for 500 ms
  while a call start and a proof challenge arrive: the proof answer first,
  then every line in order). **Replies never dropped** (mgcommands.h
  ordering; a gadget's mic error under request_status bursts): while the
  outbox holds anything, a client's Control command that answers through it
  waits as a command (`mg_cmdq_t`, 192 B, in order) and runs once the outbox
  is empty, so one command's answer always fits; a token proof and the
  device session's messages never wait. A live turn's own frames
  (change_data_type, Data, stop_mic) pass queued replies, and button_down /
  button_up go at once when no capture control is queued; 2 notification
  slots stay free while capturing (data_room; the stack's own keep is 4
  msys blocks). `test_ptt_ordering`: request_status bursts during talk
  holds on a ready link, a slow link (audio held at the release) and a
  4-buffer stack: every frame, every reply in order, the gestures at once.
- **Security draws**: every key, nonce or passkey an
  attacker must not predict (Link setup's ephemeral key and device nonce,
  the token proof's nonce, the phone setup passkey,
  Noise's keys) comes from `secure_random()` (`components/secure_random`):
  `esp_fill_random()` is a true RNG only with BT or Wi-Fi on, so without a
  radio the draw is refused and the operation with it, never drawn from a
  weaker source; `tests/test_secure_random.py` lists the draws that aren't
  security ones.
- **Device session:** `mg_command_session` (0x89) on BLE-only builds
  (`CONFIG_MUSE_GADGET_BLE_SESSION`, on with the standalone option): after a
  matching token proof the gadget hands the client its credential, sealed
  under K and that proof's nonces (the hello's sealed
  flag, no bond needed; one sealed session per proof, a second sealed hello
  gets `proof_required`; request_status lists `[10 FE 89 caps]`, sealed and
  client_refreshes, after the command list), and only sealed: an unsealed
  hello gets `insecure` on any link, bonded or not (a relayed proof on the
  relay's own bond would get the credentials in the clear);
  no Security Request; the client runs the gadget's own
  Noise session to its VM, and
  `main/noise_control.cpp` drives it with the code a Wi-Fi gadget uses
  (`link.register`, `link.invoke` and `link.result`, `noise_ctrl_req_*`
  streams), sending ServiceRequestEnvelopes through `mg_ble_session_*` instead
  of TLS. Its shared senders (`send_json_body_chunk`, `control_tx_step`,
  `req_pump_tx`) are templates over the session type: keep Wi-Fi's
  `ClientSession` path unchanged. The seal is `../portable/mg_device_session`'s,
  built from there unchanged (`mg_seal_crypto.c` is its crypto glue): change
  it there first. Link setup stores node_id with K and the pair in one
  commit (every gadget setup, with a proof key or without), and that stored
  id is the one in use (`identity_node_id()`): a new setup offers
  `gadget-XXXXXX` (`identity_setup_node_id()`, in device_info,
  the transcript, K and a setup's mint) with `CONFIG_MUSE_GADGET_NODE_ID_PREFIX`
  (default n: off by default, bench builds turn it on in
  `devices/sdkconfig.muse-bench`; off it offers `homelink-XXXXXX` as before),
  a gadget set up earlier keeps its
  `homelink-XXXXXX` until a setup commits (a re-setup included; a failed or
  abandoned one keeps it), a gadget holding credentials but no stored id
  (older firmware) stays `homelink-XXXXXX`, and a plain Home Link build
  keeps `homelink-XXXXXX` (`test_link_node_id`). Never rename a stored id.
  credentials_update (a flat object; unknown members ignored)
  replaces the pair and its `access_expires_at` in one commit through the
  re-provisioning journal (`config_set_token_pair()`, K and node_id kept),
  answered credentials_saved after that commit, sealed in a sealed session
  whose hello carried `mg_session_hello_flag_sealed_saved` (echoed:
  `[1 || SHA-256(refresh_token)[0:8]]` or `[0]`, the
  next device-to-client counter, the one credentials and data share; a
  write-back given up on while the link is up is a sealed `[0]`, never
  silence; a plain session's stays one byte, and so does a sealed session
  without the flag, a client that predates 5.7 (an older iOS app, the
  bench app): exactly as before 5.7, no counter taken, give-ups as before;
  `test_mg_session` replays `sealed_credentials_saved` and `sealed_data_large`
  for a flagged client and `sealed_data` for an unflagged one),
  handed back last in the next credentials (layer 2's `expires()` /
  `commit_expires()` on Wi-Fi gadgets; a setup keeps none, and a Wi-Fi
  gadget's own refresh keeps now + the response's `expires_in` while its
  wall clock is set, else the response's `Date` + `expires_in`, else none); a client re-proves after any refusal and never seals again
  under a counter it has already sent;
  reset and the token proof's clear erase node_id with the rest. Wi-Fi
  gadget builds (BLE audio without the standalone option) run a
  credentials-only device session instead: `../portable/mg_device_session`'s
  layer 2 under `mg_proto.c` (`cfg.creds_session`), hello, sealed
  credentials and credentials_update only, caps `[10 FE 89 caps]` with
  sealed, and client_refreshes only while the gadget holds no Wi-Fi
  credentials (set up token-only: the app refreshes its pair; with Wi-Fi it
  refreshes its own; asked at each request_status), the same
  store. On BLE-only builds the session's command dispatch (`on_ws_command`)
  leaves the Wi-Fi-only commands out (the VM commands, discover,
  `display.draw_url`), so the Wi-Fi/IP/TLS stack stays out of the image. The
  Muse apps relay the session both ways, so what the server sends that
  nothing handles (an unknown frame kind, a frame on a stream the session
  doesn't run, an unknown control message type) is logged on `mg.session`
  (mirrored on the Nordic UART) by type and size, never content, accepted
  and credited, and a malformed message is refused the same way; neither
  ends the session (`ble_session_inbound()`). `link.register` and its reply
  are logged there too, and `>session.ping` (or `!IN session.ping`) sends
  one `link.heartbeat` on an open, registered session. In a sealed session
  data is sealed too, both ways (mgcommands.h, Sealed data): one counter
  per direction shared with the credentials and updates, sealed whole at
  `mg_session_send()`, the counter checked at the first fragment and the
  tag at the last with layer 1's incremental tag, then decrypted in place
  in `mg_session_take()` (no message buffer); a refused tag doesn't use its
  counter, an abort does (with data and an update in flight together, a
  refused earlier one stays open once: layer 1's `mgseal_rx_*`, shared with
  layer 2); unsealed data or updates get `insecure`. The
  component README (Device session) has the details; `test_mg_session`
  replays `../protocols/test-vectors/mg-session-v1.json` and runs the sealed
  exchange against `mg_session_seal_ref.py`, and `test_mg_session_seal` the
  seal vectors on the host and PSA crypto.
- **link.register's family** (`main/register_family.h`,
  `../protocols/README.md` Node family): every build of this SDK
  registers `device_family` `"gadget"` (BLE-only, Wi-Fi + BLE and Wi-Fi
  only). `model_id` is the board's overlay id, `CONFIG_MUSE_DEVICE_MODEL_ID="muse-gadget-<board>"` on the
  line after the overlay's target (profiles of one board share it; the
  overlay-less C5 DevKitC-1 keeps `muse-gadget`); the catalog code (`SCW1`,
  `M5SW`, `EMO1`, `WC6`, `W175`) stays in the advertised name only
  (`tests/test_session_image.py`). **`metadata.model_name`**
  (every gadget with a device session) is the board's short product name from its overlay,
  `CONFIG_HOMEHUB_MODEL_NAME`: `StopWatch`, `Watcher`, `Mosaico`, `C6`,
  `W175` (`register_model_name()`; none set, none sent), so Muse can name the
  gadget in hand; `test_session_image` keeps the register under its 8 KB with
  it.
- **A use case taught to Muse through the device session alone**
  (`CONFIG_MUSE_SESSION_IMAGE`, default y on BLE-only builds with the full
  UI; `main/session_image.h`): the gadget registers
  `display.show_image` (`path` of a JPEG on the VM, `duration_ms` 1000-60000,
  default 5000, 0 until dismissed) whose description alone tells Muse when to use it and what
  to make (a baseline JPEG of the panel's size, quality ~70, under 30 KB,
  subject centered on a round panel). The gadget reads the file over its
  session (`image_fetch_start_vm()`, GET `/fs/raw` in ranges, with the link
  in active mode while it downloads), shows it over everything but the mic
  ring, answers once it's on screen (`shown_for_ms`, size, `download_ms`),
  and `muse_ui_image_hide_after()` takes it down from the frame tick after
  `duration_ms` (a tap, a talk press or a Live call's start sooner; a call
  refuses it as `busy`). Settings > Developer > Tests > **Ask Muse for an
  image** (the Tests row shows whenever the gadget has a test) asks Muse in
  plain words on the gadget's own session (`POST /chat/stream`,
  `{"message":"Please make a random picture ... on my gadget <BLE name> for
  5 seconds.","output_modality":"text"}`), naming no command, and shows the
  run under the button (Asking Muse... N s, Showing it, Shown with the times,
  or No picture: why; 120 s without a picture is a failure). Neither the
  phone app nor the server knows about any of it: the apps relay the
  session's bytes. Console words (USB and `!IN`): `img.ask` (the press) and
  `img.show=PATH[,MS]` (a VM file shown without Muse) are automation,
  `img?` (the last run's state and marks in ms from the press) a
  diagnostic, all logged on `mg.image`; `dev?` / `dev=1|0` read and set
  Developer mode. BLE-only builds don't register
  `display.draw_url` (no network of their own); with the test they don't
  register `display.draw_file` either (it still answers): with both there,
  Muse kept using `draw_file`, which had worked earlier in the same chat,
  and timed the 5 s itself with `display.show_animation`. Without
  the test, `draw_file`'s description says it all.
  `tests/test_session_image.py` runs the core, link.register both ways and
  the wiring. Measured on the Watcher through a Galaxy S22 (without
  active mode): Muse found `display.draw_file` from link.register alone and
  generated a 47 KB 412x275 JPEG; message to invoke 41 s, download 13.2 s.
  With active mode: 55 KB in 3.5 s (15 ms interval).
- **The device session only for an active task** (only for sessions
  through a phone relay, so this is the BLE-only
  builds' device session; a Wi-Fi gadget's own session to the VM stays
  always open). A task is active while there's a user action (a talk
  press, a key or a tap; not the screen lit alone: `mg_ble_link_user()` ->
  `noise_ctrl_user_action()`, 2 min),
  traffic owed (results to send, requests queued, waiting or open,
  link.register unanswered or a control message on its way), or a wait for
  something not yet received (`noise_ctrl_task_hold()`: the avatar face's
  owed look, the picture test's ask; at most 15 min).
  Its start, with the link up and no session, asks for one at once; while
  it's active an open session gets an empty BodyChunk on the existing
  control stream (/link-control) after 15 s with nothing sent
  (`NOISE_CTRL_KEEPALIVE_MS`, under iOS's ~30 s suspension; never a new
  POST); its end sends nothing more: no keepalive, no ask (a waiting ask is
  dropped silently). An open session left with no task is closed by the
  gadget itself `NOISE_CTRL_IDLE_CLOSE_MS` (20 s) after the last task ended,
  with open_request [0] (the gadget-asked close both apps drain), once per
  session; a task within those 20 s keeps and reuses it (the
  bench app never closes one, and a silent open session
  holds Android's VM socket and its 30 s pings and keeps the VM awake).
  Agent invokes then get node_offline until the next session.
  Logged on `mg.session` at INFO (`task active (...)`, `task ended: ...`,
  `keepalive: ...` every 15 s, `no task for 20 s: closing the session`).
  (On one gadget the old stay-online rule
  gave 136 reopens in 8 h.) **Never a re-check of what the gadget has**, on
  every build, Wi-Fi included: the avatar face looks once at boot, at the
  face's selection, at setup (its restart) and on face.set, never per open
  or on a timer, picture or not (the W175's 404 every 60 s); a failed look
  again at the next open (`mg_face_core.h`, `face_sched_t`). It never asks
  Muse to make the picture. Nothing else re-fetches
  periodically: the picture test waits for its one invoke (2 min), and
  link.register goes once per session.
- **The device session asks again while a task is active, and requests
  wait for it** (mgcommands.h: the client never reopens on its own, and iOS
  suspends the Muse app in the background and ends its sockets at each
  Bluetooth wake, so the relay reports closed every ~30 s on the Watcher).
  After any session state the gadget didn't ask for (no_network, or closed)
  while none is open and a task is active, it sends open_request [1] at
  once, then with backoff, only while the link stays connected (an ask the
  link's end overtook is dropped); with no task it doesn't ask. **The next
  ask is set when one goes** (the cores once stopped
  after the first timed ask while the client stayed silent): with the
  client silent, no_network's series asks at 0, 1, 2, 4, 8, 15, 15 ... s and
  closed's up to 60 s; a state the client reports replaces the ask set
  that way with its own step. A failed open
  counts the same (exchange 14): connecting, then closed or
  no_network with no session ever open, reaches `noise_control.cpp` (the
  session code reports every state, `mg_session.c` `on_state()`), so after
  an open_request whose open fails the next ask is that kind's next step,
  never nothing (that closed was dropped before, and the gadget went
  silent until a hello, a saved update or the link's end). Two series, neither
  restarting the other (mgcommands.h):
  after closed (the server's end or refusal) 1, 2, 4 ... s capped at 60 s;
  after no_network (the phone's network, or iOS killing the socket) 1, 2,
  4 ... s capped at 15 s; an ask of one kind still waiting doesn't hold the
  other back (whichever comes due first asks). A task starting with no
  session open asks as the next of the ended kind's series: at once for that
  series' first ask, else at its next step after its last ask (the other
  series is untouched; `ble_task_update()`). A series starts over only
  after a session that stayed open 60 s (`BLE_ASK_STABLE_MS`, from the
  client's open report to the state that ends it; then both do); after a
  shorter one the same kind of end continues it, so a server that accepts
  and ends at once is never asked faster (mgcommands.h;
  vector exchange 8 step 7: no_network after a short
  reopen starts its own series, at once). A hello answered restarts both and
  drops an ask still waiting; a credentials_update saved restarts both, and
  saved while the session has ended (since the hello the client reported a
  state other than open or connecting) asks at once while a task is active
  (with none it's only saved: a token refresh on an idle gadget opens no
  session), whether or not an ask
  waits, as the first of the ended kind's new series: new credentials may
  fix what ended it, even auth_failed (mgcommands.h, exchange 8
  step 12; saved while open or before any state, nothing more; one not
  saved changes nothing; `mg_ble_session_reask_marks()`, ordered around each
  state by the marks noted when it was reported, as `mg_session_ref.py` and
  the Zephyr side); and so does the link's end; never after
  auth_failed, not_enrolled or too_large, which also drop an ask still
  waiting, nor after its own open_request [0] (a failed session's close,
  reopened by its own reconnect policy); connecting changes nothing
  (`noise_control.cpp`, `s_ble_ask_ms[]`; every BLE-only board: the Watcher,
  StopWatch, C6 and ESP-Mosaico; a phone can answer no_network while the
  handed-off token refreshes and never retry, which would leave the gadget
  unregistered). A request the gadget makes while no session is open
  (`noise_ctrl_req_open()`: the picture test's chat POST, an image fetch)
  waits for one instead of failing: queued in order (three at most), the
  client asked at once, sent once the session is open and link.register
  acknowledged, and failed after 10 s (`NOISE_CTRL_REQ_WAIT_OPEN_MS`) with
  `NOISE_CTRL_REQ_NO_SESSION` (the picture test then says "Muse isn't
  connected yet ... press again"); a session that ends before link.register
  is acknowledged keeps them waiting. An invoke's result for a session that
  ended with such an unasked state, made within 10 s, goes in the next
  session (not after a failure, too_large or auth_failed). A GET or HEAD
  request in flight when the session ends that way, on a link still up,
  with no response bytes yet (the phone's relay) goes back to the
  wait-for-open queue once, in its place, its 10 s counted from its open,
  and is re-sent after the reopen and the register ack (each such stream
  keeps its Open and body, up to `REQ_REPLAY_MAX`, until the first response
  bytes, `req_resendable()`); one that got bytes, a second such end, or any
  other method (a POST such as img.ask's) fails as before, and its caller
  retries (img.ask: "press again"). Logged on
  `mg.session` (`request N waits for the session`, `requests that waited for
  the session go now`, `request N re-sent after the session's unasked
  end`, `N request(s) waited 10 s for the session and
  failed`, `N invoke result(s) from the last session go in this one`).
  `noise_ctrl_ble_request_open()` asks at once (the picture test's press).
  `test_mg_session`'s gadget harness runs the backoff, the cancels, the
  link's end, the waiting requests (order, body, after the register ack),
  the 10 s failure and the carried result, and replays `mg-session-v1.json`
  through this code byte for byte (exchange 8 included); its `reask` mode
  runs the series across sessions: ten 5 s sessions a server ends cleanly
  (at once, then 1, 2, 4 ... 60 s, never at once), ten no_network ends (at
  once, then up to 15 s), neither restarting the other, 59.999 s
  continuing, 60 s and 61 s restarting both, the two kinds overlapping, a
  credentials_update (saved after an end, even auth_failed and in the same
  pass: at once; right after a hello: nothing more; not saved: nothing) and
  a hello (in the same pass as the next state: that state asks at once)
  restarting, and failed opens (open_request, connecting, closed or
  no_network with none open, repeated: each arms the series' next step, a
  failure while an ask waits leaves it standing); those modes run with a
  task always active. Its `task` mode runs the active-task rule: no task (no keepalive, no
  ask after closed), a user action (asks at once), the keepalive (every 15 s,
  no new POST), a silent client after no_network and after closed, the
  task's end (no keepalive, no ask; the gadget's close 20 s later, once), a
  task within the 20 s (no close, the session reused), a hold keeping it
  open, a new action after the close (one ask), owed traffic and the wait
  holds.
- **BLE only:** `CONFIG_MUSE_GADGET_BLE_STANDALONE=y` makes a board a
  musegadgets gadget and nothing else: `app_run()` hands over to
  `run_ble_standalone()` before Wi-Fi, so Wi-Fi, the Muse session, the
  tunnel and Wi-Fi OTA never start (BLE updates do), and push-to-talk goes to the client or the
  offline queue (policy BLE only). Nor are they linked: the few calls that
  live code still makes toward them (the Muse session's TLS, the Wi-Fi power
  save, the settings' Wi-Fi scan and VM lookup) are compiled out under the
  option, so lwIP, the Wi-Fi driver, TLS, HTTP and the CA bundle stay out of
  the image (about 340 KB less on the StopWatch and Watcher, 450 KB on
  the C6, 540 KB on the ESP-Mosaico, and internal RAM back: 2 KB on the S3
  boards, 25 KB on the C6, 5.7 KB on the ESP-Mosaico). Keep it that way: a
  new call into Wi-Fi, lwIP or TLS from code a BLE-only build runs needs the
  same `#if`, or the stack comes back. Link setup runs without Wi-Fi: until the
  gadget is set up (the setup marker and the proof key; anything less is
  wiped at boot) it advertises setup, taking turns with the musegadgets UUID
  from the same address, and accepts community pairing v5 (the talk button
  confirms), token-only `provision_v2`, `device_info` and `unpair`; set up,
  it advertises musegadgets only and is set up again on the same service. The
  Waveshare C6 has a profile for it: `tools/muse/board.sh build c6ble`
  (`devices/sdkconfig.muse-waveshare-c6-18-ble`, SBC and lc3fx LC3). A healthy boot
  logs `musegadgets BLE ready` and `BLE-only musegadgets gadget`, and the
  heartbeat shows the setup stage and free internal RAM.
- **Playback:** boards with a speaker (the boards with the full UI) offer
  `mg_command_stream_audio`: SBC, PCM and (with LC3 on) LC3 from the client,
  at 8 to 48 kHz resampled to 16 kHz (LC3 at 16 and 24 kHz, 16000 listed
  first), into a 16 KB ring
  (`CONFIG_MUSE_GADGET_BLE_PLAY_BUFFER`) with buffer_update flow control.
  Live reply audio (at most ~100 ms of depth): playout starts
  at 30 ms held (`MG_PLAY_PREBUFFER_US`), in 10 ms chunks, and never
  re-buffers. A reply that starts while the link isn't active (on the
  Watcher a chat reply on a ready
  link played with 6 underruns at 30 ms) starts at 150 ms held
  (`MG_PLAY_SLOW_START_US`), or at its end of data when the clip is shorter
  (stop_streaming keep, or no data for 100 ms), and at 30 ms once active is
  granted (`mg_play_t.slow_link`, from `mg_linkpol_link_active`); a Live
  call's stream keeps 30 ms (the call holds active); start_streaming asks
  for active before its first buffer_update (`ops.stream_starting`).
  Adaptive start (`mg_play_t.start_us`): per
  connection, a reply outside a Live call that underran in its first second
  of audio makes the next one start 20 ms higher, up to the live depth's
  floor (70 ms); each clean reply (no underrun) eases it back 10 ms, never
  below 30; a reply with only later underruns leaves it; a new connection
  resets it to 30. Judged at the next stream's start, logged `mg.play: start
  threshold 50 ms (early underrun last reply)` (`clean reply`); early
  underruns still count as underruns, and the `av play end` line adds
  `early=N`. `test_mg_play` runs it. A gap is concealed (LC3's packet loss concealment for its
  first 60 ms, a faded repeat of the last 10 ms for SBC and PCM, then
  silence) and the next frame plays at once (`mg_dec_conceal`). Every
  board with musegadgets playback (the StopWatch, Watcher, ESP-Mosaico,
  Waveshare S3 1.75C and C6 1.8) makes its duplex I2S 3 x 160 frames
  (30 ms each way, `MUSE_AUDIO_DMA_LIVE_*` in `muse_audio_dma.h`, set in the
  channel config and `muse_board_t.i2s_dma_desc` / `i2s_dma_frames` alike;
  the mic reads 20 ms at a time, so capture keeps a buffer of slack). The
  Waveshare boards' BSPs take no channel config (`bsp_audio_init()` would
  leave ESP-IDF's 6 x 240, 90 ms), so their `audio_init` makes the channels
  itself and sets the codecs up as the BSP does (the 1.75C's ES7210 on MIC1
  and MIC2: two slots of plain I2S, not TDM), in the format `muse_audio`
  opens (`i2s_ready_at_init`). Drift trim
  (`mg_trim_*` in `mg_play.h`): the playback task measures the stream's
  steady depth (the ring + what it holds, over the 2 s after the first
  second: the target) and, when the output clock runs off the reply's rate,
  holds it there: more than 40 ms above the target for 2 s drops one
  sample in 1000, more than a third of the troughs' headroom below it (40 ms
  at most; not starved) for 2 s repeats one, each spread over 32 samples by
  linear interpolation (no click); a trim still losing ground doubles its
  rate every 500 ms up to 0.4 %, halves once back at the target, and stops
  at 0.1 % within 10 ms of it. On `mg.play`: `trim target 67 ms`, `trim
  -0.1% (depth 130 ms)`, `trim off (depth 99 ms)` (depths as queued ms,
  DMA included). With a live depth (below) the target follows the in-flight
  allowance and never sits above the depth plus it (`mg_trim_target`).
  `test_mg_play` plays 60 s replies into outputs 0.1 and
  0.3 % slow and fast (within 50 ms of the target; a 0.3 % fast one with
  BLE jitter conceals one gap before the trim catches up) and a correct
  one (never trims). A reply stream's buffer_update
  goes every 10 ms of
  playout while its credit changes only while a live depth is honored or a
  start before active is pending (`mg_play_t.live_us`, `slow_link`: a
  pending notification keeps the peripheral listening at every connection
  event despite its slave latency), once the last one has left the stack
  (NimBLE's NOTIFY_TX on Control, `mg_proto_control_sent`; 40 ms fallback):
  at most one per connection event (the worker looks every 5 ms while a
  stream plays). On the whole-buffer path with the link active it goes on
  change only (a gadget's AV60 bench test: fixed 15 ms audio updates held the
  notify slot and made video credit late 5x as often, with no audio gain):
  once 1 KB of room was freed since the last (`MG_PLAY_UPDATE_FREED`), or
  50 ms on with any change (`MG_PLAY_UPDATE_CHANGE_MS`), never while any
  Control notification is still in the stack, and the worker runs video
  credit first, so when both are due video takes the slot. At least every
  250 ms either way. `test_mg_ble`'s av credit case: a whole-buffer reply
  beside 300 kb/s video with 3 KB in flight, no video credit stall over
  40 ms (no longer than video alone) and 0 reply underruns. Live depth (mgcommands.h parameter 6 and capability 7)
  is the client's choice: the
  stream_audio capabilities list `live_depth` (uint16, the smallest depth
  held: the 30 ms start + the decoder's 10 ms chunk + the board's I2S DMA
  depth, 70 ms; `mg_play_live_floor_us`), and only a stream whose
  start_streaming carries parameter 6 (uint16 ms; 0 or over 1000:
  `invalid_value`) is capped. Without it the credit is the whole ring, in a
  call too. With it the device holds queued ms at max(asked, floor), and
  audio in flight doesn't count against it (~70 ms in flight left
  ~20 ms on the device under a queued + in-flight cap): the credit
  (`mg_play_credit`) is depth - queued + what it sees in flight (the time
  from a buffer_update that raised the client's allowed total to the first
  byte past the total before it, smoothed, at most 250 ms:
  `mg_proto_t.flight_us`, kept per connection) + the in-flight allowance
  (`mg_play_t.live_allow_us`: 0, +20 ms per underrun to 40 ms, -5 ms per
  5 s of audio played clean; kept across streams, reset at connect; logged
  `live depth: in-flight allowance 20 ms (underrun)`). Writes already past
  it are kept, not credited again until the depth falls. start_streaming's
  entries carry no length, so an unknown parameter id ends the list and
  what came before applies (defaults for the rest), never `unsupported`
  (`test_mg_ble`: a client writing 20 ms only when credit allows, a 15 ms
  link, 0-30 ms of jitter: without parameter 6 the ring fills, in a call or
  not; with 100 ms, 60 s: 0 underruns, queued ~88 ms, one update per
  connection event; two forced outages grow the allowance to 40 ms and it
  shrinks back to 0; 50 ms more stack delay: one underrun while it learns,
  ~88 ms held, where counting in flight against the depth underran 109
  times; and on a modeled ready link,
  slave latency 5 skipped unless a notification is pending, the 7.7 KB
  reply written in 65 ms with active granted after 0.9 s: 0 underruns). One reply stream per Live call (mgcommands.h
  "Live reply audio"): a stream opened in a call
  (`mg_play_t.keep_open`) stays open however long it runs empty (silence
  after the concealment fade, never the 10 s end, which stays for streams
  outside a call), plays the next reply's first frame at the next chunk
  (10 ms + the 30 ms DMA), and ends on stop_streaming or a disconnect
  (mgcommands.h: the device ends a call's stream only when it
  can't go on; after the call the 10 s end of a starved stream applies
  again). The device's own sound in a call (a chirp, the Sound page's or
  bench tests, a saved note: `mg_proto_claim_audio`) pauses the call's
  stream and resumes it after: nothing flushed or lost,
  Data and credits go on, a start meanwhile is taken and waits
  (`mg_proto_play_held`; `mg.play: paused for the device's sound (N ms
  kept)` / `resumed ...`); it pauses rather than ducks, since no board mixes
  two sources in its output path. Outside a call the sound still ends a
  reply. A talk press in the call flushes it
  instead of ending it (`mg_proto_claim_turn`: the same as
  `mg_stream_audio_flush`, a buffer_update sent; the next reply waits for
  the turn and plays after it). A gap of 400 ms or more (`MG_PLAY_IDLE_US`) is a
  reply boundary, not an underrun: an underrun is counted when audio comes
  back sooner, at the gap's start on the stream's clock. The speaking face,
  the panel hold and "playback over" follow audio playing
  (`mg_voice_audible()`, `mg_play_audible`: held, or decoded audio played in
  the last 300 ms), not the stream open (`mg_voice_playing()`, which keeps the
  codecs open). `mg_stream_audio_flush` (6, capability flush = 6, listed):
  barge-in, applied where Control is handled, in order with Data: the ring
  goes at once, the playback task fades its FIFO's first 5 ms out, drops the
  rest and restarts its decoder (nothing concealed), the stream and its
  counters stay, and the answer is a buffer_update with queued ms 0; logged
  `mg.play: flush (N ms discarded)`. **A flush resyncs the decoder**:
  Data reaches the ring only as whole frames (a write
  that isn't is refused whole), so no flush leaves part of a frame behind; the
  restart is the whole decoder (LC3 with its concealment state, SBC, the
  resampler) and runs before any frame written after the flush decodes, so
  the next reply decodes bit-identical to a fresh decoder; a flush that lands
  during a speaker write isn't undone by that write's pending report
  (`test_mg_play`'s flush cases: SBC, PCM and LC3 at 16 and 24 kHz, liblc3
  and lc3fx, with a stale-decoder control). Repaying gaps: each
  underrun's concealed gap adds its length to the reply's delay for good, so
  the drift trim's machinery owes it (`mg_trim_owe`) and drops one sample in
  240 (~0.4 %) while the stream holds 20 ms or more, until it's made up; a
  reply boundary or a flush clears it; logged `repay 80 ms` / `repaid`, and
  the end line counts `repaid=N` (`test_mg_play`: an 80 ms gap at 20 s and
  40 s on a 40 ms link, 60 s: the delay is back before the next gap and never
  drifts up; without it, +80 ms each). Every audio buffer_update carries queued ms after its
  counters (mgcommands.h, A/V sync: the ring + what the playback task holds
  + the board's DMA depth, `mg_play_queued_ms()`; 0xffff when unknown), and
  video frames still show as soon as decoded (the app aligns).
  The reply goes quiet at the talk press's touch-down
  (`mg_proto_barge_pause`), not at the hold threshold, so the ~250 ms of
  pre-roll recorded before the turn is claimed holds none of the Muse's own
  words: the playback task holds it as for a turn (ring and decoded FIFO
  kept, Data and credits going on, no underrun counted; a client's stop or
  flush still applies), a tap resumes it from the sample it paused at
  (`mg.play: paused at the talk press (N ms kept)` / `resumed after the talk
  press`), and the hold threshold claims the turn as below; a pause nobody
  ends lapses after 1.5 s (`test_mg_ble`'s barge pause, `test_muse_ptt_ui`'s
  touch-down cases). The touch-down also silences what the speaker's I2S
  DMA already held (~30 ms, which played into the pre-roll: +5.1 dB over
  the room in a barge-in's first 100 ms at volume 30): the playback
  task calls `speaker_clear` (`muse_audio_speaker_clear`,
  `components/muse/muse_audio_tx.h`), which fades the DMA's buffers to
  zero in place 2 ms past the read position over 4 ms (no click; the I2S
  bus, which clocks the mic in full duplex, never stops) and hands back the
  samples that were there unplayed; a tap's resume writes them first,
  ramped in over 5 ms (`mg_play_ramp_in`), so no sample is lost,
  and a hold's claim, a client's flush or drop
  discards them (`paused at the talk press (N ms kept, M ms back from the
  speaker)`; `test_muse_audio_tx` against a model of ESP-IDF's TX driver,
  `test_muse_spk_dsp`'s barge-in through the chain, `test_mg_ble`'s barge DMA clear). Every board
  with a speaker attaches its TX channel
  (`muse_audio_attach_tx`, before `i2s_channel_enable`). Push-to-talk ends a stream with stop_streaming drop (in a Live call it
  flushes the call's stream instead, above). The client reads and
  sets the Muse speaker level with the `speaker_volume` setting (0 = off:
  playback is then silent, the stream still paced).
  **The ESP-Mosaico's speaker**: its NS4150B clips at a DAC output of about -5
  dBFS, so its volume curve tops at DAC -5 dB (hw_gain 0 dB: volume 100 ->
  ES8311 0x32 = 0xB5, 50 -> 0x83 = -30 dB, 0.5 dB a step, 0 = mute), and
  everything it plays goes through a speech loudness chain
  (`muse_board_t.speaker_speech_chain`, `components/muse/muse_spk_dsp.c`):
  a 4th-order Butterworth high-pass at 700 Hz, a compressor (peak detector,
  hard knee at -30 dBFS, 4:1, attack 3 ms, release 120 ms, makeup +20 dB) and
  a -1 dBFS limiter (2 ms look-ahead, 50 ms release) with a final clamp:
  DAC -6 dBFS at volume 100 at most (64.7 dBA clean against the old map's
  60.3 at the clip edge, UMIK-1 at ~17 cm). The player runs its own instance
  where the boost would go (`mg_chain.h`: reply chunks, silence while it
  still holds audio so a reply's last 2 ms play, emptied when the speaker
  already went quiet on a reply; a barge-in pause leaves it alone, so the
  rewind the clear hands back is post-chain and is written as it is, and
  the chain goes on from the next sample); the device's own sounds (chirps,
  the Wi-Fi profile's replies and realtime voice calls, the mp3 test) go through
  muse_audio_write's instance; the bench measurements (`muse_audio_selftest`, the loopback test)
  stay raw. ~37.7k RV32 instructions per 10 ms frame (QEMU instret, the
  original's 100k), ~440 bytes a chain in PSRAM. Console words (USB and
  `!IN`): `spk.dsp` / `spk.dsp?` (a diagnostic) and
  `spk.dsp=on|off|HPF,THR,RATIO,MAKEUP` (automation; the ranges HPF 0 or
  50-4000 Hz, THR -60-0 dBFS, RATIO 1-20, MAKEUP 0-30 dB), answered
  `@spk.dsp ...`, live for every chain (a change retunes mid-reply, no
  click), kept in NVS (`muse_spk`/`dsp`, a versioned blob; unreadable or out
  of range: the defaults). `tests/test_muse_spk_dsp.py` runs the map, the
  filter, the curves, the ceilings, the settings and the barge-in pause
  through the chain on the real `muse_audio_tx` core. **Volume caps on the
  Mosaico:** with the chain its 50 plays ~4-5 dB louder
  than the old map's 50 (40 matches the old loudness);
  the first-boot default stays 50. Each stream logs its stats on `mg.play` (on the
  Nordic UART mirror), including the level that reached the speaker, and
  while it plays one line a second for A/V checks, plus one at its end:
  `av play[ end]: t=MS span=MS underruns=N new=N last=MS buf=MS queued=MS
  int=FREE low=LOWEST` (t is the stream's own clock, every 10 ms chunk the speaker took
  since start_streaming; last is the latest underrun's time on it, -1 for
  none, so an underrun in a reply's first 200 ms tells from a later one;
  the end line adds `dropped=N repeated=N repaid=N`, the drift trim's samples
  and the gaps repaid). The
  component README has the details, and what clients must do about their
  stack's Write Without Response back-pressure.
- **Firmware update over BLE:** `CONFIG_MUSE_GADGET_BLE_SMP` (on in
  every build with musegadgets BLE audio: the BLE-only builds `c6ble`,
  `stopwatch-video`, `watcher-video`, and the Wi-Fi + BLE ones such as
  `s3-gadget` and `watcher-gadget`; off on the ESP-Mosaico, which updates through ESP-Iris over
  USB) serves MCUmgr SMP,
  what the Muse apps' firmware update and `tools/mg_ble_client.py images` /
  `dfu` speak: the MCUmgr SMP core, vendored in
  `components/muse_gadget_ble/smp/` (`VENDORED.md` describes the copy; don't
  edit it here), under the ESP-IDF backend `mg_smp_esp.c`. The file is an
  MCUboot envelope around the signed app: `board.sh` writes
  `muse-gadget.mcuboot.bin` beside `muse-gadget.bin` (`tools/muse/mcuboot_wrap.py`,
  the shared MCUboot packer with the app's build id; it holds the SDK token if
  the build did). The gadget holds the app's first 352 bytes until they say
  it's this board's: the image header's chip id, and the board description
  every image carries (`components/muse_image_id`: product code and board
  profile, the device model when both carry one, in ESP-IDF's custom app
  description section, BLE updates kept).
  Another chip's or board's image, or one built before the description, is
  refused there (rc 3, EINVAL) with nothing written (esp_ota_begin erases
  nothing with sequential writes), so the other slot keeps what it held;
  install such a build over USB. MCUmgr's test-boot revert and image-state
  writes to a slot that already holds an image aren't checked (no upload), so
  an unconfirmed test boot still returns to a slot that predates the
  description. Then it writes the app to its other OTA
  slot, checks the upload's whole-file SHA-256 and the envelope's, then the
  app's RSA signature, and keeps it only then; test boots it once (`PENDING_VERIFY`),
  and it's kept at the boot's healthy mark (the boot guard's: reachable and
  30 s up) whether or not the client's confirm came (an early confirm waits
  for the mark); a test image that never gets there boots the old one again.
  Who may update (`protocols/mgcommands.h` Token proof,
  `test-vectors/mg-update-v2.json`; `mg_smp_gate.h`, and the request front
  `mg_smp_front.c` that `mg_ble.c` runs ahead of the core): no phone, token or
  pre-armed hold. An upload, image state write, reset or erase runs at once
  for a connection whose token proof matched (the Muse apps prove before they
  update), after the wired console's `update.arm`, in a SAFE the user entered
  with the key, and on bench, automation and test builds (no run waits on a
  person). Any other client (a web updater), set up or not (no device token,
  or after Forget), gets one confirm prompt on the gadget, "Press to confirm
  update" for 30 s, answered rc 10 (EBUSY) while it asks, at most once per
  10 s per connection (`MUSE_UPDATE_REARM_MS`); the client retries, and the
  first write after a physical press of the gadget's own key, the talk key
  included (a touch board's Confirm; the status light's confirm pattern on a
  screenless board), gets rc 0 (`muse_local_press.h`; a press at any other
  time, or injected input, never counts). Such an upload's first chunk must
  name its length and whole-image SHA-256: one that names none gets rc 11
  (EACCESSDENIED) and no prompt, and so does a chunk with no upload started
  on its link. The press binds to that upload's length and SHA-256 for 600 s,
  across link drops: the same upload starts again or resumes on a new link
  with no new prompt, and the link that carries it goes on (its chunks, the
  state write, an erase or the reset); a transfer under way at 600 s finishes
  on its link, but starting it again, on that link or another, asks again, as
  any other upload does. A write past the bound length, a last chunk after
  which the slot's bytes don't hash to the bound SHA-256, and a state write
  while they don't get rc 11 and end the binding. A press on a prompt an
  erase, a reset or a state write started covers only that request (the same
  body) on its link, for 600 s. Forget ends the binding, what a press covered
  and the link's proof (the wired console's arm stays). The timeout or a
  cancel answers rc 11 once and ends the prompt, binding nothing: the next
  attempt asks again (within the 10 s re-arm limit). Known
  limit: on a release gadget with a token, a nearby device
  can start a firmware update without the token proof, and the owner's next
  physical button press (the talk key included) installs it. Where images
  are signed, they are signed with the repository's development key, which
  is public, so a valid signature doesn't show who built the image. The ask
  wakes the screen; a passcode-locked gadget wakes to its keypad ("Unlock to
  update") and the confirm card follows the unlock, within the same 30 s.
  While a passcode keypad or any other passcode entry is on screen no touch
  reaches a BLE client or the HID host (`muse_touch_private()`). A SAFE the
  boot guard entered by itself (failed boots, crashes, run losses; screen
  off) answers rc 11, set up or not, logged with the way back:
  the key into SAFE, or USB recovery. Reads (image list, `mcumgr_params`,
  echo) answer anyone. The vendored core asks its hooks only at a transfer's
  start, so on a link that needs the press the request front reassembles
  each request, asks about every change whole (the slot's running SHA-256
  beside it) and answers rc 10 or 11 itself; the core sees only what it let
  through (1 KB more in the FIFO block while a link writes, and 160
  bytes for the slot while a press binds an upload). Every request, proven or
  not, nested deeper than 8 CBOR levels is refused rc 3 before any decoder
  sees it (`mg_smp_front_depth_ok`, iterative), and so is one it can't walk
  to the end: a malformed or cut-short item, a length or count past the
  request or past 32 bits (a 32-bit decoder could truncate it), bytes after
  the body. A request larger than the front holds reaches the core as its
  header alone (EMSGSIZE) and the rest of its bytes are dropped, never read
  as requests. With no memory for the FIFO block (and so no front) SMP writes
  are refused. Any image upload write past its upload's length (an offset-0
  request longer than its own len, or a later chunk past the len its first
  chunk named) gets rc 11 and ends the upload, however the link was opened
  (a matched proof, the wired arm, a bench build, the press), as smp-v12
  does; the vendored smp-v11 core would drop the extra bytes and answer
  rc 0 (`mg_smp_front_past_len`). The NimBLE host task hands ATT
  writes to the worker through a FIFO that holds the client's whole window
  (`buf_count` 3, requests of at most 512 bytes: Android refuses a larger
  write, and its app writes a request in MTU - 3 pieces), so nothing is
  dropped. One update at a time: a Bluetooth update and a Wi-Fi OTA
  (Link setup's) both claim the other slot first (`ota_claim()`,
  `main/ota.h`; every Wi-Fi download goes through `ota_start()`), and each
  refuses as busy while the other holds it. On a Wi-Fi + BLE build an image
  a Bluetooth update installed is kept at the first of: the control session
  up (as a Wi-Fi OTA's image is) or the boot guard's healthy mark (BLE
  reachable + 30 s), saved network or not (`mg_glue.c` `smp_keep_mark`): BLE
  is the recovery tier, so an image that breaks Wi-Fi can still be replaced
  over BLE. The boot's health check never rolls it back: it rolls back only
  what a Wi-Fi OTA recorded installing. Memory: updates take no base memory; nothing is held between updates. The
  core (smp-v3) keeps one pointer and takes its state and request slot
  through `mem_get`/`mem_put`, and the glue its FIFO, only while a client
  uses SMP, from PSRAM (the internal heap only on the C6), all given back
  once the link is gone (an interrupted upload's state alone waits 120 s for
  a resume); `ota?` shows what SMP holds right now. Whether a unit's
  bootloader can roll back is proven by its first test boot and kept in NVS
  (an image that boots `NEW` instead means no rollback: every update there is
  permanent, and image state says so); the console's read-only `ota?` shows
  the slots, their states, the bootloader and that verdict. Beyond the token
  proof check above, the board check and the signature check protect it, and
  the dev key is in the repo. `tests/test_mg_smp.py` runs the core with the real
  backend on the host, driven by both apps' flows. On a screen:
  "Updating N%" with the dim "Don't touch it until it's done",
  "Installing" from the last byte on, and "Update stopped" with what the stop
  left: "Nothing was installed" before the commit (still the old version),
  "Installed; restart to finish" for the link gone after it with no reset
  coming (the component README, Firmware update; `test_mg_smp`'s
  UpdateStopLine).
- **Link modes (mgcommands.h "Link modes";
  `components/muse_gadget_ble/mg_linkpol.c`):** sleep 100/160/4/600, ready
  24/36/5/400, active 12/12/0/400. Ready at every connect (asked after the
  central's first update, or 1 s after connect); active on the gadget's own
  activity (its talk press at touch-down, a call from Calling... on (Calling... and connecting for at most 30 s,
  `MG_LINKPOL_CALL_STARTING_US`; live while live), a
  stream the app starts: video, a changing UI stream, audio playback;
  push-to-talk capture, Link setup, an MCUmgr update); the gadget's own turn
  is activity (mgcommands.h): from its talk press (button_down
  sent, `mg_ble_link_turn()`) until its reply has played, assistant_state
  done or error, or 20 s with no reply (on the Watcher a plain
  "5 s after the last activity" fired mid-thinking and the reply started on
  the ready link), logged `link mode active kept (turn: waiting for the
  reply)`; back to the app's mode 5 s after the last activity ends; sleep by
  itself after 10 min in ready.
  The app's setting `link_mode` (16, per connection, never stored) sets its
  mode; its active expires after 30 s without activity; a ready or sleep
  write waits for the gadget's own activity to end; a read returns the
  effective mode (a held write isn't visible) and every change is notified. `link_params` (17,
  read-only) is the granted interval, latency and timeout. A refused
  request is asked once more after 5 s; one refused for a procedure
  collision (LL 0x2A, Different Transaction Collision; NimBLE status 554)
  after 300 ms instead unless the link is already in the set (outside the
  2 s pacing; twice a mode, then as any refusal); one with no answer at all
  in 2 s (no grant, no refusal: seen on the Watcher at a talk press from
  sleep) is asked again then unless the link is in the set by then, logged
  `link: no answer to the request in 2 s, asking again`; NimBLE keeps an
  unanswered request 40 s, ends it without an event and refuses another
  meanwhile with BLE_HS_EALREADY, which isn't a refusal
  (`mg_linkpol_request_busy()`: again 2 s later), for at most 45 s a mode
  (`MG_LINKPOL_UNANSWERED_MAX_US`), then as a refusal; at most one request per 2 s, none
  while one is out, none within 500 ms of a connection update; the
  call-start toggle stays. A touch, a key, a menu, the console
  or a GATT write alone isn't activity, and the screen and USB power no
  longer matter.
  A user's wake or touch (any key press, a touch, the screen lit by the
  user; bench-injected input too) moves sleep to ready, never active, and
  restarts the 10 min, lifting even the app's written sleep for 10 min
  (`mg_ble_link_user()`; logged `link mode sleep -> ready (touch)`);
  a talk press still goes straight to active. Every mode change and grant is
  logged on `mg.ble` (`link mode ready -> active (talk press)`, `link:
  granted 15.00 ms latency 0`).
  During each switch the gadget listens
  at every connection event: from the request until its set applies, a
  refusal, the stack refusing to send it, the link's loss or 20 s, it
  ignores its slave latency locally, then the granted latency applies
  (only while the link has a latency). On the S3 and C3 the controller's
  undocumented vendor command 0xFC13 (Set Preferred Slave Latency 0) does
  it; that controller restores the granted latency only at a connection
  update instant, so after a refusal or 20 s it keeps listening until the
  next update (logged). The C5, C6, H2 and S31 controllers have no override:
  a 1-byte notification is kept pending every half interval instead
  (Battery Level's value if subscribed, else an empty Nordic UART TX one if
  subscribed; never an unsubscribed one). Logged `link: listening every
  event (switch to active)` / `link: latency back (N ms): granted, latency
  0`; bench switch `link.wake=0|1` (off by default on every ESP32 controller: it goes on only where an A/B shows a gain, and the A/B showed none: S31 + iPhone 17 2976 vs 2974 ms, S3 + S22 10.7 vs 11.1 s; `CONFIG_MUSE_GADGET_BLE_LINK_WAKE`; RAM only; automation)
  and `link.wake?` (diagnostic), USB console and Nordic UART; bench knob
  `link.sleep=ITVL_MIN,ITVL_MAX,LAT` (for a sleep-parameter A/B: replaces
  sleep's 100/160/4 in the sleep request, 6 s timeout kept; RAM only, kept
  across connections; checked against the BLE spec's ranges and timeout >
  (1 + latency) x max x 2, else the usage line; `link.sleep=0` restores the
  defaults; automation) and `link.sleep?` (the set in effect and the last
  sleep grant; diagnostic). The component
  README (Link modes) has the controller details and limits.
  `tests/test_mg_linkpol.py` runs the policy on a fake clock, the window
  included;
  `test_mg_ble` runs settings 16 and 17 through the protocol.
- **Who is connected (setting 19):** the Status page's
  row reads "Connected: <name>": only the name the gadget reads itself, the
  central's GAP Device Name, over GATT once 1.5 s after connect
  (`CONFIG_MUSE_GADGET_BLE_PEER_NAME`, which selects NimBLE's central role
  and GATT client); "Connected: phone" while that read is
  pending or after it failed, each failure counted (`peer_name_fails=N` on
  the console's `status` line, never the name). `client_name` (18) is
  reserved (the apps don't send a device name): not listed,
  `unsupported` like any unlisted setting. Never plain "Connected" or
  "Connected: Muse app". A Wi-Fi gadget with no phone app shows
  "Connected: <Wi-Fi network>". A tap on the row (bench: `tap=home.status`)
  shows "<app name>: <version>" from `client_app` (19) or "App version
  unknown" (over Wi-Fi "Wi-Fi: <IP>, <RSSI> dBm"), back after 5 s or another
  tap. Names get the fonts' ASCII stand-ins; a row too wide for its line
  wraps onto a second one, never "..." (the whole
  name stays readable): at a space where both lines stay whole, else at whole
  UTF-8 characters, the first line moved up a line into the room above and
  each line as wide as the panel's chord there; if two lines of the
  connection font still can't hold it, two of the rows' smaller font; only
  then is line 2 cut, at whole characters (`muse_status_wrap`; the
  simulator's `ssid=NAME` key renders it). `client_app` is per connection
  (cleared at disconnect), answered by get_settings, listed in
  request_status, and refused with `invalid_value` when over-long or not
  UTF-8 text. The names never reach the Nordic UART mirror or any log line
  (the app relays NUS into its logs; `test_muse_status_conn` scans every log
  and console call); the screen shows them.
  `components/muse_gadget_ble/README.md` (Who is connected) has the details;
  `mg_ble_client.py` sends `mg_ble_client <commit>` as `client_app` after
  request_status where it's listed (`--no-identity`: not), and no device name.
  `>ble.pause=SECS` (1-600; `!IN ble.pause=SECS` on the Nordic UART)
  drops the connection and stops all advertising for SECS, then resumes as
  before (no unpair, no NVS write): a gadget-side "off the phone" for A/B
  tests, logged on `mg.ble`.
- **A capture on a slow link is held, never cut short** (on the Watcher
  a talk press on a sleep link, its request for active unanswered,
  lost 156 frames and its `stop_mic`, and the phone's recording timed out
  after 120 s and swallowed the next press). Everything a capture sends goes
  through one ordered queue in `mg_proto.c` (the capture's outbox,
  `mg_capq_t`): button_down, change_data_type, the audio, button_up where it
  was released, the tail, stop_mic; a gesture goes at once, ahead of held
  audio and queued replies, when nothing of the capture's control waits and
  the stack takes it (mgcommands.h), else in order
  there. The audio is held in PSRAM
  (`CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S`, 10 s of its format), without
  PSRAM in internal RAM (`CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB`,
  8 KB, while 32 KB stays free), allocated at the capture's start and given
  back once everything has gone; it drains as the link takes it (the voice
  task's slices and the worker's 5 ms rounds, leaving the stack 4 msys blocks
  for control messages), and the voice task never waits on the link. Control
  messages are never dropped, only retried until they go or the link drops;
  only an overflowing buffer drops audio, the oldest whole frames. Logged on
  `mg.voice`: `capture over: ... still held for the link` and, once stop_mic
  went, `utterance over BLE: 3.4 s, N bytes, M frames dropped, rms -24.1
  dBFS, peak -3.2 dBFS, floor -61.0 dBFS, loud1s -19.8 dBFS, 0 clipped; end
  sent N ms after the capture ended, up to N ms held for the link` (the
  capture's level as the Wi-Fi path's `recorded` line measures it:
  whether the mic heard speech, from the gadget alone; `mg_capture_meter_t`,
  integer sums on the voice task). `loud1s` is the loudest 1 s window's RMS
  (for spoken-prompt tests; the whole capture when it is under a
  second): each 10 ms frame's energy from the sums already kept, a running
  sum over a 100-frame ring (`mg_loud1s.h`, 400 B, PSRAM where there is
  some), nothing more per sample; the Wi-Fi `recorded` line carries it too. `test_mg_ble` runs a 3.3 s turn on a
  modeled sleep link whose request is never answered, the overflow and a
  fast link (the component README, The voice transport layer).
- **A ~200 ms post-roll on release**: a press's
  capture for the client goes on `BLE_POSTROLL_FRAMES` (200 ms) after
  button_up, then its last frame, then stop_mic, all through the outbox;
  the red mic ring stays until the capture actually stops. A new
  press in the post-roll cuts it: that turn ends at once with its own last
  frame and stop_mic, and the press goes straight to its own turn (the mic
  runs on into its pre-roll; no refused press, two complete utterances).
  Device-ended turns get none: the 15 s cap, the client stopping it, a mic
  error, the link's loss, and the re-proof cut (a new token proof's challenge
  turns push-to-talk off: the press's turn ends at once, logged `push-to-talk
  turn ends: a new token proof ...`). The Wi-Fi paths keep their 120 ms tail
  (`TAIL_FRAMES`). `test_muse_ptt_ui` (voice 72: ~200 ms after the release,
  the ring up through it; 73: a press 100 ms into the post-roll, DUEDUE, no
  gap; 67/68: the cap and the client's stop, nothing after button_up).
- **The bench guard, `ptt.minrise` (added after five
  push-to-talk turns the mic didn't hear were sent and transcribed as
  "Help."):** with `ptt.minrise=N` (console words, USB `>` and `!IN`:
  `ptt.minrise=N` 1-40 dB and `ptt.minrise=0` off are automation,
  `ptt.minrise?` a diagnostic; answered `@ptt.minrise N` / `@ptt.minrise
  off`; `mg_minrise.h`) every BLE push-to-talk turn the gadget starts, a Live
  call's talk hold included, is held on the gadget (`mg_proto_turn_hold`:
  `button_down`, `change_data_type`, the audio from the pre-roll on and the
  turn's claim all wait in the capture's outbox, 16 s of the format in PSRAM;
  32 KB internal on the C6, `CONFIG_MUSE_GADGET_BLE_CAPTURE_GUARD_INTERNAL_KB`)
  until its capture's loud1s rises N dB over its floor (the utterance line's
  own meter: the quietest 20 ms window since the turn's first sample, pre-roll
  included); then it's released in a turn's usual order (`button_down`, the
  claim, `change_data_type`, the held audio as a burst at the outbox's pacing,
  live audio, `button_up`, `stop_mic`). One that never rises sends nothing at
  all (no gesture, no `stop_mic`), logs `ptt: nothing heard (rise X dB < N):
  turn not sent`, counts `ptt_nothing=K` on the `status` line, shows "Didn't
  catch that" (never thinking), and a reply paused at the touch-down plays on
  as after a tap. RAM only, off at every boot (never NVS), so a user's quiet
  speech is never dropped; off, every turn goes exactly as before. The
  component README (The voice transport layer) has the details;
  `test_mg_ble`'s `test_minrise` / `test_minrise_reply`, `test_muse_ptt_ui`
  76-78 and `test_ptt_minrise`.
- **The bench volume, `vol=` (bench and test mutes are RAM-only, never
  written to the saved volume):** console words, USB `>` and `!IN` (`muse_vol_word.h`):
  `vol=N` (0-100) sets the speaker in RAM only (`muse_settings_set_volume_ram`:
  0 is off with the level kept, as a mute; the saved values are kept aside,
  nothing goes to NVS), answered `@vol N (ram; saved M)`; `vol=saved` puts
  the saved volume back and `vol?` reads both, answered `@vol N saved M`
  (N and M as speaker_volume reads them: 0 while the speaker is off); both
  `vol=` words are automation, `vol?` a diagnostic; logged on `mg.bench`
  (the Nordic UART mirror). A reboot comes back at the saved volume. A
  user's change (a key, a slider, a call's controls) or the app's
  set_settings speaker_volume ends it: the saved values come back first,
  then that change applies and is saved, so the saved volume never holds a
  bench value; the client's set of the level the speaker already plays at
  changes nothing (`set_speaker_volume` in `mg_glue.c`: nothing saved, the
  bench volume stays). Like any change on the gadget it's reported
  unsolicited as speaker_volume. The bench tools use it for a run's
  volume and put it back: `mg_ble_client.py play --volume`
  and `av60.py --volume` send `!IN vol=N` over the Nordic UART and
  `!IN vol=saved` after (set_settings and the original written back on an
  older image). `test_muse_ptt_ui`'s
  `test_bench_volume_is_ram_only` runs it on `muse_settings.c` (no NVS
  write, the reboot, `vol=saved`, a user's and the app's change after it,
  the grammar), `test_mg_speaker_reports`
  the client's no-change set.
- **The gadget shows every speaker change it didn't make (the speaker is
  controlled on the device, and the phone can't mute it without the device
  showing it):** `muse_settings` counts every change of the
  level or the switch with who made it (`muse_settings_volume_changes`,
  `muse_vol_by_t`: the gadget, the phone, the bench; a boot's restore from
  NVS counts none). The phone is a client's set_settings speaker_volume
  (`mg_glue.c`) and Muse's own phone setup writes (`muse_ble.c`); the bench
  is the console's `vol=` and `vol=saved`. `muse_ui.c`'s frame tick
  (`volume_watch`, `muse_vol_show.h`, before the video check so the caption
  is current under it too) shows an outside change at once, with the overlay the
  gadget's own controls use and a short source line, **"Set by phone"** or
  **"Set by bench"**: in a call the call's controls (their level bar, the
  source on their hint line, `muse_call_volume_by`); on the home layout
  (the five catalog boards, every touch panel that isn't small or e-paper)
  Speaker's volume card, the source under the level, closed 3 s later as
  after a tap (`muse_home_volume_by`); on the compact and keys-only faces,
  which have no volume card, the caption line (`Set by phone: 40%`, `Set by
  phone: muted`), where their speaker key says "Speaker on/off"; the
  passcode's locked face and screens (no Speaker, any touch opens
  the keypad) get the caption line too, never the interactive card. A dark
  panel lights for it (`muse_app.c`, `muse_input_wake`). The gadget's own
  changes show as before, with no source line. **Muted (speaker_volume 0)
  shows for as long as it lasts:** the muted-speaker glyph (`volume_off`,
  the standard set's, cropped to the top row's ink like Wi-Fi and
  Bluetooth: `MUSE_ICON_INK`, `gen_ui_assets.py`) first in the home's top
  row and on the Sound page's Speaker row before its switch, and
  the font's mute symbol first in the compact faces' status row; the Sound
  page's switch and Volume slider follow a change made elsewhere. Logged on
  `muse_home` / `muse_settings_ui` / `muse_ui` (`volume overlay: 0% (Set by
  phone)`, `top row: muted icon on`, `Sound page: muted icon off`).
  Screenless gadgets (the Voice PE, the reSpeaker Lite: no speaker_volume
  setting, Muse's `voice.configure` is their outside change) play their
  volume cue, the LED ring's arc (`led_status_show_volume`), for it, before
  voice is up too, and take the same `vol=N` / `vol=saved` / `vol?` on their
  USB console (`voice_volume_word`, RAM only, the ring showing it; the dial
  or `voice.configure` ends it). `test_muse_ptt_ui`'s
  `test_bench_volume_is_ram_only` runs the counting.
- **Advertising interval (`main/adv_pace.h`):** once set
  up, the musegadgets advertisement goes fast (30-50 ms, itvl 48-80) for 30 s
  after boot, after a disconnect (from the link's loss) and after a talk
  press with no app connected (`ble_server_adv_fast()`, each trigger
  re-arming the window), then Apple's 417.5 ms (itvl 668): a one-shot callout
  restarts the advertisement at the window's end. Logged `adv: fast 30 s
  (disconnect)` / `adv: slow 417.5 ms`. Before setup the companion's turns
  keep 100-150 ms and Link setup its fast default; BLE stays quiet during a
  Wi-Fi video stream. `test_link_gadget_setup` runs it on ble_server.c's code.
- **Console words over Bluetooth:**
  `CONFIG_MUSE_GADGET_AUTOMATION` (Kconfig in `muse_gadget_ble`, default n:
  every board overlay builds a release image; lab and CI builds add the bench
  overlay, `MUSE_BENCH=1` with `devices/sdkconfig.muse-bench`, which turns it
  on) keeps every Nordic UART console word open to any
  connection (`!PAIR AUTO`, `!RESTART`, `!SAFEID`, `!SILENCE`, `!SETUP ADV`,
  `!IN ...`, `bench.out=`, `ble.pause`, `link.*=`, `vib=`, the bench
  tunings). Off (a release build), a word that changes state
  (`mg_unlock_automation_word()`) runs only on a connection whose token
  proof matched and answers `locked` on `mg.ble` otherwise
  (`mg_unlock_remote_ok()`, at `nus_command()`); in SAFE, `safe.off` /
  `safe=0` stay open (the way out of SAFE). Arming the bench auto-confirm
  (`!PAIR AUTO`, `!PAIR AUTO 1`) is refused there on every connection, proven
  or not (the USB console's `>pair.auto` arms it). Identity and status reads
  (`status`, `version`, `guard?`, `ota?`) and the other `?` words stay open;
  the owner's content (`!IN voice?`, `!IN img?`) answers only a proven
  connection on a set-up gadget (`mg_unlock_owner_word()`; refused, it's
  logged on the gadget and nothing goes back). The USB console is open
  with every word in every build (the local path: no other window arms
  remote words). `test_mg_unlock` and `test_owner_reads` hold the rows.
- **Passcode lock:**
  every board with the v1.0 home (touch, full layout: the StopWatch, the
  Watcher, the ESP-Mosaico, the C6 1.8, the 1.75) offers a 4-digit passcode,
  off by default, set in Settings > Passcode (after Display): Turn passcode
  on, choose, enter it again; Change and the switch's Turn off ask for the
  current code. It's device-local: nothing about it goes over BLE, the
  console or the device session. The rules are pure C
  (`muse_passcode_core.c`: the record, salted SHA-256 with a format tag, the
  wrong count and wait level in Muse's NVS namespace as the `passcode` blob;
  when it locks; the waits; the keypad's geometry and hit rules), behind a
  mutex in `muse_passcode.c`, the screens in `muse_passcode_ui.c` (the
  locked face is the home with no Speaker / Call / hold region and a lock
  glyph for the page dots, `muse_home_set_locked()`; the digit ring on round
  panels, the 3 x 4 grid on the others, the wait after 5 wrong codes, Forgot
  passcode). It locks at screen-off (or 5 min after it), after 5 min idle
  where the screen stays on, at every boot; never during a Live call or the
  gadget's own talk turn. Locked, a talk press opens the keypad ("Unlock to
  talk") and is dropped; a call the phone starts waits behind it ("Call
  from phone": nothing from the call until unlocked, declined after 60 s);
  menus, Status, Settings, reply text and drawn files
  stay off the screen; the Link setup prompt, low battery and power screens
  show as usual. Wrong codes: 5 free, then 30 s, 1, 2, 4, 5 min, kept across
  reboots; never a wipe. Only NVS saying there's no `passcode` blob means
  no passcode: a blob that can't be read (any other NVS error, or one of no
  known size or format) boots locked with no code right
  (`muse_pass_init_unreadable()`; wrong codes still reach the wait, counted
  in RAM, nothing written over it), and Forgot passcode's Reset pairing (or
  the console's `pass clear`) erases it. The pairing's erase takes it (`passcode_forget()`
  in `main/app.c`: Reset pairing, the token proof's clear, a setup reset, a
  completed Link setup). A UI stream or phone camera view that starts or
  arrives while locked is held like a drawn file (`muse_video_set_hold()`:
  not shown, the screen not woken for it, shown once unlocked if still live;
  a call's own video never). A Set, Change or Turn off passcode keypad on an unlocked gadget holds it
  the same way and keeps its touches from the phone (`muse_video_set_cover()`). Console: `pass` (a diagnostic: `@pass on locked
  tries=0 wait=0` or `@pass off`, never the code, salt or hash), `pass
  lock` and `pass clear` (automation: lock now; erase only the passcode, the
  pairing kept, logged "passcode cleared from the console"; the shared
  ?PASS / PASS lock / PASS clear words); `tap=pass.swipe.<k1>,<k2>,<k3>` plays one
  stroke through those keys at a finger's pace (on the ring through the
  centre; `muse_touch_press_path()`), the three-key swipe check on hardware;
  `tap=pass.<0-9|del|forgot|reset>` presses its
  controls as a finger. The simulator renders it (`passcode=DDDD`,
  `passlock=true`, `stroke=X,Y;...[;up]`; `render_screens.sh` writes
  `passcode_*.png` beside the other screens). `tests/test_muse_passcode.py`
  holds every rule.
- **Recovery:** `CONFIG_MUSE_BOOTGUARD` (`muse_bootguard.h`) in every
  build (see below for the builds without
  musegadgets) and `CONFIG_MUSE_GADGET_WDT` (`muse_wdt.h`) in every build
  with musegadgets BLE (the BLE-only builds and the Wi-Fi + BLE ones such as
  `s3` and `watcher`):
  three boots in a row that don't reach healthy (BLE advertised or connected
  and 30 s up; Wi-Fi isn't part of it) restart into the ROM's download mode
  with a USB host attached, else SAFE mode (no Muse UI, audio or input; Link
  setup, musegadgets and the BLE update stay up); two crash resets keep Muse
  off (SAFE) until the BLE console's `safe.off` or a new image. On a Wi-Fi +
  BLE build SAFE is Bluetooth only (`run_safe_bluetooth_only()` in `app.c`):
  Wi-Fi, the Muse session, the tunnel and Wi-Fi OTA stay off that boot, so
  none of that code can be what keeps crashing it, and device_info says wifi
  "none" (a setup there is token-only). The task watchdog (15 s, a panic;
  on in every BLE gadget profile with the interrupt watchdog at 1.5 s, and
  also watching both idle tasks) watches the NimBLE host, the musegadgets worker, the
  Muse input task and Muse's voice task (each mic read, speaker write and
  delay feeds it; its rest waits are 10 s slices; the input task logs
  `mg.voice: the voice task hasn't moved for 5 s ...` first, and `voice?`
  says where it is any time, with the voice task's, LVGL's, the worker's and
  the panel send task's state and priority now), and on a Wi-Fi build the Link session too: from its
  first TLS read or write after connecting (the connect and the reconnect
  wait, each with its own timeout, aren't watched). Every wait in it returns
  within the watch: the socket is non-blocking, each TLS read or write
  retries inside a loop bounded by a 15 s deadline and feeds the watch while
  it waits (`io_retry_wait()`), so a Wi-Fi stall ends the session, never the
  chip. **Leaving SAFE**: the BLE
  console's `safe.off`, the USB console's `>safe=0` (or `>safe.off`), a new
  image, or holding the talk key 5 s (`MUSE_BOOTGUARD_SAFE_HOLD_MS`; the key
  seen up first, so one held through the boot doesn't count): Muse on and a
  planned restart (the console words answer first and restart 500 ms later,
  `MUSE_BOOTGUARD_SAFE_OFF_RESTART_MS`; the C6 needs the restart). SAFE turns the
  panel off right after `safe_init` (`muse_board_t.safe_display_off`, no
  display init or LVGL: the AMOLEDs' reset held, the LCDs' backlight or its
  rail off, a buzz on the StopWatch), since an AMOLED keeps its last frame
  across the CPU reset and SAFE looked like a working home; the reTerminal
  Sticky's e-paper can't (it keeps its image unpowered). There is no UI in
  SAFE: each board reads its key without init (`muse_board_t.safe_key`: BOOT
  on the Waveshare S3 1.75/1.75C and the C6 1.8, KEYA on the StopWatch, the
  AI key on the ESP-Mosaico, the wheel on the Watcher through its expander;
  a board without one logs that only the consoles leave SAFE). **Into SAFE
  with the key**: the board's SAFE key, seen up first, pressed within 3 s of
  boot (`MUSE_BOOTGUARD_ENTRY_WINDOW_MS`) and held 10 s
  (`MUSE_BOOTGUARD_ENTRY_HOLD_MS`) makes a planned restart (`MUSE_RESTART_SAFE_ON`)
  into one SAFE boot whose cause is `button` (`muse_bootguard_safe_why()`,
  `guard?`; its recovery declaration says reason `user`). It's a hold after boot, not at reset: BOOT is a strapping pin on
  the S3 and C6 boards and starts the ROM's download mode when held at reset.
  A key held through the boot never counts, a press released before 10 s or
  begun after 3 s never enters SAFE, and a Link button press begun in the
  window doesn't run the 5 s setup reset. In a SAFE the guard entered itself,
  a press begun in the window is this way in; a later one still leaves SAFE at
  5 s. Boards with no key the gadget can read raw from boot (Core2, CoreS3,
  Waveshare S3 LCD 7, LCD 1.28, EchoEar-2ST) recover over USB. **SAFE's
  consoles**: every recovery path stays open.
  Over BLE: SMP (image upload, confirm, reset; on a set-up gadget only in a
  SAFE the user entered with the key), the Nordic UART's bench
  words (`!PAIR AUTO`, `!RESTART`, `status`, `version`, `link.wake`/`sleep`
  ...; in a release build the ones that change state need the connection's
  token proof, as outside SAFE) plus `safe.off`/`safe=0`, `guard?` and
  `ota?` (always open), and the log mirror.
  Over USB: the log, and `safe=0`, `safe.off`, `guard?`, `restart`, then every
  word that reaches no Muse feature (`glue_link_console`: `version`,
  `sdk_token=`, `hang=`, `gatt.dump`, and musegadgets BLE's bench words). The
  Muse words and injected input (`tap=`, `press=`, `!IN`) answer that Muse is
  off: **only while in SAFE, and only because SAFE runs no Muse feature for
  them to act on**. Leaving SAFE is a restart into the normal console, every
  word and injected input back (the SAFE paths are keyed on
  `muse_bootguard_mode()` alone). On a Wi-Fi Muse build a small loop rejoins
  Wi-Fi in the keeper's place, so the Link session stays reachable
  (`muse_input_safe_console_start()`;
  ESP-Iris's console on the
  Mosaico lands there too). **Builds without musegadgets** (`mosaico`,
  `s3` with `MUSE_VIDEO=1`, the Link-only boards such as the DevKitC-1): the
  same crash count; healthy is 30 s of uptime with no reachable requirement
  and never a stay-reachable reset (`CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE`
  off: their BLE is off or silent once set up, and a Wi-Fi or VM outage isn't
  a crash). SAFE there keeps Link up (Wi-Fi, the Noise session with
  `device.health`) and the USB console, with Link setup BLE
  as it is (the SAFE identity is for musegadgets builds only). Muse is off,
  and the Link's own commands too (`CONFIG_MUSE_BOOTGUARD_SAFE_LINK_QUIET`:
  several reach Muse's display and session, which SAFE never starts):
  `link.register` lists only `device.health`,
  `device.health` says `safe: true`, every other command but the VM
  management ones answers `safe_mode`; a Link-only board's status light or
  screen shows the error state, and its button's 5 s hold leaves SAFE
  instead of resetting setup. **The ESP-Mosaico** runs the guard beside
  ESP-Iris's own counter (three unplanned crash resets without 30 s up: Vibe
  Mode): SAFE comes at the second crash and is decided at boot (no restart),
  so Iris still catches a SAFE boot that crashes too; SAFE keeps ESP-Iris on
  USB (its `safe_init`: Iris and the power switch), so Vibe Mode can still
  install the next build. On the
  panels drawn through `boards/muse_lcd_bands.c` LVGL never busy-waits for a
  band (otherwise it spins on a band whose last piece never comes back and
  starves its core): it waits on a semaphore for at most 1 s
  (`lv_display_set_flush_wait_cb`), the send task waits for each piece at most
  500 ms, and either one gives the band up, logs on `lcd_bands` (where the
  send task was, LVGL's priority then), counts and redraws the whole screen
  (`tests/test_lcd_bands_recovery.py`). Keep every wait there bounded.
- **Plugging in:** USB turns an off gadget on (its power chip does;
  each boot logs what did). On a low cell it charges dark: right after the
  boot guard, before Link, BLE or the display, `muse_charge_dark_hold()`
  (`components/muse/muse_charge_dark.h`) holds with no display or radio until
  the cell reads the board's low-cell level (`battery_shutdown_mv`, the low-battery
  shutdown voltage: one constant per board) plus 300 mV on charge, then
  boots; only on a fresh power-up USB caused (`muse_board_t.early_power`),
  before any app code, never in SAFE mode, and a key press boots at once.
  While it holds, the USB recovery tier and the keys answer and the BLE tier
  is down (no advert; BLE recovery may be down during the hold since USB
  and the keys remain). The
  ESP-Mosaico can't comply (its SAM8108 switch ignores VBUS):
  `devices/README.md`, Plugging in turns it on.
- **A bench nap never cuts power:** `>nap` makes the
  gadget behave as on battery over USB (`muse_state_set_as_if_battery`): the
  codecs rest, the display pauses (the CPU stays at full speed with a USB
  host), the Wi-Fi naps. The as-if battery ends by itself after 10 min
  (`MUSE_AS_IF_BATTERY_US`, logged), and an expired one on USB ends the nap.
  Nothing that cuts power reads it: the Power off a board allows only on
  battery (`power_off_battery_only`, the ESP-Mosaico, whose latch USB holds
  and which then comes back only on its switch) goes by
  `muse_state_on_battery_real()` in Settings, the app's power_off and the
  power-off itself, and the low-battery shutdown by the reading's own
  USB flag. There's no deep sleep outside the power-test image.
  `tests/test_as_if_battery.py` lists who may still read the as-if battery
  (behavior only); a new caller that can cut power uses the real state.
- **Video:** `CONFIG_MUSE_GADGET_BLE_VIDEO` (`tools/muse/board.sh build
  stopwatch-video`, `mosaico-video` or `watcher-video`, overlay
  `devices/sdkconfig.muse-video-ble`, BLE only) adds
  `mg_command_stream_video`: the phone's camera as H.264 on Data, into the
  shared video core (Video from a phone, above) as its BLE transport
  (`main/mg_glue.c`). Beside reply audio, change_data_type picks the stream
  Data goes to; when the client ends one, Data goes to the other, and when
  the device ends one (its own stop_streaming), Data selected for it is
  discarded until the client's next change_data_type (mgcommands.h;
  `rx_discarded`, logged once on `mg.ble`: `Data discarded after
  device end: N bytes until change_data_type`; `test_mg_ble`). A dropped or
  undecodable frame asks for a keyframe at once, then every 300 ms. Video credit goes back promptly
  (AV60: the apps keep 2-3 KB in flight in a reply): buffer_update once 1 KB is taken from the
  ring (per frame, before its decode) or received, else 20 ms after any news, one in flight per
  connection event (NOTIFY_TX-gated as reply audio's), 250 ms keepalive, a still UI stream silent
  (`mg_proto_video_tick`). The Phone camera item and `>video` / `>video=0` ask
  the BLE client for a stream too; `>video.itvl=N[,ce]` tunes the link a
  stream asks for. The client is `../android/musegadgetstest` (Muse Gadgets Test,
  GadgetLink). The component README has the BLE specifics and numbers.
  The profile also takes UI streams (`../protocols/UI.md`: the panel's
  466 px one to one, one or two bands, frames on change, the link relaxed
  while still) and offers `mg_command_ui_input`: touch events, and with
  `CONFIG_MUSE_GADGET_BLE_HID` (off in the profile; `MUSE_HID=1
  tools/muse/board.sh ...` adds `devices/sdkconfig.muse-hid`) HID over GATT
  beside musegadgets, with bonding (the component README, UI streams and UI input
  and HID). Bench console: `>ui.lat=N`, `>ui.md5=1`, `>touch=X,Y[,X2,Y2,MS]`,
  `>restart` (`>restart=0` / `=1`: from a task on that core), `>ui.dump` (the last UI picture's decoded planes and the
  panel's pixels as base64 lines; `tools/muse/uidump.py LOG OUT` makes PNGs),
  `>ui.hq=0` (the plain conversion, no frame buffer, for comparison) and
  `>ble.pause=SECS` (link modes, above) and `>ble.forget` (forget every bonded phone, a HID host that keeps taking the
  connection, and turn HID off) `>ptt=1` / `>ptt=0` (push-to-talk down and up, as the `d` / `u` keys, for a
  console that takes only lines, such as the Mosaico's over ESP-Iris) and
  `>hid.off` (HID off and its service
  hidden until a client's `hid_map` turns it on, so a phone's OS stops
  attaching as its host). On every build with the full UI, presses of Muse's own
  screen and buttons for a bench without fingers (each logs its outcome on
  `mg.bench`, on the Nordic UART mirror): `>press=X,Y[,X2,Y2,MS]` (a finger
  down, moved, lifted: the touch read reports it in place of the panel, for
  LVGL and the call's video alike; the phone's touch events get none of it,
  unlike `>touch=`), `>tap=<control>` (the same at a control its screen
  locates: `home.speaker`, `home.speaker.hold`, `home.slider<N>`,
  `set.speaker`, `set.volume<N>`, `call.minus`, `call.plus`, `call.panel`,
  `call.swipe.down`, `call.swipe.up`; "not on the screen now" otherwise),
  `>key=speaker|power|aux` (a press and release, never a hold), `>key=talk[,MS]` (the talk key held MS ms, 400 by default, through the key's own path, from a dark panel too; never a pairing's confirming press; `>key=talk,MS,AFTER_S` queues one to start AFTER_S s later, up to 4, each waiting for the one held before it, so a bench can arm holds over the Nordic UART and hand the connection to a phone; `>key=talk?` lists them, `>key=talk,cancel` drops them),
  `>page=face|settings|settings/<page>` (not in a call; `settings/tests`
  and `settings/vibration` too) and `>wake?`
  (`screen awake|asleep|dark`). On a release image a Nordic UART line's
  `ptt=1` or `key=talk`, or a bench press or tap on a talk surface, answers
  `real press needed` and never opens the mic; the gadget's own press does,
  and so does the wired console's `>ptt=1` and `>key=talk` (the USB console
  is fully open: an automated run never waits on a person). Test and
  automation builds take every source. On a board with a vibration motor (the
  StopWatch), `>vib=pulse,MS[,PCT]` (1-500 ms; a pulse under the actuator's
  30 ms plays lengthened to it), `>vib=preset,tap|double|long|ring|success|error[,PCT]`
  (ring runs 10 s, or until `>vib=preset,ring` again or `>vib=stop`),
  `>vib=stop` and `>vib?` play Developer > Tests > Vibration's tests through
  the production haptics engine (they skip `haptics_enabled`, never the update, battery, sleep,
  talk-turn, cap and duty rules) and answer with its status line on
  `mg.bench`; nothing plays one on its own (no boot or timer pattern). A press on a sleeping screen is refused
  (`screen asleep`): `>wake` first. Over the Nordic UART, `!IN <word>` takes
  these and `call`, `call=0`, `call?`, `wake`, `ptt=1` / `ptt=0`, `voice?`,
  `vib=` / `vib?`, `mic.gain?` / `mic.gain=N`, `spk.dsp` / `spk.dsp=`, `ptt.minrise=` / `ptt.minrise?` and
  `vol=` / `vol?` (`mg_glue.c`'s allowlist: nothing that changes setup,
  power or tokens). Who may send them (Console words over Bluetooth, above):
  in a bench build (`CONFIG_MUSE_GADGET_AUTOMATION`) every Nordic UART word
  is open: `voice?`, `wake?`, `call?`, `vib=`,
  `vib?`, `key=` (talk holds, queued ones too, open the mic), `ptt=`, `tap=`,
  `press=`, `page=`, `call`, `call=0`, `wake`, `ble.pause=` and, with the
  device session, `session.ping`, and `!PAIR AUTO` (also the USB console's
  `>pair.auto` / `>pair.auto=0`), `!RESTART`, `!SAFEID`, `!SILENCE` and
  `!SETUP ADV` (the vibration tests' safety rules hold
  whoever asks; `vib=` / `vib?` also work as plain lines), so benches stay
  automatable: there an armed `!PAIR AUTO` or an injected press (`press=`,
  `tap=`, `key=`) can complete a Link setup with no one at the gadget. In a
  release build the words that change state run only on a connection whose
  token proof matched (`locked` otherwise), arming `!PAIR AUTO` never runs,
  and a `key=` while a setup or re-setup prompt is up answers `real press
  needed` and presses nothing (an aux press it made never confirms a prompt
  that arms after the line, either: `aux_key()`); identity and status reads and
  the other `?` words (`wake?`, `vib?`, ...) and, in SAFE, `safe.off` /
  `safe=0` stay open, the owner's content (`voice?`, `img?`) answers only a
  proven connection on a set-up gadget, and the USB console stays open with
  every word (its `ptt=1` and `key=talk` are the owner's own action; only a
  scripted touch on a talk surface doesn't talk on a release image). An old tool's `?UNLOCK` /
  `!UNLOCK` / `!LOCK` gets `UNLOCK not needed` (`mg_unlock.c`, which keeps
  its name from the owner unlock it once held). The `!IN` allowlist itself
  stays: nothing that changes setup, power or tokens beyond those words. All are
  queued to the input task (never run on the NimBLE host); what reads
  LVGL's objects runs on the LVGL task (`muse_ui_run_in_lvgl`), and no
  bench word waits for the display lock.
  The Wi-Fi video overlay (`MUSE_VIDEO=1`) goes on a board's Wi-Fi profile
  only (`board.sh` refuses it with a `-gadget` alias): with Wi-Fi, the UI and
  video the S3 has no internal RAM left for BLE audio.
- **Mute reports and subtitles:** every change of the speaker level
  made on the gadget (a key, a slider, Speaker's long press, a call's volume
  − / + or swipe, the console's `vol=` / `vol=saved`) reports speaker_volume
  unsolicited (0 when muted), and the
  client's own set is never reported back, even when it takes two settings
  writes (`mg_spk_report.h`); each report logs `mg.ble: speaker_volume N
  reported`. **A report is owed until it leaves on Control** (otherwise it would be lost with no client
  ready, behind a full outbox, or queued at a disconnect): `mg_proto` keeps
  the level a client last saw (`spk_seen`: the last report the stack took,
  or the client's own set; kept across connections) and whether a change is
  owed; the report never waits in the outbox: once the replies queued ahead
  of it have gone it's sent now or stays owed, and `mg_proto_capture_pump`
  (every 5 ms while owed) and every connection's ready (the SUBSCRIBE that
  completes it kicks the worker) send the level at that moment, nothing if
  it's the level the client last saw. Logged `speaker_volume N reported
  later (owed: no client)` / `(owed: the link is busy)`, then `speaker_volume
  N reported (owed)` when it leaves. `test_mg_ble` (`test_spk_owed`: a busy
  stack, a full outbox, a disconnect with one owed, a change while
  disconnected, no duplicate, the client's write not echoed). Subtitles turn on only with the client's state 1, in a call or
  not (a muted push-to-talk reply too): words, reply_end and clock while the
  mode is off are dropped and counted, state 0 and clear take the band off
  at once, and a call's end turns the mode off. They need no token proof,
  but a central whose proof hasn't matched on a set-up gadget gets less:
  its subtitles (`mg_subs_event_t.proven` false) show only on a screen
  that's already on, and never wake it, keep it lit, hold the panel or
  reset the idle timer, so auto-sleep and the passcode's auto-lock run as
  usual. The owner's state 1 (a proven central's, or the console's) wakes a
  sleeping screen (a sleeping one draws no band; never while the passcode
  lock is up) and each words message keeps it on. **Nothing of the band
  draws over a card that asks for a press or a code** (the pairing confirm,
  the update prompt, the re-setup prompt, a passcode keypad:
  `muse_subs_card`, set by `muse_ui.c`'s `sync_card` every frame and as the
  card changes): the tall band is hidden at once, the video's band and the
  call's controls aren't drawn, a touch goes to the card, never the call,
  and the update prompt's confirm counts the band as covering the card
  (`muse_subs_band_drawn`), so no caption ever lies over a Confirm
  (`tests/test_subs_unproven.py`). A talk press's touch-down pauses the band
  with the reply's audio: a tap brings it back where the reply now is, the
  turn's claim drops the rest of that reply's subtitles, and a press nobody
  decided lapses after 1.5 s (`MUSE_SUBS_PAUSE_MAX_MS`). A word waits for
  its moment while its timing domain has a time (a frame on the panel, or
  this reply's clock: a reply's clock never carries over to the next); only
  a word with neither shows 2 s after it came (`MUSE_SUBS_LATE_MS`). Behind the text the band darkens the picture to 1/8 of its brightness on
  GUI screens and to at most 0.35 over video and UI streams
  (`MUSE_SUBS_SCRIM_*`, `muse_subs_core.h`), so pure white text keeps 7:1
  over the brightest pixel behind it. **Over video the band is 2 lines; with
  no video on screen it's the tall band**: an LVGL rounded panel at the GUI scrim with one label on
  the system layer (under the mic ring), laid out on LVGL's tick
  (`tall_tick`): the two-line band's bottom and width (on a round panel the
  bottom line's chord, one wrap width for every line, so nothing reflows as
  lines scroll up), growing as lines come (never shrinking mid-reply) up to
  just below the home's top row, or below the clock while it shows
  (`muse_home_clock_shown`, the home's own geometry); newest line at the
  bottom; lines that come at once scroll in one at a time,
  `MUSE_SUBS_SCROLL_MS` apart. Lines (no clock / clock): StopWatch and W175
  10 / 8 (28 px), Watcher 8 / 7. When the
  pool fills, the lines that scrolled away go first, whole
  (`muse_subs_keep_from`), so the lines kept never reflow.
  `tests/muse_subs_tall_harness.c` (`test_muse_subs.SubtitleTallBandTest`);
  the simulator renders it (`subs=TEXT`, `subs.more=TEXT`). Nothing of them draws
  while the passcode lock is up (a phone's call held behind the keypad
  included): the band and the video overlay stay dark, the words are kept,
  and the latest band shows once it's unlocked (`tests/test_subs_passcode.py`,
  the simulator's `dump=sys` and `passenter=DDDD`). `mg.subs` logs the mode
  (`state 1 (ms timing)`) and each band transition (`band on (client)`,
  `band off (state 0)`), never the words.
- **Live call:** `CONFIG_MUSE_GADGET_BLE_LIVE_CALL` (default on with the
  full UI) adds `mg_command_live_call`: Live call in Settings or the menu
  asks the client to start a voice call with the Muse, the screen goes
  black with only "Calling…" centered until the call's first picture
  ("Waiting for phone…" with no app ready; without video,
  the face with "Live m:ss" once live), on-screen controls (End call,
  volume − / + and a level bar) show over it, the face or the call's video
  for 3 s after the last touch. **Up, a tap away from them dismisses them
  at once**, anywhere on the picture
  that isn't a control, and a tap on a control acts and re-arms the 3 s
  (`muse_call_tap`); the touch that showed them never dismisses them on its
  own lift (a tap's, a drag's or a hold's), and a press away held 250 ms or
  more (not a tap) keeps them. **Where the picture isn't a talk surface they come up
  at the touch-down**: the call
  video and the calling screen always, and the call's face unless the touch
  lands on the home's hold region with Settings > Talk on Screen or Both
  (`muse_home_talk_at`; Button, the default on every call board, makes
  every touch a touch-down one). A drag or a hold keeps them up, 3 s from
  the last touch, and the lift is still a tap (it acts on a control, or
  dismisses them away from one, only if they were up at the touch-down). On the hold region with screen talk
  they wait for a tap's lift, and a hold (250 ms) talks with no
  controls flash (`muse_call_touch_*`). `mg.call` logs `call controls
  drawn on the screen|over video N ms after the touch-down` once per show
  (the simulator: 10-20 ms from the finger). They're one 300 x 156
  panel, the same pixels on every board with a 320 px screen or larger
  (`muse_call_controls.h`): its hit areas only grow their
  rows to 7 mm where a panel's pixels per mm need it, and − and + take taps
  out to the screen's side edges. A vertical swipe or the board's other buttons set the
  volume (only in the owner's call: an unproven central's call state shows the call screen but remaps no
  key, so power sleeps the screen and the speaker key toggles the speaker), and video the client streams during it shows without Phone camera
  and closes with the call (`components/muse/muse_call.c`). A call the gadget ends is over on the gadget
  at once, whether or not the phone answers. A capture the client's `start_mic` began before the call ends
  when the call starts, and its `stop_mic` is always taken. The console's `>call` / `>call=0` start
  and end one (`>call?` only reads: it never counts as activity); the component README has the details. A phone that holds the link leaves the Nordic
  UART no way to start one, so `>call.at=DELAY_S,LEN_S` (USB console, RAM only) arms a timed call
  before it connects: it starts DELAY_S after the next Live call client is ready, ends itself after
  LEN_S, and fires once; `>call.at=0` disarms, `>call.at?` reports (`muse_call_at.h`).
- **set_mic_gain:** the client's
  1-100 level has a fixed meaning: 50 is the board's tuned gain
  (`MUSE_MIC_GAIN_TUNED`, 30 dB); 1-50 map linearly in dB from 0 dB (the
  codec's lowest step) up to tuned, and 50-100 from tuned up to the board's
  no-clip ceiling (`mic_gain_speech_max_db`: the highest gain at which
  normal speech 30 cm away doesn't clip; tuned on a board with no headroom,
  every board until its 30 cm clip check, so there 50-100 all give 30 dB),
  in 3 dB steps, applied at once (muse_audio's next read: a running capture
  included, every capture format), for that connection's captures only (RAM,
  never NVS or the Settings gain); the user's or tuned gain again at
  disconnect and at every new connection (`mg_mic_gain.h`, `test_mg_ble`).
  Console words (`muse_mic_gain_word.h`), on USB
  and `!IN` over the Nordic UART, answered on `mg.bench`: `mic.gain?` (a
  diagnostic: the Settings gain, the connection's override if any, and the
  gain captures use) and `mic.gain=N` (automation: the Settings gain as the
  slider sets it, saved, 0 to 36 dB on its 3 dB steps). `test_muse_ptt_ui`
  runs the migration on `muse_settings.c` and the words' parsing.
- **Face:** boards with a display or status light take `assistant_state`
  from the client: listening, thinking after push-to-talk, then responding
  (speaking), done (happy, then idle) or error, as the Wi-Fi path does, and
  speaking while a client's audio plays; idle after 120 s without an update
  (tool calls can keep a turn quiet that long).
  Face changes are logged on `mg.face` (on the Nordic UART mirror). Only
  boards with a vibration motor (the StopWatch) list `haptics_enabled` and the
  `ptt_buzz_*` settings; `request_status` lists every accepted setting.
- **Bench client:** `tools/mg_ble_client.py` (needs a C compiler and
  `python3 -m pip install -r tools/requirements-bench.txt`, bleak, cryptography
  and smpclient at the versions it's tested with) is a client for a
  Mac or Linux box:
  `scan`, `test` (Device Information, Battery, request_status, settings, a
  start_mic capture decoded to `mg_capture.wav`, queue status, and a 2 s
  playback on boards with a speaker), `play` (`--tone HZ --secs N`,
  `--sweep`, `--wav FILE`; `--volume N` sets the speaker level for the run
  only, RAM only through `!IN vol=N`, put back after;
  `--loopback` records the mic while it plays and checks the tone comes
  back, saving `mg_loopback.wav`), `ptt` (records
  each button press to a WAV, then sends responding and done and prints the
  face changes; `--no-states` skips that), `state <name>` (one
  assistant_state), `call` (the phone's side of a live call: answers the
  gadget's start with connecting then live and its end with ended;
  `--answer`, `--start`, `--state`, `--end-after`), `tools/av60.py` (Live
  call; the bench test AV60 on one call-video gadget:
  a `--secs 60` call started from the Mac, H.264 at the board's video size, fps
  and kb/s (`--size WxH --fps N --kbps N` override; two bands on the
  ESP-Mosaico) from the repository's H.264 test streams (`tests/vectors/h264`'s
  testsrc2 and UI clock) transcoded with ffmpeg and cached,
  two spoken replies (LC3 at 24 kHz where listed, 32 kb/s, a sweep and a tone
  at -18 dBFS; `--volume N` first, for the run only: `!IN vol=N`, put back
  at the end) and one muted reply (subtitle state 1 and
  words over the video), reply audio and video alternating on Data with
  change_data_type about 25 times a second as the app does; it reads the
  once-a-second `av video:` / `av play:` lines from the Nordic UART mirror,
  and a USB console with `--console PORT` (read-only, DTR/RTS untouched,
  115200), and prints a per-second table, PASS / FAIL per rule and a JSON
  summary; the clip is encoded before connecting, one timed sender writes
  only with write room and credits and logs what it sent each second and
  what it waited on (write-wait: the Mac; credit-wait: the gadget), and a
  sender that misses its schedule by > 5% in a 5 s window makes the run
  INVALID (exit 3, rerun) unless the gadget's credits caused it (a FAIL);
  every phase is bounded, so a stuck run ends with its phase named (exit
  2), and the jsonl is written as the run goes; `--dry` runs the whole flow
  against a gadget in the same process with its console on a pty, and
  `--evaluate LOG` re-checks a saved run), `setup [--tokens FILE --address ADDR] [--confirm-serial PORT]`
  (Link setup as the Muse app does it: community pairing v5, a press of the
  gadget's button (a set-up gadget: a 3 s hold, or Confirm on a touch
  screen), or `>pair.confirm` on its console with `--confirm-serial`,
  then token-only `provision_v2` with generated `bench-...` tokens, or real
  ones from `--tokens`: a file of two lines, access then refresh, readable
  only by its owner, or `-` for stdin, sent only to the gadget `--address`
  names (required with `--tokens`, as for `reprovision`);
  it keeps the proof key in `~/.mg_ble_client/`, mode 0600; a set-up gadget,
  found by its musegadgets advertisement, is set up again), `reprovision`
  (Gadget setup v2 on one connection: the kept key's proof, setup again, then
  the old key must fail and the new one match), `proof
  [--clear]` (the token proof with that key; `--clear` resets the gadget),
  `queue [--download] [--clear]` (it proves first; with `--download`,
  `--clear` clears only after every clip downloaded whole, and without it
  `--clear` erases every clip unfetched) and `log` (the Nordic UART
  mirror), `ui FILE` (a UI stream from `.au`, `.264` or
  two-band `.mgframes` files, with the refinement ladder's timing; per-frame
  link time, received-to-shown and the device's decode times; `--md5
  --index index.json` checks every decoded picture; `--latency N`, `--hold
  SECS`), `touch` (turns touch events on and prints them), `hid --mode
  digitizer|abs|rel|off --square x,y,w,h [--fisheye] [--rel W,H]` (sends
  `hid_map`, shows `hid_status`; once HID is on, macOS asks to pair on every
  connection: say no, or use a phone), `bench [--send SECS] [--recv SECS]` (raw throughput both ways with
  `device_action throughput_test`), and `images` / `dfu` (MCUmgr SMP updates
  on the Zephyr gadget and the ESP32 BLE-only builds; `dfu` proves with the
  saved key first; with `--unproven --address` it runs no proof, and the
  gadget's rc 10 asks for a press of its button, which `dfu` waits for,
  sending the write again about every second until it's admitted (rc 11: no
  press in time, or cancelled); the image goes over BLE unencrypted, so one that
  holds an SDK token is refused unless `--allow-token-image`, and never sent
  with `--unproven`; best effort: it finds a plain `mgst_` token or a Zephyr
  token slot that isn't blank, not a token stored any other way). `test` round-trips every setting the board lists, checks the others
  are refused, and runs the token proof when `setup` left a key for the
  gadget. It prints Link setup records and the gadget's log through
  `tools/redact.py` (a secret shows as its length and a SHA-256 prefix), and
  advertised names and Nordic UART lines with every control character but
  newline and tab shown as `\xNN`. On
  macOS the terminal needs Bluetooth permission; a sandboxed agent
  usually can't use Bluetooth.
- **Automated setup tests:** Link setup waits up to 60 s for the button
  after `confirm_required`; on a rig the USB serial console presses it, so
  setup runs with nobody at the bench:
  - Boards with the full UI (`components/muse`, the BLE-only C6 build
    included) print `@pair.pending` when a confirmation starts waiting. Bench
    builds (`MUSE_BENCH=1`, which turns on `CONFIG_MUSE_PAIR_CONFIRM_CONSOLE`)
    take the console line `>pair.confirm`: the talk button's setup press
    (`muse_link_talk_press()`), never push-to-talk. They answer
    `@pair.confirm confirmed`, or `@pair.confirm none` when nothing waits.
    Other builds answer `@pair.confirm disabled`: the Muse Gadget setup
    contract allows a simulated press only in test builds, so a shipping
    build is confirmed with the button alone. Flash rig boards with
    `MUSE_BENCH=1 tools/muse/board.sh flash <board>`.
    The `d` / `u` keys are push-to-talk only and never confirm. The Zephyr
    gadget takes the same line in every build: bench builds confirm on it,
    release builds answer `@pair.confirm disabled` (`zephyr/AGENTS.md`, Bench
    console).
  - `tools/mg_confirm.py PORT` watches a console for `@pair.pending` and
    answers it (`--count 0 --secs N` keeps answering, `--log FILE` saves the
    console, `--now` confirms what waits now); start it, then run the Muse
    app or `tools/mg_ble_client.py setup` as usual. `mg_ble_client.py setup
    --confirm-serial PORT` does both in one process. A sandboxed agent that
    can use Bluetooth but not serial ports (or the reverse) runs the two as
    separate processes.
  - Both open the port without changing DTR or RTS, which would reset the
    board, and reopen it if the board restarts.
  - The console is the USB cable, so this asks for the same physical access
    as the button, like the console's other setup commands. Light and
    status-screen boards have no console input: their button stays the only
    way to confirm.
- **With live voice calls too** (`CONFIG_MUSE_REALTIME_VOICE`, above): each
  press is routed first (`mg_voice_route()`, by the routing policy). To the
  phone over BLE, or into its queue, it's a musegadgets utterance, and no
  call starts; to Muse over Wi-Fi, the press does what it does on a build
  without BLE: a realtime call (held on the StopWatch, a tap elsewhere) or a
  voice note. Auto sends it to a phone that's connected, set up and has
  push-to-talk on, else to Muse. A press that doesn't go to Muse clears its
  call mark, so the StopWatch's blue button mutes again rather than hanging
  up. `tests/test_muse_ptt_ui.py` and `tests/test_muse_voice_call.py` run
  each route through the production voice loops.
- **The Wi-Fi path stays as it was.** The push-to-talk loops only call into
  `mg_voice.h` under `#if CONFIG_MUSE_GADGET_BLE_AUDIO`.
- **Test** with the host tests below: `test_mg_ble` (protocol, routing,
  queue, the token proof against `../protocols/test-vectors/mg-token-proof-v1.json`),
  `test_mg_session` (the device session against
  `../protocols/test-vectors/mg-session-v1.json`, on the BLE side alone and
  under `noise_control.cpp`'s link code, and the same envelopes over Wi-Fi;
  the second half needs cJSON from one `idf.py build`),
  `test_mg_token_proof` (the same on the device's PSA code; needs `IDF_PATH`
  like `test_link_pairing_handshake`), `test_link_gadget_setup` (Wi-Fi-optional
  `provision_v2`, the token-only commit and its rollback, BLE-only boot
  recovery, the Link flags byte, hello while set up, re-provisioning with
  each write failing and the old setup kept, a committed session refusing
  another `provision_v2` while a failed one may retry, the re-provisioning
  journal on `config_store.c` with power loss at each step, the press
  confirming only an armed setup, an abandoned setup leaving a set-up gadget
  as it was), `test_link_identity` (names: `MuseGadget-XXXXXX` from the
  Wi-Fi MAC, `MuseGadget-<PRODUCT>-XXXXXX` from the Bluetooth address or,
  on the ESP32 catalog boards' overlays, the Wi-Fi MAC; the length check), `test_mg_codecs` (SBC, liblc3 and lc3fx round trips, lc3fx and liblc3
  decoding each other's streams, every lc3fx file but its README against VENDORED.md's hashes), `test_mg_play`
  (playback: resampler, ring, decoders), `test_mg_ble_client` (the bench
  client's parsing and decoding, its settings and token proof
  against the firmware's protocol code, and its Link setup crypto against
  `tests/vectors/link_pairing_v5.json`, which needs `cryptography`; setup
  with `--confirm-serial` against a console on a pty; `reprovision`'s checks
  against the firmware's token proof code), `test_mg_ui` (`hid_map`, the HID
  report map parsed into its reports, the relative pointer's steps) and `test_mg_confirm`
  (`tools/mg_confirm.py` against a simulated console, and `>pair.confirm` /
  `@pair.pending` in both SDKs' firmware; its log is private and masked),
  `test_redact` (records with fake tokens through `tools/redact.py`, the
  client's errors, NUS lines and `log`, and the console tools) and
  `test_token_logs` (no firmware log line formats a token). Build the
  board with it on and one without it. BLE isn't emulated in QEMU; on a board,
  use `tools/mg_ble_client.py`.
- **Vendored codecs** (`../xplat/libsbc`, `../xplat/liblc3`) are
  Google's Apache-2.0 code, one copy shared with the Zephyr SDK;
  `components/libsbc` and `components/liblc3` only build them. Keep their
  `LICENSE` files and the modification notes in their headers, change them
  only through compile-time options both SDKs can choose, and never vendor
  LGPL code such as BlueZ's SBC. `../xplat/lc3fx` is the fixed-point LC3
  (SBC and LC3 on every gadget, lc3fx on chips without a fast FPU): a
  copy of the Muse Gadgets fixed-point LC3: a change goes in with its note
  and new hashes in `VENDORED.md`.

## Cable mirror

`MUSE_MIRROR=1 tools/muse/board.sh build|flash <board>` (overlay
`devices/sdkconfig.muse-mirror`, `CONFIG_MUSE_MIRROR`) adds the mirror in
`components/muse_mirror`. `tools/muse/mirror.py PORT` then shows the screen in
a desktop window over the console cable, the chip's own USB or a USB-UART
bridge: the mouse touches, the board's keys and typing go to the board, the
computer can play and record its sound, and Ctrl/Cmd+S and +R save
screenshots and recordings. `docs/mirror.md` is the guide,
`../protocols/MIRROR.md` the wire format.

- **Boards:** the full LVGL UI with PSRAM; not the FreeInk e-paper UI.
- **The port:** the viewer holds the console. Close it before flashing or
  `idf.py monitor`; it never resets the board.
- **Trust:** whatever is on the cable controls the board, as the console
  already does; there is no pairing or key.
- **Without the board:** `devices/sdkconfig.muse-virtual-display` builds a
  board with no panel, keys or codec for any ESP32-S3 with 8 MB of flash and
  octal PSRAM. Its size, shape and name are in menuconfig (Muse mirror); a
  name that holds a known model, such as "1.75C", gets that board drawn in the
  window. Its sound is the computer's.
- **ESP-Mosaico:** `tools/muse/mirror.py iris:DEVICE_ID` reaches it over ESP-Iris on
  its USB-C (`CONFIG_MUSE_MIRROR_IRIS`, `components_mosaico/muse_mosaico_iris`).
  The viewer then holds the Iris data port: close it before Vibe Mode installs.
- **Test** with `tests/test_muse_mirror*.py`; the viewer's need PySide6 and
  numpy (`tools/muse/requirements-mirror.txt`).

## WebAssembly apps

WebAssembly apps are an optional component, off by default. Enable them on
boards with the full LVGL UI and PSRAM using
`MUSE_APPS=1 tools/muse/board.sh build|flash <board>`
(`CONFIG_MUSE_WASM_APPS`, overlay `devices/sdkconfig.muse-apps`). On the
gadget they stay off until the person turns on Run apps in Settings.

Before changing or testing apps or their integration hooks, read
[the component instructions](components/muse_apps/AGENTS.md). They cover
the API, runtime, memory, capabilities, storage, input and focused tests.
For app authoring and the guide to send Muse, see
[the SDK README](components/muse_apps/sdk/README.md).

## Configuration gotchas

- `sdkconfig.defaults.esp32c5` is loaded after `sdkconfig.defaults` for C5
  builds only (ESP-IDF's target-specific defaults): it moves lwIP and Wi-Fi
  heap buffers to PSRAM, raises the TCP windows to 96 KiB with window scaling
  and deepens the receive path to match (96 dynamic RX buffers, a 96-entry
  receive mailbox, 12 queued out-of-order segments). Window scaling needs
  PSRAM required at boot, so a C5 whose PSRAM doesn't start resets instead of
  running without it, and Wi-Fi uses 4 static TX buffers in internal RAM.
  Other chips keep 16 KiB windows in internal RAM;
  `cmake/validate_config.cmake` enforces both.
- `sdkconfig.defaults.esp32` keeps ESP-IDF's 64 KB internal DMA reserve on
  the classic ESP32, which isn't offered `CONFIG_HOMEHUB_DMA_RESERVE_KB`.
- `CONFIG_HOMEHUB_ERROR_REPORT` (on in `sdkconfig.defaults`) sends Muse one
  `link.error_report` after a panic, watchdog, brownout, power-glitch,
  CPU-lockup or eFuse reset (`../protocols/ERROR_REPORT.md`).
  `main/error_report.c` keeps the record in RTC memory that survives a reset,
  writes nothing to flash, and notes the fault from the panic wrapper in front
  of ESP-IDF's handler (`muse_crashrec_panic_hook`, called from
  `components/muse_restart/muse_crashrec.c`'s, after the crash record). Anything added to the report
  must stay within that document's "What it must never carry". It needs the
  stock panic handler, so it is unavailable with
  `CONFIG_ESP_PANIC_HANDLER_IRAM`. A BLE-only gadget sends it over the device
  session, under the same rule: after the VM confirmed its registration.
- The BLE gadget profiles (every overlay that turns on
  `CONFIG_MUSE_GADGET_BLE_AUDIO`: the `-gadget` overlays, the C6's BLE-only
  one, `sdkconfig.muse-video-ble`) share three settings other boards don't
  take: both watchdogs on, a first-boot speaker volume of 50
  (`CONFIG_MUSE_DEFAULT_VOLUME`; Kconfig's 70 elsewhere) and an ATT MTU of
  exactly 247 (one link-layer packet per full write; `sdkconfig.muse`'s 256
  elsewhere). Other boards keep `sdkconfig.defaults`' watchdogs off (PSRAM
  cache stalls tripped them). In those profiles: the interrupt watchdog at
  1.5 s, and the task watchdog at
  15 s on both idle tasks and the gadget watchdog's users (`muse_wdt.h`), each
  ending in a panic and a restart. Don't turn them off or loosen them in an
  overlay (`tests/test_watchdogs.py` fails, and `cmake/validate_config.cmake`
  stops a musegadgets BLE build whose generated `sdkconfig` has them off or
  looser, as an old one does: delete it and build again); a board that trips them gets its
  cause fixed, or a tuned value with the evidence beside it. The restart
  leaves a crash record in RTC memory (`muse_crashrec.h`): the next boot logs
  `crash: task watchdog on core 0, 312.4 s up, pc 0x...; running X / Y;
  didn't reset the task watchdog: IDLE0 (crash 1 since power-on)` after its
  `boot:` line, and `guard?` repeats the last one. The boot guard
  (`CONFIG_MUSE_BOOTGUARD`) is on in the BLE gadget profiles (those
  overlays set it; the Kconfig default follows musegadgets BLE audio or
  the standalone profile): two crash resets within 10 minutes end in SAFE
  mode, never a loop. Other builds leave it off. To cause one on purpose (the hardware check), the USB console's
  `hang=task` (a top-priority spin on core 0: the task watchdog, 15 s) and
  `hang=int` (interrupts off: the interrupt watchdog, 1.5 s; refused in a
  build with that watchdog off), automation,
  never on the Nordic UART (`muse_restart_test_hang`). The crash record
  names each as its watchdog (`muse_crashrec_classify`: the task watchdog's
  panic reaches the panic handler as a fault, so its ISR's mark and reset
  hint decide).
- Each build's generated `sdkconfig` lives in its build directory
  (`build/sdkconfig`, `build-<board>/sdkconfig`). Once it exists, it overrides
  later edits to `sdkconfig.defaults` or the overlays. After changing those,
  delete the generated `sdkconfig` (or the whole build directory) and rebuild.
  `cmake/validate_config.cmake` stops the build if a stale config has the old
  TCP buffer sizes, the old cJSON nesting limit or ESP-IDF's internal DMA
  reserve (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`) on alongside
  `CONFIG_HOMEHUB_DMA_RESERVE_KB`, and warns if it still has the full root CA
  bundle.
- HTTPS trusts ESP-IDF's common root CAs only (`sdkconfig.defaults`), with
  cross-signed verification so chains that reach one of them through a
  cross-sign still verify. A server whose chain ends at another root (some
  `display.draw_url` image hosts, an OTA URL you host yourself) fails to
  connect. If you need one, select the full bundle in menuconfig
  (Component config > mbedTLS > Certificates > Certificate Bundle
  Configuration).
- Edit per-build settings with `idf.py -B <dir> menuconfig` (under "ESP32
  Device SDK"). Don't commit a generated `sdkconfig`: it's gitignored for a reason.
- The e-ink UI's task stack (`muse_eink`) must be in internal RAM: Settings'
  handlers write NVS synchronously, and flash writes disable the cache. A PSRAM
  UI task stack asserts in `esp_task_stack_is_sane_cache_disabled()` when
  changing a setting. The framebuffer and font caches can remain in PSRAM. Boards whose code and
  read-only data run from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`) keep the
  cache on during flash writes, so there a task with a PSRAM stack may write
  NVS. Mapping flash is different on every board: `esp_partition_mmap`,
  `spi_flash_mmap` and `bootloader_mmap` freeze the cache, and so does what
  calls them (`esp_ota_get_state_partition`, `esp_ota_get_boot_partition`,
  `esp_ota_mark_app_valid_cancel_rollback`, `esp_ota_set_boot_partition`,
  `esp_ota_end`, `esp_image_verify`, `esp_https_ota`): on a PSRAM stack that
  asserts (`s_task_stack_is_sane_when_cache_frozen`; the musegadgets worker's
  SMP healthy mark did). `CONFIG_MUSE_STACKS_IN_PSRAM` (`muse_stack.h`,
  on for the Waveshare S3 1.75C and the SenseCAP Watcher, their BLE-only
  video builds included, with `CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM`
  moving the captions' and musegadgets' statics there too) moves the stacks of app tasks that never map flash there
  (the Noise session, Muse's keeper, boot and Wi-Fi scan, the musegadgets
  player, the tunnel's send task); any task that can map flash keeps an
  internal stack: `muse_task_create()` for a task that
  never ends, `muse_task_slot_run()` for one that ends and starts again (it
  ends with `MUSE_TASK_END()`, never `vTaskDelete()`: a parked worker runs it,
  since a self-deleting PSRAM-stack task aborted on ESP-IDF v6.0.1; the rule
  stays on v6.0.3).
- E-paper boards with the full UI (`CONFIG_MUSE_UI_FREEINK`, the Sticky so
  far) draw with [FreeInk](https://github.com/Free-Ink/freeink-sdk), not
  LVGL, which they don't build at all. To bring up another panel: give it a
  FreeInk board profile (or reuse one), select `MUSE_UI_FREEINK` and name the
  profile in `MUSE_FREEINK_DEVICE` (`components/muse/Kconfig`), and write a
  board file that supplies `touch_read`, buttons, audio and power and points
  `display_pause` at `muse_eink_display_pause` (`eink/muse_eink.h`); leave
  `display_start` and the other display hooks NULL. The UI's fonts are
  embedded TTFs (`eink/fonts/`, regenerated by `tools/muse/gen_eink_fonts.sh`).
  FreeInk is pinned to a commit in `components/muse/idf_component.yml`.
- The BLE-only device session task (`mg_vm`, 8 KB) follows
  `CONFIG_MUSE_STACKS_IN_PSRAM` too, so `on_ws_command()` (run by both
  session tasks) never maps flash (`tests/test_muse_stack.py`), and the
  BLE-only Watcher's LVGL draw threads (`swdraw`) take 3 KB each
  (`CONFIG_LV_DRAW_THREAD_STACK_SIZE` in
  `devices/sdkconfig.muse-sensecap-watcher-video-ble`, which `board.sh`
  loads for `watcher-video` only; 1.7 KB used at worst, measured;
  `watcher` and `watcher-gadget` keep 8 KB until their Wi-Fi screens are
  measured): `>tasks`
  prints each task's free stack (the low-water mark) of its size.
- The StopWatch's BLE-only video build does the same
  (`devices/sdkconfig.muse-m5stack-stopwatch-video-ble`: `swdraw` 3 KB,
  1.7 KB used at worst, and `CONFIG_MUSE_STACKS_IN_PSRAM`, which the
  StopWatch's PSRAM code and data allow). Without them it idles at 18.6 KB of
  internal RAM free, largest block 7.9 KB, under the ~9 KB the video
  tasks take. `tests/test_muse_stack.py` holds the floor for both video
  builds (draw threads ≤ 3 KB, app stacks in PSRAM), so it can't regress
  silently.
- espressif/esp_lvgl_adapter 0.6.4's `esp_lv_adapter_pause()` marked
  the adapter paused and only then cleared any pending acknowledgement, so
  an acknowledgement the LVGL worker posted in between was cleared away and
  the pause waited for good (with -1; with a timeout it returned, but left
  the worker asleep in its paused wait, LVGL frozen). On a gadget that hung
  whatever paused LVGL (video's show task when a stream's picture starts, the
  screen-off path) until the task watchdog reset it. The build fixes it
  without touching the managed component: `cmake/esp_lvgl_adapter_fix.cmake`
  (included after `project()`) compiles a copy of its `esp_lv_adapter.c` from
  the build directory with the stale acknowledgement cleared before the mark,
  and a timed-out pause waking the worker. Each edit must match upstream's
  text exactly once and the adapter must be 0.6.4, or the build stops: **on
  an adapter version bump, re-check the pause handshake** and update or drop
  the fix (`tests/test_lvgl_adapter_fix.py` runs the edit on upstream's pause,
  `tests/vectors/esp_lvgl_adapter/pause_0.6.4.c`). On
  top of it nothing waits unbounded: every pause goes through
  `muse_lvgl_hold()` (video and every board's screen-off path; never call
  `esp_lv_adapter_pause()` directly), which waits `MUSE_LVGL_PAUSE_MS`
  (500 ms), retries once, then gives the pause up with LVGL left running and
  an error on `muse_ui` (the holder's own redraw repaints; the next hold tries
  again).
- OTA is off by default (`CONFIG_HOMEHUB_OTA_ENABLED=n`), except on boards
  with the full UI. A Muse Gadget build (musegadgets BLE audio) refuses
  every Wi-Fi OTA whatever the option (an image built for an S3 would pass
  the chip id and the dev key's signature): Link setup's `ota_url` and
  Link's BLE `device.ota` are ignored. Each
  refusal logs on `link.ota` with the offered version (when the request
  names one) and the URL's host and path, never its userinfo or query
  (`ota_url_for_log()`). Muse Gadgets update over BLE, from the Muse apps or
  a web updater (Firmware update over BLE, above).
  `CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD`
  (default n) takes them again, for this board's image only: its
  board description, read from the download's first bytes (the HTTP
  client's ON_DATA events, matched against ESP-IDF's own read of the app
  description), must name the same product code and board profile before
  `esp_https_ota_perform` (esp_ota_begin and the first write); ESP-IDF's
  chip id check (on the first 1 KB, before its first write) and the RSA
  check at the end stay (`tests/test_link_ota.py`). A Home Link build (no
  musegadgets BLE audio) reads those first bytes too and refuses an image
  for another device model (`CONFIG_MUSE_DEVICE_MODEL_ID`) before the first
  write, so one S3 Home Link board's image never lands on another; an image
  without a model (built before it) installs as before.
- Versions: `version.txt` holds the release number
  (`MAJOR.MINOR.PATCH`), the one place it's set. `cmake/version.cmake` turns
  it into each image's version: the number alone when HEAD is exactly the
  board's release tag (`<CODE>-vX.Y.Z`, CODE its
  `CONFIG_HOMEHUB_BLE_PRODUCT_CODE`; `vX.Y.Z` on a board without one) with no
  tracked changes, else `X.Y.Z+g<commit>` and `.dirty` with uncommitted
  changes, so no two images share a string. It's `PROJECT_VER`, so
  `esp_app_get_description()->version`, DIS Firmware Revision, Link
  `device_info` / `pairing_ready` (`firmware_version`), the boot log and
  Settings all show it. `-DPROJECT_VER=...` still overrides it. Tags are made
  at release (never by a build): bump `version.txt`, commit, tag
  `<CODE>-vX.Y.Z` for each board released from that commit. The OTA gate
  compares the numbers; with equal numbers any other string counts as newer,
  and a device on the old `999.0.0` placeholder takes any version
  (`tests/test_version.py`).
- BLE is shut down and its memory released once setup completes, except on
  boards with the full UI. `CONFIG_HOMEHUB_BLE_PERSISTENT` keeps NimBLE up
  beside Wi-Fi with a basic GATT service (`main/ble_basic.c`) advertised
  slowly; it costs roughly 40 KB of internal RAM for as long as the device
  runs (`tools/coexist/sdkconfig.ble-mem` wins most of it back on the C5).
  On the C5, `CONFIG_HOMEHUB_BLE_COEX` (`main/ble_coex.c`) keeps BLE
  advertising and connections from reserving half the radio's airtime once
  setup is complete, which doubles Wi-Fi throughput with BLE up. It relies
  on ESP-IDF's private coexistence status bits, so it builds on IDF v6.0.x
  only until the bits are rechecked on another version.
  `tools/coexist/` holds the overlays used to size it, and the data-path
  bench (`CONFIG_HOMEHUB_PIPELINE_BENCH`) with its results.
- With BLE persistent, `CONFIG_HOMEHUB_BLE_COMMANDS` (default on) lets Muse
  drive the Link's BLE: `ble.state`, `ble.power`, `ble.advertise`,
  `ble.scan`, `ble.connect`, `ble.disconnect`, `ble.discover`, `ble.read`,
  `ble.write` and `ble.subscribe` (`main/ble_commands.c` over the central in
  `main/ble_client.c`). They run one at a time on their own task. `ble.power`
  off stops the stack without releasing the controller's memory, so it can
  come back; the default build, which releases BLE after setup, can't.
  `tests/test_ble_commands.py` runs them against a fake client.
- On the C5 with PSRAM, `CONFIG_HOMEHUB_FAST_GCM` (default on there) wraps
  ESP-IDF's one-shot AES-GCM with `main/fast_gcm.c` for TLS and Noise. It
  takes about 19 KB of internal RAM (8-bit GHASH tables and the PSRAM staging
  buffer; see its Kconfig help). Turn it off to compare against IDF's;
  `tests/test_ghash32.py` checks the tables and its failure handling.
- The tunnel relays broadcast and multicast UDP between the VM and the LAN,
  takes the VM's `net.forward` (`CONFIG_HOMEHUB_NET_FORWARD`, default on) and
  reports the station's address in `link.register` (`main/tunnel_relay.c`,
  `docs/home-link-network.md`, which is the behavior the VM side builds on).
  NAPT's idle UDP time is
  `CONFIG_HOMEHUB_TUNNEL_UDP_IDLE_S`, which `main/CMakeLists.txt` compiles
  into ESP-IDF's lwIP, along with a 64-entry ARP table for `device.discover`
  (1,296 bytes of internal RAM) and an input hook
  (`main/lwip_hooks/tunnel_napt_guard.h`) that drops packets too short for
  NAPT's rewrite, packets from the LAN for the tunnel's subnet, and TCP for a
  TCP forward from outside the station's subnet. They rely
  on lwIP's own defaults and the hook's place before NAPT, so they're checked
  on ESP-IDF v6.0.x only, and the longer UDP time comes only with the hook: on
  another version the build warns and lwIP keeps its 2 s, its 10 entries and
  no check.
- Planned restarts go through `muse_restart(why)` (`components/muse_restart`),
  never a bare `esp_restart()`: it marks them in RTC memory so the next boot
  reads as a software restart, and a watchdog reset without the mark stays a
  real one (ESP-Iris counts those toward its crash loop). A reboot out of a
  failure stays `esp_restart()`. Each boot logs how it began (`link.main:
  boot: ...`); on the ESP-Mosaico `tools/muse/iris_bench.py boot` asks. The
  S31's `esp_restart()` comes back as an RTC watchdog reset with ESP-IDF
  master, so there `muse_restart()` ends in the ROM's system reset (the
  component's README).
- The codecs open only when their I2S DMA buffers fit
  (`components/muse/muse_audio_dma.h`): opening reallocates them when the
  format changes, and if that fails esp_codec_dev enables the channel anyway
  and it faults. Short of internal DMA memory, `muse_audio` logs `audio
  unavailable: N B DMA needed ...`, leaves voice off with everything else up,
  and tries again on the next wake (or read or write, at most once a second).
  Once open, the channels keep their buffers through rest (closing disables
  them, ESP-IDF reallocates only for a new size), so a wake reopening in the
  same format needs no new memory and is never refused for it (reallocating, the
  StopWatch on battery asked 12.5 KB more at every wake and stayed silent).
  A board whose `audio_init` already sets the channels up in that format
  says so (`i2s_ready_at_init`, the StopWatch): its first open needs nothing
  new either, so a unit that rested before any open succeeded still opens.
  Each refused open logs (at most once in 10 s), on the Nordic UART mirror
  (`muse_audio` is a default tag); a client's capture waits up to 1.5 s of
  closed reads for the codecs to open before it ends.
  A board that changes the I2S channels' `dma_desc_num` / `dma_frame_num` says
  so in `muse_board_t` (`i2s_dma_desc`, `i2s_dma_frames`).
- The mic ring (`components/muse/muse_mic_ring.h`): a red ring at the screen's
  edge while the mic captures for someone (push-to-talk once capture starts, a
  client's start_mic, a realtime voice call's open mic). The voice loops turn it on and off;
  `muse_ui.c` paints its spans into every area LVGL renders, at
  `LV_EVENT_FLUSH_START` (over everything, no draw tasks; toggling redraws
  only its band, PERFORMANCE.md), and `boards/muse_lcd_bands.c` paints it
  on every band drawn straight to the panel (video, UI streams). A new capture
  path calls `muse_mic_ring_set()`; a new panel path that bypasses both calls
  `muse_mic_ring_apply()`. A rectangular panel with rounded corners sets
  `corner_px`.
- The turn's status dot (`components/muse/muse_turn_dot.h`): during a turn's thinking (#E07BFF) and responding (#6FF0BF), a
  small breathing dot at the top center, just inside the mic ring, on every
  screen that doesn't show the avatar (Status, Settings, the button menu, a
  picture Muse drew, the phone camera, UI streams); nothing at listening
  (the ring) or outside a turn, never text, and nothing during a realtime
  voice call (its screens stay as the call has them;
  `muse_turn_dot_set_in_call()`). It follows `muse_state`'s mode, so BLE and Wi-Fi
  turns show alike. Painted like the ring: `muse_ui.c` at
  `LV_EVENT_FLUSH_START`, redrawing only its box (at most 12.5 times a
  second), `boards/muse_lcd_bands.c` on every band drawn straight to the
  panel, and the video view repaints its box over a still picture. A custom
  avatar whose thinking/speaking accents aren't the standard ones gets the dot over
  the face too. A new panel path that bypasses both calls
  `muse_turn_dot_apply()` beside `muse_mic_ring_apply()`.
- The screen timeout on USB (`muse_input.c` `check_sleep`): the same as
  on battery, with no USB keep-alive. Display > Sleep when charged (Always by
  default, kept in NVS as `sleep_chg`, device-local): at Never a lit screen
  stays lit while on USB with the battery charged, by the board's
  `muse_board_t.charged` (`muse_charged.h`; Firmware behavior notes
  below), and the usual timeout runs from when it lets go; it never lights a
  dark one. "Charged" is tracked whatever the setting
  (the first charger done since this plug-in, cleared only by unplug), so
  switching to Never on a gadget already charged holds at once, even while
  the charger tops up; the setting only decides whether it holds. A USB plug-in is a wake. The passcode's "screen stays on"
  (5 min idle lock) follows that hold or the timeout Never.
  Input scenario 20 in `tests/test_muse_ptt_ui.py` covers it.
- The panel during a turn (`muse_input.c` `check_sleep`): a turn the
  gadget started (its talk press; `muse_state_own_turn()`) keeps the panel
  lit through thinking and responding until done, and the sleep timer starts
  again at done or error. A client's start_mic wakes a dark panel as its
  capture starts (the ring in sight), and nothing sleeps while the mic
  captures; the phone's assistant_state alone never wakes a dark panel nor
  holds a lit one past its usual timeout (the user may be chatting on the
  phone). A reply on the gadget, a client's stream_audio reaching the
  speaker or subtitles' band on screen (`muse_voice_reply_holds_panel()`),
  holds a lit panel the same way until it ends, then the usual timeout runs
  from its end; it never wakes a dark one. Subtitles' state 1 wakes it
  (`muse_subs.c`); their words keep a lit one on (`muse_input_keep_lit()`)
  without waking a dark one.
- Don't move offsets in `partitions.csv`. `prod_data` and `prod_bak` are fixed
  manufacturing locations, and the table offset of `0x10000` leaves room for a
  larger Secure Boot bootloader. Check the `check_sizes` line in the build
  output: app slots are 2 MB (4 MB on Muse).

## Describe your device

Muse learns what a gadget is from its `link.register`, and how to use it from
its `device.guide` command; `../protocols/DEVICE.md` has the rules. Every
device built with this SDK registers `device_family: "gadget"`; other
families are reserved for Muse's own devices.

- **Model.** Each board's overlay sets `CONFIG_MUSE_DEVICE_MODEL_ID` and
  `CONFIG_MUSE_DEVICE_MODEL_NAME` after `CONFIG_IDF_TARGET`. The id names the
  hardware, so every profile of a board uses the same one; boards in
  `devices/` use `muse-gadget-<board>`. A build without them reports
  `muse-gadget` ("Muse Gadget"). Every build sends that id as `model_id`,
  and the build fails on a value outside the rules.
- **Guide.** `device.guide` returns the Markdown file `CONFIG_MUSE_DEVICE_GUIDE`
  names. `main/guide.md` covers boards with a status light or status screen,
  `devices/guides/muse.md` (set in `devices/sdkconfig.muse`) the boards with
  the full UI, and `devices/guides/voice.md` the Voice PE and the reSpeaker Lite and XVF3800.
  Give a board its own guide in `devices/guides/` when these would be wrong
  about it, and set it in its overlay. A relative path is looked up in the
  project directory, then in `esp32/`. A leading HTML comment (the license
  header) is left out, and a guide over 4096 bytes fails the build.
- **Guides in this repository** are generic to the board: what it is and what
  it can do, what it shows, its controls, recovery and limits, in at most 2048
  bytes. They skip setup and pairing, since Muse only sees a device that's
  already set up. They follow "Guides in this repository" in
  `../protocols/DEVICE.md`, and CI checks them.
- **Your own gadget's guide** is free-form: say what it's for and how you want
  Muse to use it. Keep the file anywhere in your project and point
  `CONFIG_MUSE_DEVICE_GUIDE` at it; only the 4096-byte limit is checked. For
  a desk light whose firmware adds `light.set` and `timer.start`:

  ```markdown
  # Focus light

  The lamp on my desk, used as a focus timer. "Focus" means 25 minutes of
  work: call `light.set` with red, then `timer.start` with 1500 seconds.
  The light turns green by itself when the timer ends. A press on the
  button ends a focus session early.
  ```

`tests/test_link_device_description.py` checks the model lines, that every
guide an overlay names embeds within the limit, this repository's guides
against the policy, and `link.register`'s size.

## Adding a command

Muse calls a gadget's commands by name: the firmware lists them in
`link.register` and answers each `link.invoke`. A command you add lives in two
places, which must use the same name. (`device.health` and `device.guide` are
handled in `noise_control.cpp` itself; everything else goes through
`on_ws_command()`.)

1. **Advertise it** in `build_register_json()` in `main/noise_control.cpp`:
   `add_command(commands, "relay.set", "<description>", required, optional)`.
   `required` and `optional` map each parameter's name to `{type,
   description}` (`string_param()` makes a string one). Muse reads the
   descriptions, so say what the command does and what it returns. If it can
   take longer than the default 30 seconds, set its `timeout_ms`, as
   `device.discover` does.
2. **Handle it** in `on_ws_command()` in `main/app.c`. Return
   `{"ok": true, "payload": {...}}`, or `command_error(code, message)` for a
   failure. Only `ok`, `payload` (or a `payload_json` string) and the
   error's `message` reach the Muse. Always return a result: `NULL` reaches
   the Muse as a generic "command handler did not return a result" error.
   Validate the parameters yourself: `params` is `NULL` when the request has
   none, and the firmware doesn't check them against the advertised
   `required` and `optional`, so check each one's presence, type, length and
   allowed values.
3. **Don't block.** `on_ws_command()` runs on the Noise session's task, so
   anything slow (the network, a slow sensor, a camera) belongs in its own
   task. Copy `request_id`, `session_generation` and every parameter the task
   needs, strings included, into memory the task owns (`device.discover`
   uses `cJSON_Duplicate()`): the request is freed as soon as
   `on_ws_command()` returns. If an allocation or the task start fails, free
   what you allocated and return `command_error()`. Otherwise start the task
   and return `{"_async": true}`. The task then calls
   `noise_ctrl_send_command_result()` once, failures included, or the Muse
   waits out the timeout. It takes ownership of the result and frees it, so
   don't free or reuse it afterwards. `camera.capture` and `device.discover`
   work this way. A result from an earlier session is dropped.
4. **Gate it on a Kconfig option** in `main/Kconfig.projbuild` when it needs
   particular hardware, and wrap both places in the same `#if`, as
   `sensors.read` does with `CONFIG_HOMEHUB_SENSECAP_SENSORS`. Add new source
   files to `main/CMakeLists.txt`.
5. **Keep `link.register` small.** It's printed into at most 8 KB, and a
   device whose registration doesn't fit never registers.
6. **Add a host test** in `tests/`. `test_link_sensecap_sensors.py` checks
   that `sensors.read` is advertised and dispatched under the same option, and
   runs its parser against a harness.
7. **Update the guides** (Describe your device, above) only if the command
   needs a rule its description can't carry.

Report environmental sensors (temperature, humidity, CO2, tVOC, light and so
on) through `sensors.read`, on every board that has them, so Muse has one
command to call wherever it runs. Don't add them to `device.health`, which is
for the link itself (battery, power, network), or give them a board-specific
command. Return each reading as `{value, unit, age_s}`, as
`sensecap_sensors.c` does, and report a sensor with no recent reading as null.
Read slow or I2C sensors in a background task and have `sensors.read` return
the latest value.

Muse sees the command once the board reconnects with the new firmware. Keep
the management commands that `on_ws_command()` also handles (`device.list_vms`,
`device.set_vm`, `device.reset_vm` and `device.unpair`) out of
`link.register`: `tests/test_link_transport_contract.py` checks they stay
unadvertised.

## Say Muse, never Hatch

Users never see the name Hatch.

- Anything a person reads says Muse, the Muse app, or the Muse's name:
  - screen text and error captions (`CAN'T REACH MUSE`, not `CAN'T REACH HATCH`)
  - settings labels
  - Kconfig prompts and help
  - log lines
  - tool and script output
  - docs
- Don't use `hatch` in a new file name or identifier. Use `muse` or
  `muse_gadget`. The Muse chat code is `components/muse/muse_chat*`.
- The ESP32 account clients use `https://api.muse.ai`, or the `api_url_v2`
  base the app sends during pairing, with bare API paths. They ignore
  `api_url`: only older firmware reads it, and that firmware adds `/hatch/`
  itself. Keep saving it so a device flashed back to older firmware still works.
  Pairing and the Muse host setting take only the hosts the Muse app sends
  (`components/muse/muse_server_hosts.h`); `CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS`
  accepts others in development builds.
- `hatch` stays only where the server or the Muse app depends on it. Don't
  rename these:
  - the VM host `hatch.metaaivm.com`
  - the `hatch_refresh:` auth prefix, the `hatch-web` app id and the
    `HatchLink/` user agent
  - pairing labels and ids such as `hatch-link-pairing-v%d`, the `hatch_link`
    model and the `hatch-link:` device id
  - the bug report fields
  - the setup commands (`hatch.token` and the rest) and the `"hatch"` key
    in the status JSON, which the app and older tools use
- Some older identifiers still carry the name (`muse_hatch_*`,
  `MUSE_HATCH_*`, `CONFIG_MUSE_HATCH`). Leave them unless you're asked to
  rename them. Don't copy the name into new code.

## Firmware behavior notes

Where the ESP32 firmware adds to or departs from the common gadget behavior:

- **Barge-in pause lapse:** the reply muted at a talk press's touch-down
  resumes on a tap and ends at the turn's claim; a pause that neither ends
  (a press the voice task never decided) lapses after 1.5 s
  (`MG_PROTO_BARGE_PAUSE_MAX_MS`), so a reply is never left stuck. An
  addition.
- Barge-in echo on the StopWatch, measured at volume 50: the first 300 ms
  +0.3 / +0.1 dB over the floor; the first 100 ms +0.4 dB with the
  I2S DMA clear (at volume 30 without it: +5.1 dB). Tap resume, from the
  firmware's counts: paused at the press, the DMA's unplayed samples kept
  and replayed, 0 underruns / dropped / repeated; whether the resume clicks
  acoustically hasn't been measured.
- **Bench guard (`ptt.minrise`):** an addition, off at
  every boot: a held turn's claim of the speaker waits for its release, and
  a held turn that never rose is treated like a tap for the barge-in pause
  (the reply plays on from where it paused). Its `stop_mic` is never sent: no
  `stop_mic` without the `button_down` that started the client's recording.
- **Passcode prompt fonts:** the keypad's prompt uses the panel's own type
  set, the closest font to the design's capitals: 1-2 px off the design
  on the ESP-Mosaico and the C6 1.8 (exact on the StopWatch and the
  Watcher).
- **Settings > Passcode on the C6 1.8:** "Change code" drops its icon where
  the label doesn't fit beside it.
- **Passcode and "Call from phone":** the call waits behind
  the keypad with no phase, so no picture, subtitles or mic; a sleeping
  screen wakes to the keypad, and a reply stream opened before the call
  ends (the call's own is busy until the unlock). Not yet
  verified on hardware: no phone audio plays and the mic stays closed before
  the unlock.
- **Sleep when charged, the % fallback:** "charged"
  is the charger's own charge-done report (`muse_board_t.charged =
  MUSE_CHARGED_DONE`, latched at the first done of a plug-in whatever the
  setting, and only unplug clears it; a failed read
  keeps it but holds nothing until a good reading shows USB still in, which
  holds again at once; a restart starts over): the AXP2101's REG01[2:0] == 4 (Waveshare C6 1.8 and 2.06, S3 1.75C
  and 1.75, S3 2.16, M5Stack CoreS3), the charger's status pin through the
  M5PM1 (StopWatch: the LGS4056's CHG_STAT released with USB and a cell;
  StickS3: its CHRG), the SenseCAP Watcher's STDBY, and on the ESP-Mosaico
  and reTerminal Sticky, whose chargers have no line to the SoC, the
  BQ27220's FC bit (its detection of the charger's termination by taper
  current). **These chargers have no done signal the
  firmware can read, so they keep the battery-% rule** (`MUSE_CHARGED_PERCENT`:
  charged at 90 %, no longer below 85 %, no latch): the **M5Stack Core2**
  (the AXP192's charging bit can't tell done from not charging), the **AIPI
  Lite** (its charge line can't tell done from unplugged), the **VN ESP32-S3
  1.83-inch NV3023** (its line only says charging), the **Freenove
  FNK0104B** and the **Waveshare ESP32-S3-Touch-AMOLED-1.85C** (no charge
  line). Boards with no battery reading or no USB sense (StickC Plus2, AI
  Passport, S3-Touch-LCD-1.28, and those without `read_power`) hide the
  setting and time out normally.

## Tests

These are host-side Python unittest harnesses that compile the firmware C/C++
against fakes, with no board or IDF environment required:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

To run each module in its own process under the freeze watchdog, as CI does, put
`python3 tools/stuck_watch.py --` in front. A module that runs well past its usual
time (1800 s until it has 5 passing runs on this computer) is stopped and
reported as STUCK, with what it was waiting on, instead of hanging the run.

The tests that compile cJSON find it by themselves (`tests/host_cjson.py`):
`CJSON_SOURCE_DIR` if set, else `managed_components/espressif__cjson/cJSON`
(any `idf.py build` or `idf.py reconfigure` here fetches it), else the newest
1.7.x in ESP-IDF's component manager cache. Without one they fail, saying how
to get it. Set `CC` or `CXX` to change compilers. Set `IDF_PATH` (activating
ESP-IDF does) so the real-crypto tests, `test_link_pairing_handshake` and
`test_mg_token_proof`, run instead of skipping.

The e-ink navigation and presentation tests use the pinned FreeInk source
(`tests/host_freeink.py`): `FREEINK_SOURCE_DIR` if set, else
`managed_components/freeink-sdk` from a Sticky configure. CI checks out that
exact pin for the host tests.

A test that feeds its driver input passes it through a temporary file
(`tests/host_child.py`): on macOS, feeding it through a pipe can deadlock
`communicate()`.

Some tests skip when an optional tool or library is missing (a C or C++
compiler, CMake, a wasm32 compiler, libx264 or libvpx, the PPK2 client, the
mirror viewer's PySide6 and numpy, pyserial); check the summary for
`skipped=`. Two need inputs you set up:

- `test_noise_core` links against the host's PSA Crypto library, found with
  `pkg-config mbedcrypto`. Install `libmbedtls-dev` and `pkg-config` on Debian
  or Ubuntu (as CI does), or `mbedtls` and `pkgconf` with Homebrew.
- `test_link_pairing_handshake` builds Mbed TLS from source for its
  real-crypto case, so it needs `IDF_PATH` (set by `export.sh`) or
  `MBEDTLS_SOURCE_DIR`.

## Before you hand back work

1. `idf.py build` (and the board build you touched) passes, with the size check
   under the slot limit.
2. The host tests pass. Before a push, run the whole suite on
   the exact commit with `tools/verify_commit.sh <commit>` (a clean worktree,
   the simulator, cJSON and ESP-IDF set up so nothing skips; `IDF_EXPORT` or
   `IDF_PATH` names ESP-IDF, `VERIFY_PY` a Python with the tests' packages; it
   prints every skip and fails on any, as on a setup failure; a simulator that
   doesn't build fails it). It covers the Zephyr and portable checks the
   ESP32 suite holds too, so run it for pushes that touch `portable/`,
   `protocols/` or `zephyr/` as well.
3. If you flashed, the boot log reaches `starting`, and the log shows no
   panic or reboot loop.
