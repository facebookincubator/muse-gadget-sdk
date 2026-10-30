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

# UI simulator

This is a desktop preview of the Muse interface in a 412 x 412 SenseCAP
Watcher window. It compiles the production `muse_ui.c`, state and text code,
and the avatar renderer. Like the firmware, it uses
`components/muse/avatar/muse_pixel.c` when present and the bundled default
otherwise. Adding or removing the custom file takes effect on the next build.
SDL supplies the display, mouse input, and timing while
small host adapters stand in for ESP-IDF, FreeRTOS, Wi-Fi, Bluetooth, Link,
settings, and power services.

The simulator is intended for fast UI work and repeatable screenshots. It does
not emulate the ESP32-S3 CPU, the Watcher's Himax camera, audio hardware,
Bluetooth radio, memory pressure, or power timing. Those paths still need a
firmware build and final testing on a device.

## Build

The simulator supports Linux and macOS. The host needs CMake 3.24 or newer,
Ninja, GCC or Clang with C11 support, and Python 3.9 or newer for the tests.
ESP-IDF is not required.

On Debian or Ubuntu, install the build tools with:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3
```

On a Mac, install the Xcode command-line tools and the remaining tools with
[Homebrew](https://brew.sh/):

```sh
xcode-select --install
brew install cmake ninja python
```

`xcode-select --install` can report that the tools are already installed. The
same build commands work on Apple silicon and Intel Macs and produce a native
binary; Rosetta and ESP-IDF are not needed.

From the repository root:

```sh
cmake -S esp32/simulator -B esp32/simulator/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build esp32/simulator/build --parallel
```

CMake uses a compatible system SDL2 when available and otherwise downloads
the pinned SDL 2.32.10 archive. By default it always downloads pinned LVGL
9.5.0 because the production UI needs version-specific private APIs and the
fonts and drivers enabled by this simulator's configuration. Set
`-DMUSE_SIM_FETCH_DEPS=OFF` only when both compatible system packages are
available and LVGL was built with that configuration. After one successful
fetch, CMake's
`-DFETCHCONTENT_FULLY_DISCONNECTED=ON` option reuses the populated dependency
cache without network access. Installing SDL2 with Homebrew is optional.

## Automated tests

After the normal build, run the deterministic headless tests with:

```sh
ctest --test-dir esp32/simulator/build --output-on-failure
```

CTest uses SDL's dummy video driver, validates command-line and scenario error
handling, renders every included scenario twice, and checks that each pair of
framebuffer captures is identical while different scenarios remain distinct.

For a checked build with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S esp32/simulator -B esp32/simulator/build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMUSE_SIM_SANITIZERS=ON
cmake --build esp32/simulator/build-asan --parallel
ctest --test-dir esp32/simulator/build-asan --output-on-failure
```

On macOS, these checks run without leak detection because Apple Clang's
AddressSanitizer runtime does not support it. Linux also enables leak detection.

## Interactive preview

Run this from a terminal in a logged-in Linux or macOS desktop session:

```sh
./esp32/simulator/build/muse_simulator
```

Mouse input acts as touch. The keyboard controls the common UI states:

| Key | Action |
|---|---|
| F1 ... F7 | Boot, idle, listening, thinking, speaking, error, and off |
| H | Happy idle animation |
| Space, held | Listen while held, then switch to thinking |
| `+` / `-` | Raise or lower the audio level |
| `[` / `]` | Lower or raise turn progress |
| S | Toggle sleep |
| P | Save `muse-simulator.ppm` in the current directory |
| Esc | Quit |

Run `muse_simulator --help` for the same controls and all command-line
options. On a MacBook, hold Fn or the Globe key while pressing F1 through F7
unless macOS is configured to use those keys as standard function keys.

For a quick manual smoke test:

1. Confirm that a fixed-size 412 x 412 `Muse Gadget Simulator` window opens.
2. Press F3 for listening mode, then use `+` and `-` to change the level meter.
3. Hold Space to listen and release it to enter thinking mode; the progress
   ring should become a moving segment.
4. Press F5 for speaking mode and use `+` and `-` to animate the mouth.
5. Press H and confirm that the avatar performs the happy animation.
6. Press S to sleep, then click the dark window to wake it.
7. Drag left across the window to open the settings placeholder and drag right
   to return to the avatar. Settings controls are not implemented in the simulator.
8. Press P and confirm that `muse-simulator.ppm` appears in the current
   directory, then press Esc to quit.

An included scenario can also initialize a visible interactive session:

```sh
./esp32/simulator/build/muse_simulator \
  --scenario esp32/simulator/tests/scenarios/pairing.txt
```

An interactive window requires a display session. Use `--headless` when
running over SSH or in CI.

## Scripted and headless runs

A scenario is a text file containing one `key=value` setting per line. Blank
lines and lines beginning with `#` are ignored. Settings are applied in file
order, so `advance` can render an intermediate state before the next change.

```text
face=thinking
caption=Finding a good answer...
progress=0.65
battery=72
usb=false
wifi=connected
ble=connected
paired=true
link=online
advance=800
```

Render it without a display server and save the final composited simulator
display as a binary PPM image:

```sh
./esp32/simulator/build/muse_simulator \
  --headless \
  --scenario esp32/simulator/tests/scenarios/thinking.txt \
  --run-ms 250 \
  --screenshot thinking.ppm
```

Supported scenario keys are:

- `face`: `boot`, `idle`, `listening`, `thinking`, `speaking`, `error`, `off`,
  or `happy`
- `caption`, `level`, `progress`, `battery`, `battery_mv`, `usb`, `charging`,
  and `asleep`
- `wifi`: `off`, `no_network`, `connecting`, `connected`, `failed`, or
  `not_nearby`
- `ble`: `off`, `advertising`, or `connected`; plus `passkey` and `paired`
- `link`: `boot`, `unpaired`, `pairing`, `confirm`, `connecting`, `online`,
  `offline`, or `error`
- `speaker`, `brightness`, and `advance` in milliseconds

Invalid options and scenario values return a nonzero exit status and identify
the bad line.

See [THIRD_PARTY.md](THIRD_PARTY.md) for the desktop dependency versions and
licenses.
