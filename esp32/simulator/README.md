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
Add `-DMUSE_SIM_EMOJI=ON` to show emoji in captions, as the firmware's
`CONFIG_MUSE_EMOJI` does.

## Automated tests

After the normal build, run the deterministic headless tests with:

```sh
ctest --test-dir esp32/simulator/build --output-on-failure
```

CTest uses SDL's dummy video driver, validates command-line and scenario error
handling, renders every included scenario twice, and checks that each pair of
framebuffer captures is identical while different scenarios remain distinct.

It also checks C6 bezel clipping against the original floating-point hook:
360 complete framebuffer comparisons across translations, indicator angles,
4/16/32/48/64-line strips, and both RGB565 byte orders. The integer fallback is
checked over every radius 1–1024. These are pixel/geometry regressions, not
measurements of C6 gesture FPS.

## The v1.0 screens and the ring's cost

`MUSE_SIM_BOARD=stopwatch|w175|watcher|mosaico|c6` picks the panel (466 round,
466 round with side keys, 412 round, 480 rounded square, 368x448). `render_screens.sh OUT_DIR`
renders every v1.0 screen of those four boards (home with and without the
clock, unpaired, mic ring, muted, captions, low battery, charging,
Status, Settings and its pages) into `OUT_DIR/<code>/`.
Scenario keys it adds:
`clock=HH:MM[/24]|none`,
`talk=both|screen|button` (Settings > Talk; the simulator starts on both, the firmware on button), `bench.out=true|false` (the bench switch on for the connected central: "Bench" in the top row), `volume=N` (as restored at boot: no change shown), `phone.volume=N` and `bench.volume=N` (a speaker change made from outside, the phone's set_settings or the console's `vol=N`: the gadget shows it), `device.volume=N` (the gadget's own change), `ssid=NAME` (Wi-Fi connected to NAME: the Status row's name, to
check its wrap), `page=status|settings[/<page>]`, `ring=true|false` (the mic
ring), `avatar_face=FILE.ppm|none` (the avatar face: a panel-sized P6
picture as the firmware draws it at either Size, which `tests/test_mg_face.py`'s
`render()` writes; Settings > Watch face appears),
`avatar_face.pick=avatar|pixel` and `avatar_face.size=large|small` (that page's
state), and `bench=N`: N full redraws with the ring off and on, fastest of five
rounds, plus the ring's spans alone per full frame (host CPU: the
ratio is what carries over to a device, not the milliseconds). `dotbench=N` (after `face=thinking` and a
page without Muse) times the turn dot's breath step, its box redrawn,
against full redraws, with the host's own flush taken out; on a busy host
the numbers swing, so read the best of several runs.

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
- `options`: the reply option picker's answers, separated by `|`
- `image`: fill the image view with a six-digit RGB hex colour, or `hide`
- `speaker`, `call` (preview the live-call layout), `brightness`, and `advance` in milliseconds

Invalid options and scenario values return a nonzero exit status and identify
the bad line.

See [THIRD_PARTY.md](THIRD_PARTY.md) for the desktop dependency versions and
licenses.
