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

# Devices

These are the boards the ESP32 Device SDK runs on. Each board is an `sdkconfig`
overlay: a short file of settings loaded on top of
[`../sdkconfig.defaults`](../sdkconfig.defaults) that picks the chip, flash,
memory, button, and how the device shows its status. The ESP32-C5 DevKitC-1 is
the default and needs no overlay.

Every board pairs with the Muse app, joins your Wi-Fi, and holds an encrypted
session to Muse. The rest depends on the hardware.

## Supported devices

| Board | Chip | Display | Flash / PSRAM | Reference | Buy |
|---|---|---|---|---|---|
| **ESP32-C5 DevKitC-1** | ESP32-C5 | None (RGB status light) | 8 MB / 8 MB | [Espressif docs](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp32-c5-devkitc-1/index.html) | [DigiKey](https://www.digikey.com/en/products/result?keywords=ESP32-C5-DevKitC-1) |
| **ESP32-C6 devkit without PSRAM** | ESP32-C6 | None (RGB status light) | 8 MB or more / none | [Espressif docs](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c6/esp32-c6-devkitc-1/user_guide.html) | — |
| **Espressif ESP32-S3-DevKitC-1 (N8R8)** | ESP32-S3 | None (RGB status light) | 8 MB / 8 MB | [Espressif docs](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html) | — |
| **ideaspark ESP32 with 1.9" display** | ESP32 | 1.9" 170×320 LCD | 16 MB / none | — | [Amazon](https://www.amazon.com/s?k=ideaspark+ESP32+1.9+inch+ST7789) |
| **Seeed SenseCAP Indicator** | ESP32-S3 | 4" 480×480 LCD | 8 MB / 8 MB | [Seeed wiki](https://wiki.seeedstudio.com/SenseCAP_Indicator_Get_Started/) | [Seeed Studio](https://www.seeedstudio.com/SenseCAP-Indicator-D1-p-5643.html) |
| **Seeed reTerminal E1001** | ESP32-S3 | 7.5" 800×480 black and white e-paper | 32 MB / 8 MB | [Seeed wiki](https://wiki.seeedstudio.com/getting_started_with_reterminal_e1001/) | [Seeed Studio](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) |
| **Seeed reTerminal E1002** | ESP32-S3 | 7.3" 800×480 six-colour e-paper (E Ink Spectra 6) | 32 MB / 8 MB | [Seeed wiki](https://wiki.seeedstudio.com/reterminal_e10xx_with_esphome/) | [Seeed Studio](https://www.seeedstudio.com/reTerminal-E1002-p-6533.html) |
| **Home Assistant Voice Preview Edition** | ESP32-S3 | None (12-LED ring) | 16 MB / 8 MB | [ESPHome repo](https://github.com/esphome/home-assistant-voice-pe) | [Home Assistant](https://www.home-assistant.io/voice-pe/) |
| **Waveshare ESP32-S3-Touch-AMOLED-1.75C** | ESP32-S3 | 1.75" 466×466 round AMOLED, touch | 32 MB / 8 MB | [Waveshare wiki](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C), [GitHub](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) | [Waveshare](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm) |
| **Waveshare ESP32-S3-Touch-AMOLED-1.75** | ESP32-S3 | 1.75" 466×466 round AMOLED, touch | 16 MB / 8 MB | [Waveshare wiki](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75), [GitHub](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75) | [Waveshare](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm) |
| **Espressif ESP32-S3-BOX-3** | ESP32-S3 | 2.4" 320×240 LCD, touch | 16 MB / 16 MB | [Espressif BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3), [ESP-BOX](https://github.com/espressif/esp-box) | — |
| **AIPI Lite** | ESP32-S3 | 128×128 LCD | 16 MB / 8 MB | [xiaozhi-esp32 board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/xorigin/aipi-lite) | [AliExpress](https://www.aliexpress.com/w/wholesale-aipi-lite.html) |
| **Waveshare ESP32-C6-Touch-AMOLED-1.8** | ESP32-C6 | 1.8" 368×448 AMOLED, touch | 16 MB / none | [Waveshare wiki](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-1.8) | [Waveshare](https://www.waveshare.com/esp32-c6-touch-amoled-1.8.htm) |
| **Seeed SenseCAP Watcher** | ESP32-S3 | 1.45" 412×412 round LCD, touch | 32 MB / 8 MB | [Seeed wiki](https://wiki.seeedstudio.com/watcher/), [GitHub](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware) | [Seeed Studio](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html) |
| **M5Stack Cardputer ADV (experimental)** | ESP32-S3 | 1.14" 240×135 LCD | 8 MB / none | [M5Stack docs](https://docs.m5stack.com/en/core/Cardputer-Adv) | — |
| **M5Stack StickS3** | ESP32-S3 | 1.14" 135×240 LCD | 8 MB / 8 MB | [M5Stack docs](https://docs.m5stack.com/en/core/StickS3), [M5Unified](https://github.com/m5stack/M5Unified) | [M5Stack](https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit) |
| **M5Stack StopWatch** | ESP32-S3 | 1.75" 466×466 round AMOLED, touch | 16 MB / 8 MB | [M5Stack docs](https://docs.m5stack.com/en/core/StopWatch), [M5Unified](https://github.com/m5stack/M5Unified), [factory firmware](https://github.com/m5stack/M5StopWatch-UserDemo) | — |
| **M5Stack CoreS3** | ESP32-S3 | 2" 320×240 LCD, touch | 16 MB / 8 MB | [M5Stack docs](https://docs.m5stack.com/en/core/CoreS3), [Espressif BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3) | — |
| **Guition JC3248W535** | ESP32-S3 | 3.5" 320×480 IPS LCD, touch | 16 MB / 8 MB | [JC3248W535C notes](https://github.com/sirisakG2/JC3248W535C), [JC3248W535-Driver](https://github.com/me-processware/JC3248W535-Driver) | — |
| **M5Stack StickC Plus2** | ESP32 | 1.14" 135×240 LCD | 8 MB / 2 MB | [M5Stack docs](https://docs.m5stack.com/en/core/M5StickC%20PLUS2), [M5Unified](https://github.com/m5stack/M5Unified) | [M5Stack](https://shop.m5stack.com/products/m5stickc-plus2-esp32-mini-iot-development-kit) (end of life) |
| **M5Stack Core2 (v1.0)** | ESP32 | 2.0" 320×240 touch LCD | 16 MB / 8 MB | [M5Stack docs](https://docs.m5stack.com/en/core/Core2), [M5Unified](https://github.com/m5stack/M5Unified) | — |
| **Freenove FNK0104B** | ESP32-S3 | 2.8" 240×320 LCD, touch | 16 MB / 8 MB | [Freenove repo](https://github.com/Freenove/Freenove_ESP32_S3_Display) | — |

## Features

| | DevKitC-1 | C6 devkit | ideaspark | SenseCAP Indicator | reTerminal E1001 | reTerminal E1002 | HA Voice PE | Waveshare S3 1.75C | Waveshare S3 1.75 | AIPI Lite | Waveshare C6 1.8 | Watcher | StickS3 | StickC Plus2 | Cardputer ADV | BOX-3 | StopWatch | CoreS3 | Core2 | FNK0104B | S3 DevKitC-1 | JC3248W535 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Home-network tunnel | ✅ | — | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Shows status on | Light | Light | Screen | Screen | E-paper | E-paper | Light ring | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Avatar | Light | Avatar |
| Images from Muse | — | — | ✅ | ✅ | Black and white | Six colours | — | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| UI and settings | — | — | — | — | — | — | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Experimental | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| Push-to-talk | — | — | — | — | — | — | ✅ | ✅ | ✅ | ✅ | Text replies | ✅ | ✅ | ✅ | Text replies (experimental) | ✅ | ✅ | ✅ | ✅ | ✅ | — | With an added I2S mic |
| Speaker and mic | — | — | — | — | — | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Buzzer and mic | ES8311 (experimental) | ✅ | ✅ | ✅ | ✅ | ES8311 | — | Speaker (NS4168), no mic |
| Air sensors | — | — | — | D1S, D1Pro | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — |
| Touch | — | — | — | — | — | — | — | ✅ | ✅ | — | ✅ | ✅ | — | — | — | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| Battery status | — | — | — | — | — | — | — | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Voltage only | — | — | ✅ | ✅ | ✅ | Voltage only | — | — |
| Over-the-air updates | Off | Off | Off | Off | Off | Off | Off | On | On | On | On | On | On | On | Off | On | On | On | On | On | Off | On |
| Buttons | BOOT | BOOT | BOOT | Top | Green | Green | Centre (talk), dial | PWR (talk), BOOT | BOOT (talk), PWR | Two | BOOT (talk), PWR | Wheel (press to talk, turn to sleep) | Front (talk), side (menu), PWR | Front (talk), side (menu), PWR | GO/Space (talk), Esc/Enter/arrows (menu) | BOOT/CONFIG (talk) | Yellow (talk), blue (sleep), PWR | PWR (talk), RST | Touch BtnB (talk), PWR | BOOT (talk) | BOOT | BOOT (talk) |

Boards without PSRAM (the ideaspark, the C6 boards and the Cardputer ADV) don't have room for
the home-network tunnel. Muse can still reach and control them once the
control session is up. The Waveshare C6 and Cardputer ADV also can't hold their own voice
session, so push-to-talk sends your voice note over its control session to the
Muse it's paired with, and the reply scrolls past as text. It can't show images either: the UI holds a whole image in
PSRAM, where the ideaspark draws one straight to its screen.

The SenseCAP Indicator's sensors hang off its RP2040, which passes the
readings to the ESP32-S3. The D1S and D1Pro have CO2 and tVOC sensors built
in, and temperature and humidity come from the Grove AHT20 in the box (plug it
in). Muse reads them all at once with `sensors.read`. This needs Seeed's stock
RP2040 firmware.

The reTerminal E1001's e-paper shows only black and white, 1 bit per pixel,
and keeps its picture without power. It shows a still status screen (the
agent's name, the character and a line of status text) that changes only when
the status does, since each refresh takes a second or two. Images from Muse are dithered
to black and white on the device and refresh once they are all in, with a
brief black-and-white flash. `display.draw_url` tells Muse the bit depth.

The reTerminal E1002 is the same board with a six-colour E Ink Spectra 6
panel: black, white, yellow, red, blue and green, 4 bits per pixel. It shows
the same status screen, with the character in colour. Images from Muse are
dithered to those six inks on the device (grays to black and white only),
and each refresh takes about 30 seconds and flashes. `display.draw_url` tells
Muse the six exact colours, the resolution and how slow it is.

The SenseCAP Watcher keeps its factory data (the identity SenseCraft uses) in
an `nvsfactory` partition at `0x9000`, where Muse puts its partition table and
NVS. It's unique to each Watcher, so back it up before you flash Muse the first
time. To return to Seeed's firmware, flash it and then write the backup back:

```sh
tools/muse/paced_esptool.py --chip esp32s3 -p PORT read-flash 0x9000 0x32000 nvsfactory.bin
tools/muse/paced_esptool.py --chip esp32s3 -p PORT write-flash 0x9000 nvsfactory.bin
```

The Watcher's CH342 USB bridge corrupts reads at 921600 baud and above, so
these use esptool's default of 115200. It also drops bytes when a whole packet
arrives at once, so plain esptool can't upload its stub or write flash
(`0107: Checksum error`, `0105: The format of the received message is
invalid`). `tools/muse/paced_esptool.py` takes esptool's arguments and sends 64
bytes at a time at the line rate; `tools/muse/board.sh flash watcher` uses it.

The M5Stack StickS3 has 8 MB of flash, so it uses its own partition table with
two smaller app slots. The front button is push-to-talk and the side button
steps through the menu, as on the AIPI. Powering off from Muse turns off the
screen, speaker and codec and puts the ESP32-S3 in deep sleep; either button
wakes it. Double-click the power button for a full power-off, and click it to
turn back on. Muse turns off the power chip's green LED, which would otherwise
stay lit. The IMU, IR and Grove port aren't used yet.
[M5Unified](https://github.com/m5stack/M5Unified) is M5Stack's reference
driver for the power chip and peripherals.

M5Stack ships the StickS3 with UiFlow2, which hands the ESP32-S3's USB to its
own driver and switches off the chip's USB serial port, so esptool can't find
it. The power chip drives the boot pin (GPIO0), so there's no BOOT button
either. To flash Muse the first time, put UiFlow2 in USB mode, open its REPL
(for example with `mpremote repl`) and paste:

```python
import machine; m = machine.mem32
m[0x600C001C] = m[0x600C001C] | (1 << 10)                 # USB serial clock on
m[0x600C0024] = m[0x600C0024] & ~(1 << 10)                # and out of reset
m[0x60038018] = 0x4200                                    # its pads on
m[0x60008120] = (m[0x60008120] | (1 << 20)) & ~(1 << 19)  # give it the USB pins
```

The REPL stops answering. Unplug the USB cable and plug it back in (the
battery keeps the stick running), and it shows up as a USB JTAG/serial port.
Anything that resets the stick now boots UiFlow2 again, so pass
`--after no-reset` to esptool until Muse is on. Back up the flash, then flash
Muse:

```sh
python -m esptool --chip esp32s3 -p PORT --after no-reset read-flash 0 0x800000 sticks3.bin
tools/muse/board.sh flash sticks3 PORT
```

Muse keeps the USB serial port on, so later flashes need none of this. To go
back to UiFlow2, write the backup:

```sh
python -m esptool --chip esp32s3 -p PORT write-flash 0 sticks3.bin
```

The M5Stack StopWatch has the same CO5300 round AMOLED as the Waveshare S3
1.75C, with a CST820 touch controller and the StickS3's ES8311 audio. The
yellow button (upper left) is push-to-talk and the blue one (upper right) puts
the screen to sleep; settings are on the touch screen. Powering off from Muse
turns off the screen, touch, audio and the expander's L3B rail and puts the
ESP32-S3 in deep sleep; either button wakes it. Double-click the power button
for a full power-off, and click it to turn back on. M5's IO expander (M5IOE1)
switches the panel and touch resets, audio power and the amp; its power chip
(M5PM1) reads the battery and the charger. The IMU, RTC, vibration motor and
Grove port aren't used yet. [M5Unified](https://github.com/m5stack/M5Unified)
and M5's [factory firmware](https://github.com/m5stack/M5StopWatch-UserDemo)
are the references. It enumerates as the chip's own USB serial port, so
flashing needs nothing special: `tools/muse/board.sh flash stopwatch`.

The M5Stack CoreS3 runs on Espressif's
[BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3),
which brings up its ILI9342C LCD, FT6336U touch, AW88298 amp and ES7210
microphones and switches the AXP2101's rails for each of them. It has no user
button: PWR on the left side is push-to-talk and pairing confirmation, read
from the AXP2101's key interrupts (its hardware power-off moves to a 10 s
hold), and settings are on the touch screen (swipe left from Muse). RST on the
bottom edge restarts the board, and holding it for 3 s enters the bootloader.
Powering off from Muse switches the AXP2101 off; click PWR to turn it back on.
Its 1 W speaker is quiet, so the volume starts at 100%. The camera, proximity
sensor, IMU, RTC, SD card and Grove ports aren't used yet. It enumerates as the
chip's own USB serial port: `tools/muse/board.sh flash cores3`. If esptool
can't connect, hold RST for 3 s, until the green LED lights, to enter the
bootloader.

The M5Stack StickC Plus2 runs the full UI on a classic ESP32, as does the Core2
below. It has
8 MB of flash and 2 MB of PSRAM, so it uses the 8 MB partition table too. The
front button is push-to-talk and the side button steps through the menu. The
power button wakes the screen, and holding it for 2 s powers off. There's no
power chip: the power button switches the stick on and the ESP32 keeps it on
(GPIO4). Powering off lets go of that, which cuts the power on battery. On
USB the stick stays powered, so the ESP32 also goes into deep sleep, and the
front or power button wakes it. Muse speaks through a small passive buzzer,
so replies are quiet. The battery shows its voltage, but the stick can't tell
Muse whether it's on USB or charging. The IMU, IR, RTC and Grove port aren't
used yet. [M5Unified](https://github.com/m5stack/M5Unified) and
[M5GFX](https://github.com/m5stack/M5GFX) are M5Stack's reference drivers
for the pins and peripherals.

The Plus2's console is a CH9102 USB-UART bridge, which drops out above
230400 baud, so `tools/muse/board.sh flash plus2` uses 230400. It comes with
M5Stack's factory firmware. Back up the flash before you flash Muse for the
first time:

```sh
python -m esptool --chip esp32 -p PORT -b 230400 read-flash 0 0x800000 plus2.bin
```

To go back, write the backup with `write-flash 0 plus2.bin`, using the same
chip, port and baud.

## M5Stack Core2 port

Core2 v1.0 (AXP192 PMU), using Espressif's `m5stack_core_2` BSP.
The 2.0" touch LCD shows the avatar UI: hold the middle bottom touch zone (M5's BtnB) to talk, navigate
the rest on screen. Holding PWR would cut the power in hardware past about
4 s, so the side power button is aux instead: tap to sleep or wake, and
hold for a clean shutdown on the PMU long-press event. Pairing presses are
confirmed by tapping the touch zone.
Settings, battery telemetry, images and the home-network tunnel all work.
Menu power-off shuts down through the PMU, and PWR turns the board back on.
Core2 v1.1 uses the AXP2101 PMU and is not supported by this port; the board's
back sticker or PCB identifies the revision. The IMU, RTC, vibration motor
and SD slot aren't used yet. [M5Unified](https://github.com/m5stack/M5Unified)
and [M5GFX](https://github.com/m5stack/M5GFX) are M5Stack's reference drivers
for the pins and peripherals, and the display, touch and speaker setup follows
Espressif's [BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_2).

The console is a CP2104 or CH9102F USB-UART bridge (early units the former,
later ones the latter), so `tools/muse/board.sh flash core2` uses 230400
baud, which both take. Back up the flash before you flash Muse for the first
time:

```sh
python -m esptool --chip esp32 -p PORT -b 230400 read-flash 0 0x1000000 core2.bin
```

To go back, write the backup with `write-flash 0 core2.bin`, using the same
chip, port and baud.

## Guition JC3248W535

The JC3248W535 (sold as JC3248W535C_I_Y and JC3248W535EN) is a 3.5" 320×480
IPS panel on an AXS15231B, driven over QSPI, with the controller's own
capacitive touch on I2C. In QSPI mode the AXS15231B ignores the row address,
so a write can only start at the top of the screen or carry on from the last
one: the board renders LVGL in direct mode into one full-screen buffer in
PSRAM and sends every frame whole. The panel's init sequence is the one
Arduino_GFX uses for this board.

It has an NS4168 I2S amp with a JST 1.25 connector for a 4-8 Ω speaker, but no
microphone. Push-to-talk records silence until you add an I2S MEMS mic such
as an INMP441: SCK to GPIO42 and WS to GPIO2 (shared with the amp), L/R to
GND, VDD to 3.3 V, and SD to a free GPIO, which you set as
`CONFIG_MUSE_JC3248W535_MIC_GPIO` under **Muse** in `menuconfig`. BOOT is the
only button: push-to-talk, pairing confirmation, and waking the screen. Power
off puts the chip in deep sleep until BOOT is pressed; there's no battery
gauge.

It enumerates as the chip's own USB serial port. Back up the stock firmware
before flashing Muse for the first time, and restore it with
`write-flash 0 jc3248w535.bin`:

```sh
python -m esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 jc3248w535.bin
tools/muse/board.sh flash jc3248w535 PORT
```

## Cardputer ADV port

Experimental ADV-only port, tested with ESP-IDF 6.0.1. Supports the display,
keyboard, BLE/Wi-Fi pairing, voice notes and text replies.

- Hold **Space/GO** to talk. **Esc** opens/closes the menu or goes back;
  **Enter** selects and confirms pairing.
- **Up/Down** (`;`/`.`) navigate; **Left/Right** (`,`/`/`) change values,
  with or without Fn. Typing chat messages is not supported.
- Menu power-off enters deep sleep; **GO** wakes it. Use the side switch
  for physical power-off.

Replies may be shortened; use the Muse app for the full conversation.
Spoken replies, images, the home-network tunnel, battery telemetry and
extra peripherals (SD, IMU, IR, expansion) are not supported.

Set your SDK token in `build-muse-m5stack-cardputer-adv/sdkconfig` (ignored
by Git). Build with `tools/muse/board.sh build cardputer-adv` and flash with
`tools/muse/board.sh flash cardputer-adv PORT`. To enter download mode,
switch off, hold GO while connecting USB, then release GO.

Back up the original 8 MB firmware before flashing; keep it outside Git:

```sh
python -m esptool --chip esp32s3 -p PORT read-flash 0 0x800000 cardputer-adv-backup.bin
```

Restore with `write-flash 0 cardputer-adv-backup.bin`. Pair in Muse under
Settings > Devices > Developer mode > Add Device, then press Enter.

## ESP32-S3-BOX-3

The BOX-3 port uses Espressif’s BSP for the LCD/touch hardware revisions and
the ES8311/ES7210 audio codecs. BOOT/CONFIG is push-to-talk and pairing
confirmation; settings use the touchscreen. The capacitive home button, dock
sensors, SD card, IR, and battery telemetry are not integrated. Power off
enters deep sleep; BOOT/CONFIG or RESET wakes the unit.

See [BOX-3 setup](esp32-s3-box-3.md) for PowerShell build and flash commands,
token setup, and the hardware verification checklist.

## Build

From the `esp32` directory, load `sdkconfig.defaults` first and then the
board's overlays, in order:

| Board | Target | Overlays after `sdkconfig.defaults` | Build |
|---|---|---|---|
| ESP32-C5 DevKitC-1 | `esp32c5` | none | `idf.py build` |
| ESP32-C6 devkit without PSRAM | `esp32c6` | [`devices/sdkconfig.c6-nopsram`](sdkconfig.c6-nopsram) | `tools/board.sh c6-nopsram build` |
| ESP32-S3-DevKitC-1 | `esp32s3` | [`devices/sdkconfig.espressif-s3-devkitc-1`](sdkconfig.espressif-s3-devkitc-1) | `tools/board.sh espressif-s3-devkitc-1 build` |
| ideaspark ESP32 | `esp32` | [`devices/sdkconfig.ideaspark`](sdkconfig.ideaspark) | `tools/board.sh ideaspark build` |
| Seeed SenseCAP Indicator | `esp32s3` | [`devices/sdkconfig.sensecap-indicator`](sdkconfig.sensecap-indicator) | `tools/board.sh sensecap-indicator build` |
| Seeed reTerminal E1001 | `esp32s3` | [`devices/sdkconfig.reterminal-e1001`](sdkconfig.reterminal-e1001) | `tools/board.sh reterminal-e1001 build` |
| Seeed reTerminal E1002 | `esp32s3` | [`devices/sdkconfig.reterminal-e1002`](sdkconfig.reterminal-e1002) | `tools/board.sh reterminal-e1002 build` |
| Home Assistant Voice PE | `esp32s3` | [`devices/sdkconfig.home-assistant-voice`](sdkconfig.home-assistant-voice) | `tools/board.sh home-assistant-voice build` |
| Waveshare S3 1.75C | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-waveshare-s3-175c`](sdkconfig.muse-waveshare-s3-175c) | by hand |
| Waveshare S3 1.75 | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-waveshare-s3-175`](sdkconfig.muse-waveshare-s3-175) | by hand |
| Espressif ESP32-S3-BOX-3 | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-espressif-box-3`](sdkconfig.muse-espressif-box-3) | `tools/muse/board.sh build box3` |
| AIPI Lite | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-aipi`](sdkconfig.muse-aipi) | by hand |
| Waveshare C6 1.8 | `esp32c6` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-waveshare-c6-18`](sdkconfig.muse-waveshare-c6-18) | by hand |
| SenseCAP Watcher | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-sensecap-watcher`](sdkconfig.muse-sensecap-watcher) | by hand |
| M5Stack Cardputer ADV | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cardputer-adv` | `tools/muse/board.sh build cardputer-adv` |
| M5Stack StickS3 | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-m5stack-sticks3`](sdkconfig.muse-m5stack-sticks3) | by hand |
| M5Stack StopWatch | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-m5stack-stopwatch`](sdkconfig.muse-m5stack-stopwatch) | by hand |
| M5Stack CoreS3 | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-m5stack-cores3`](sdkconfig.muse-m5stack-cores3) | `tools/muse/board.sh build cores3` |
| Guition JC3248W535 | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-guition-jc3248w535`](sdkconfig.muse-guition-jc3248w535) | `tools/muse/board.sh build jc3248w535` |
| M5Stack StickC Plus2 | `esp32` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-m5stack-stickc-plus2`](sdkconfig.muse-m5stack-stickc-plus2) | by hand |
| M5Stack Core2 | `esp32` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-m5stack-core2`](sdkconfig.muse-m5stack-core2) | `tools/muse/board.sh build core2` |
| Freenove FNK0104B | `esp32s3` | [`devices/sdkconfig.muse`](sdkconfig.muse), [`devices/sdkconfig.muse-fnk0104b`](sdkconfig.muse-fnk0104b) | `tools/muse/board.sh build fnk0104b` |

`tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]` builds each
board in its own `build-<board>` directory. For the boards with the full UI, run `idf.py`
with the target and overlays from the table:

```sh
idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" build
```

To flash, add `-p PORT flash` with the same arguments. Boards with the full UI need 16 MB
of flash or more, except the StickS3, StickC Plus2 and Cardputer ADV, whose overlays switch
to the 8 MB layout in [`partitions_muse_8mb.csv`](../partitions_muse_8mb.csv).
[`AGENTS.md`](../AGENTS.md) covers flashing, monitoring, and what to do when a
build picks up stale settings.

## Add a board

1. Copy the closest overlay here as `sdkconfig.<yourboard>`.
2. Set at least `CONFIG_IDF_TARGET`, `CONFIG_HOMEHUB_BUTTON_GPIO`, the status
   backend (`CONFIG_HOMEHUB_LED_BACKEND_*`), and the flash size and mode.
3. Without PSRAM, also turn off `CONFIG_SPIRAM` and `CONFIG_HOMEHUB_TUNNEL` and
   keep mbedtls in internal memory, as [`sdkconfig.ideaspark`](sdkconfig.ideaspark)
   does.
4. Build with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.<yourboard>"`
   and add your board to the tables above.

A board with a new kind of display, or one that runs the UI, needs code
as well. [`AGENTS.md`](AGENTS.md) walks through every kind of board, from the
overlay to testing on hardware.

Got it working on something new? Share it in the
[Muse Gadgets Discord](https://discord.gg/3bhjCkZdd6).

### Watcher camera

Camera support is disabled by default. In the Watcher build's `menuconfig`,
under **Muse**, enable **SenseCAP Watcher camera capture and live preview**
(`CONFIG_MUSE_WATCHER_CAMERA=y`) and rebuild. It requires PSRAM. Disabled
builds omit the camera worker, shutter UI, double-click gesture, and
`camera.capture` command.

Double-click the wheel to open a live camera view. Aim the Watcher, then tap
**TAP TO TAKE PHOTO** or double-click the wheel again. The captured frame stays
on screen. Tap the image to return to the avatar.

The `camera.capture` command returns a JPEG in
`payload.data_base64`, with `payload.format` set to `jpeg-base64`. During live
preview it returns the latest frame. Otherwise it requests a fresh frame.
Capture runs asynchronously and reports initialization, timeout, or busy errors.
The camera uses the existing Himax firmware. It's powered only while a capture
or the live view runs (a capture takes under a second, start-up included), and
nothing polls it in between.
Photo attachments to voice messages are not included.
