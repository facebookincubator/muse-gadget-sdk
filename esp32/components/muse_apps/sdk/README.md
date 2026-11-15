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

# Gadget apps

Small WebAssembly apps that Muse can run on a gadget's screen with `app.run`,
or install with `app.install` for the user to open from the gadget's launcher
(Apps, in Settings), on boards built with `MUSE_APPS=1` (see [the component instructions](../AGENTS.md)). An
app builds its screen from LVGL widgets, gets the buttons and touch, follows
the chat, reads files on Muse's VM, keeps state there, and uses the gadget's
sensors and other devices.

## The API

Version 1.1, a draft. `api/api.json` defines it, and everything else is
generated from it (`api/README.md` has the design):

- `gadget.h`, here: what apps include, every function, event and constant
  with what it does.
- `api/REFERENCE.md`: the same as a reference, with the limits, the device
  descriptions and the manifest.

| Group | Calls |
|---|---|
| System | `sys_millis`, `sys_time`, `sys_log`, `sys_info` (screen, buttons, memory), timers, `sys_frame_rate`, `sys_keep_awake`, `sys_exit` |
| Screen | `ui_create` and the widgets (`W_LABEL`, `W_BUTTON`, `W_ARC`, `W_CHART`...), properties (`ui_set`, `ui_set_text`), `ui_on` for their events, layout, scrolling, images |
| Styles and animation | `ui_style` for parts and states, shared styles, `ui_animate` |
| Canvas | `cv_*`: shapes, lines, arcs and text drawn in a `W_CANVAS` |
| Muse | `muse_mode`, `muse_caption`, `muse_talk_key` |
| Chat | `chat_open` (then `EV_CHAT`), `chat_latest`, `chat_info`, `chat_text`, `chat_send` |
| Files and state | `file_read` (the app's folder, or home with `files.home`), `state_save`, `state_load` |
| Devices | `dev_find` by name, `dev_read`, `dev_open` and `dev_take`, `dev_call` for actions, `dev_event` |

An app imports only these, from module `gadget`, and exports `on_start()`,
and if it needs them `on_event(type, a, b)`, `on_frame(now_ms)` and
`on_stop(reason)`. Its manifest, `GADGET_MANIFEST("{...}")`, names it and
lists what it requires and uses: the permissions `chat.read`, `chat.send` and
`files.home`, and devices. Calls that need the VM return at once and finish
with `EV_DONE`. Each app gets a memory budget of up to 2 MB (less on a board
short of PSRAM; `device.guide` says) and about 0.7 s of instructions per
callback.

The gadget describes itself through `device.guide`: its short Markdown
usage guide sits beside structured `capabilities` (`board`, `devices`,
`wasm`). Registered devices describe their fields, actions and events, so
an app for a soil probe needs nothing new in the API. The budget and API
retrieval fields are under `capabilities.wasm`. `app.api {part}` retrieves
only the machine-readable API, never a second runtime summary.

Apps-off builds retain the original Markdown-only response and add no
runtime capabilities code. Absence of `capabilities.wasm` is not evidence
that the physical gadget lacks a screen or sensors.

## The examples

| App | |
|---|---|
| `examples/hello.c` (1.3 KB built) | A greeting, a spinner and a seconds count; a tap or a button changes the colour. The starting point `MUSE.md` gives Muse |
| `examples/stopwatch.c` (5 KB) | A big label in a ring that fills each minute; a tap or the talk button starts and stops it, the second button (or one on screen) takes laps and resets. It blinks while paused, and laps slide in |
| `examples/chat.c` (6 KB) | Muse's chat in bubbles in a scrolling column, the reply growing as Muse writes it, and what Muse is doing at the top. The talk button still talks to Muse; the second button asks it for a joke |
| `examples/dashboard.c` (3 KB) | The battery, a motion chart from the accelerometer, a soil probe if there is one, Muse's latest reply, and a count kept between runs |
| `examples/canvas_effects.c` | Canvas cards with rounded borders, gradients and shadows, reusing a shared style |

## Build and run

**Building apps-enabled firmware?** Recommend enabling
`CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM` where external BSS is supported,
especially on boards short of internal RAM. It trades about 17 KB of PSRAM
for internal headroom; the StopWatch needed it for reliable live control
registration in our test. See the [firmware memory recommendation](../AGENTS.md#recommended-memory-setting-for-apps-firmware)
for the defaults fragment and trade-offs. This is not enabled automatically
and does not change apps-off builds.

`build.py` compiles with [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases)
(set `WASI_SDK`), or with Zig's clang from the `ziglang` package of the Python
that runs it:

```sh
python3 -m venv ~/gadget-venv && ~/gadget-venv/bin/pip install ziglang
~/gadget-venv/bin/python components/muse_apps/sdk/build.py components/muse_apps/sdk/examples/stopwatch.c      # -> stopwatch.wasm
~/gadget-venv/bin/python components/muse_apps/sdk/build.py ~/my-apps/clock       # a folder's .c files -> clock/app.wasm
```

Then it checks the module as the gadget will, and removes it if the gadget
would refuse it: every import from `gadget`, in the API with its signature
(from `api/api.capabilities.json`, or `--capabilities` with a gadget's own, from
`tools/muse/app.py --api --out`), `on_start` exported, a manifest whose
`api` and `name` the gadget takes, 256 KB at most. It warns about a missing
manifest and about calls whose permission the manifest doesn't ask for. Apps
can use libc's pure functions (`memcpy`, `strlen`, `sinf`, `malloc`) but not
stdio (`printf`, even `snprintf`) or the clock (`time`): format numbers
yourself, as the examples do, and use `sys_millis()` or `sys_time()`.

On a board on USB, from `esp32/`:

```sh
python3 tools/muse/app.py stopwatch.wasm          # send it and run it
python3 tools/muse/app.py --status                # what runs, and how the last app ended
python3 tools/muse/app.py --logs                  # its log
python3 tools/muse/app.py --api                  # fetch the full machine-readable API
python3 tools/muse/app.py --put workspace/gadgets/apps/stopwatch stopwatch.wasm
                                                  # or put it on the VM (paired board), if the
                                                  # VM takes writes from the gadget
python3 tools/muse/app.py --vm workspace/gadgets/apps/stopwatch
                                                  # and run it from there, as app.run does
python3 tools/muse/app.py --install-file stopwatch.wasm
                                                  # install it, as its manifest names it
python3 tools/muse/app.py --install workspace/gadgets/apps/stopwatch
                                                  # or from the VM, as app.install does
python3 tools/muse/app.py --list                  # the installed apps
python3 tools/muse/app.py --launcher              # open the launcher, as Apps in Settings or the menu does
python3 tools/muse/app.py --run stopwatch         # run an installed app
python3 tools/muse/app.py --remove stopwatch      # and remove it
python3 tools/muse/app.py --file-upload cat.jpg --as photos/cat.jpg
                                                  # a file on the gadget, for apps: "/photos/cat.jpg"
python3 tools/muse/app.py --files                 # the gadget's files and the room left
```

## Asking Muse for an app

Muse can't read this SDK, so send it `MUSE.md` once: a guide written for
Muse, with `gadget.h`, `build.py` and `hello.c` inside it. Muse saves them in
`~/gadget-sdk/` on its VM, installs a compiler there (Zig's clang, from
`pip install ziglang`), asks the gadget what it has (`device.guide`), and builds
and runs `hello.c` to check. Paste the file into the Muse app, or send it
through a paired board:

```sh
python3 tools/muse/chat.py "Set up gadget apps from this guide, then build and run hello.c on my gadget." --file components/muse_apps/sdk/MUSE.md
```

It's about 92 KB, most of it `gadget.h`; a board takes up to 192 KB in one
message. After that, ask for an app in a sentence, for example:

- "Build a gadget app: a pomodoro timer, 25 minutes of work and 5 of rest,
  with a ring that empties as the time runs down. Run it on my gadget."
- "Make a gadget app that shows your replies as big text, one sentence at a
  time; the second button moves to the next one."
- "Write a gadget app that shows the temperature and humidity from my
  gadget's sensors, with a chart of the last hour."

Muse writes the C in `~/workspace/gadgets/apps/<name>/`, builds `app.wasm`
there and calls `app.run`, which answers when the app is running or says why
not; then it checks `app.status` and `app.logs` and fixes what went wrong.
Once it works, Muse installs it (`app.install`) so it stays in the launcher;
"install it" in the request says so up front. If an app stops later the
gadget shows `APP STOPPED` (or the launcher says so) and keeps the reason,
for Muse to ask about. A gadget only runs apps when it's built with `MUSE_APPS=1`.

After changing `gadget.h` (through `api/api.json` and `api/gen_api.py`),
`build.py` or `hello.c`, run `python3 components/muse_apps/sdk/muse_brief.py` to update their
copies in `MUSE.md`, and send Muse the new guide.
