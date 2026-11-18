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

# Mirror a Muse screen over its console cable

The opt-in mirror shows a Muse board's LVGL screen in a desktop window.
Use its drawn buttons or keyboard shortcuts, and click or drag the screen
on boards with touch. It also saves screenshots and records video.

## Build

Use ESP-IDF 6.0.3 and the usual board profile, with the mirror overlay:

```sh
MUSE_MIRROR=1 tools/muse/board.sh build s3        # Waveshare 1.75C, the chip's own USB
MUSE_MIRROR=1 tools/muse/board.sh build watcher   # SenseCAP Watcher, a USB-UART bridge
```

These use separate `build-muse-waveshare-s3-175c-mirror/` and
`build-muse-sensecap-watcher-mirror/` directories. Follow the normal board flashing
instructions for that image. For a manual `idf.py` build, append
`devices/sdkconfig.muse-mirror` to `SDKCONFIG_DEFAULTS`.

The feature requires the full Muse LVGL UI and PSRAM. It is off by default
and does not apply to FreeInk displays. It allocates two RGB565 screen copies
(`width * height * 2` bytes each: the live one and the snapshot an update is sent
from) and a 32 KiB message buffer in PSRAM, where the mirror task's stack is too. The first viewer that asks for deflated messages adds the ROM compressor's state and a second message
buffer, about 200 KB of PSRAM together. Allocation failure leaves the
regular UI running and logs `mirror unavailable`.

### Without the board

A virtual display runs the Muse UI on an ESP32-S3 with no screen: any module
with 8 MB of flash and octal PSRAM, such as a Seeed XIAO ESP32-S3. The UI
draws into PSRAM and the viewer shows it; touch, the two keys and sound go
through the viewer, the sound to and from the computer.

```sh
idf.py -B build-muse-virtual-display -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-virtual-display/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-virtual-display" \
  menuconfig build
```

Under Muse mirror, set the screen's width, height, shape and diagonal (466 x
466 round, 1.75" by default) and its name. A name that holds a known model is
drawn as that board: "Waveshare ESP32-S3-Touch-AMOLED-1.75C (virtual)" shows
the round Waveshare with its keys. Flash it as any ESP32-S3 build.

## Start the viewer

From `esp32/`, create a virtual environment and install its requirements:

```sh
python3 -m venv .venv-mirror
. .venv-mirror/bin/activate
python -m pip install -r tools/muse/requirements-mirror.txt
python tools/muse/mirror.py --list
python tools/muse/mirror.py /dev/cu.usbmodemYOUR_BOARD
```

On Windows, activate `.venv-mirror\Scripts\activate` and use a port such as
`COM5`. Linux ports commonly look like `/dev/ttyACM0`. Install rules or
serial-port group membership appropriate to your system if access is denied.
Close `idf.py monitor` and other readers of the same port first.

Discovery reads descriptors without opening ports and lists Espressif
native USB Serial/JTAG (`303a:1001`) and ESP-Iris boards. It does not identify the board model.
For USB-UART bridges, such as the Watcher's, pass the console
port explicitly. Use `--baud` if that firmware's console differs from
115200. The viewer asks the board to run the session at `--fast` baud
(460800 unless given; 921600, 230400, or 0 to stay at `--baud`), and the
console is back at its own speed when the viewer closes. No board reset is requested when opening or closing the viewer.

```sh
python tools/muse/mirror.py /dev/cu.usbserialYOUR_BOARD --baud 115200
python tools/muse/mirror.py COM5 --png screen.png
python tools/muse/mirror.py /dev/ttyACM0 --stats --seconds 10
```

With no ports, the viewer uses passive native USB discovery. Multiple
explicit ports appear side by side on a dark backdrop. `--scale 1.5` sets the initial
size, and `--log console.log` appends console lines.

## Controls

- Drag the body to move the window and its corners to resize it. Known
  boards are drawn from their makers' outlines; the SenseCAP Watcher from a
  generated mockup in `tools/muse/devices/`, with a small button over its wheel.
- Click a drawn button, or hold digits 1..4 for Talk, Auxiliary, Power and
  Speaker. Available buttons follow the board's metadata. A reset key is
  drawn where the board has one but cannot be pressed.
- Arrow keys, Enter and Escape send navigation on boards with a keyboard. Click or drag the active
  display on touch boards. The mouse wheel scrolls touch screens.
- Ctrl/Cmd+S saves each screen and a separate image of all drawn devices.
- Ctrl/Cmd+R starts or stops recording the drawn devices. Recording
  requires `ffmpeg` on PATH.
- Ctrl/Cmd+0 returns to actual size; Ctrl/Cmd+T toggles floating on top.

Screenshots and recordings go to Desktop, or the home directory if Desktop
does not exist. Menu commands expose the same actions. Preferences last
for the current run.

On a board whose console is the chip's own USB, or a UART bridge running
the session at 460800 baud or more, the speaker and microphone buttons route sound. The speaker button steps off, PC, BOTH; the microphone button off,
BOTH, PC:

| Button | Off | BOTH | PC |
|---|---|---|---|
| Speaker | The board plays | The board and the computer play | Only the computer plays |
| Microphone | The board's microphone | The board's and the computer's | Only the computer's |

The first click on the speaker turns on the computer's speaker and
microphone together. The computer plays with 300 ms in hand, and the board
waits for 60 ms of the computer's microphone before using it, so sound
carried between screen messages on a UART keeps playing.

The menu's "Sound: hear the board's microphone" plays what Muse records,
while it records. The computer's default input and output devices are used,
and the operating system may ask for microphone access. The buttons are
dimmed when the cable is too slow for sound.

When a visible text field is focused on the board, type into the viewer or
paste with Ctrl/Cmd+V. Digits enter text instead of pressing board buttons;
Backspace, Delete, Left and Right edit the field. Enter submits the compact
keypad through its normal Ready action. A pending multi-tap character is
committed before each desktop edit. The field retains its character and
length limits. Escape is consumed while a field is open.

Text entry follows the board under the mouse in a multi-board window.
Without an open field, digits and navigation keys return to board controls.
Typed text is sent over the same cable and is not written to console logs.

## ESP-Mosaico over ESP-Iris

The ESP-Mosaico's USB-C port belongs to ESP-Iris, which Vibe Mode installs
through, and its console is the UART on the header. Its mirror build
(`MUSE_MIRROR=1 tools/muse/board.sh build mosaico`, then the same with `flash`)
also carries the mirror over Iris (`CONFIG_MUSE_MIRROR_IRIS`). The viewer
opens the board's ESP-Iris data port itself, the second of the two serial
ports the USB-C shows, so no other software runs on the computer:

```sh
python tools/muse/mirror.py --list                 # iris:DEVICE_ID and its port
python tools/muse/mirror.py iris:DEVICE_ID         # or no target: every board found
```

The screen, touch, buttons, typing and sound both ways work as on the
cable. The viewer locks the port as the ESP-Iris Gateway does, so only one
of them holds it: close the viewer before Vibe Mode installs through it. While
a Gateway runs on the computer (it may be installing firmware, during which
the port goes away and comes back), the viewer says so and waits for it to
exit instead of opening the port. On an ESP-Mosaico the viewer got 24.7 screen
updates a second (31.2 KB/s, deflated) with the avatar on screen. Without
deflate it got 22.4 (383 KB/s), and a message to the board took 1.8 ms (90%
within 10.8 ms) while the screen streamed.

## Limits and checks

Native USB and UART use the same framing through Muse's console adapter.
UART throughput is limited by the baud rate. On a SenseCAP Watcher (CH342
bridge) with the avatar speaking, the viewer got 4.2 screen updates a second
at 115200 baud and 11.0 at 460800; nothing arrived at 921600, and the viewer
then steps down to 230400, then to the console's own speed. There is no fixed screen frame rate.
Framebuffer copying stops when the viewer disconnects and a reconnect
forces a full redraw. Holding keys is cancelled on disconnect or timeout.

Host checks, with the viewer requirements installed:

```sh
python -m unittest discover -s tests -p 'test_muse_mirror*.py' -v
```

The text-input test uses the pinned LVGL source an IDF configure fetches.
See [the wire protocol](../../protocols/MIRROR.md).
