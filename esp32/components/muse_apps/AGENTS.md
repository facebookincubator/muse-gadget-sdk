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

# WebAssembly apps

Read [the ESP32 instructions](../../AGENTS.md) for building, flashing, token
handling and the shared firmware rules. Paths in backticks and commands below
are relative to `esp32/` unless stated otherwise. This file lives in
`components/muse_apps/`.

## Enablement and structure

`MUSE_APPS=1 tools/muse/board.sh build|flash <board>` adds
`devices/sdkconfig.muse-apps` (`CONFIG_MUSE_WASM_APPS`). Apps run on boards
with the full LVGL UI and PSRAM, replacing the face. They can be installed
for the launcher (Apps in Settings or the menu), get buttons and touch,
follow the chat, read files, keep state and use registered hardware.
They're off until the person turns on Run apps in Settings (or the menu),
kept in NVS; off, Muse isn't offered the app commands and they're refused.
Changing it restarts the gadget, so `link.register` follows it.
Kconfig keeps them from builds with the BLE commands (together they make
`link.register` larger than 8 KB) and from boards where an app's exit is out
of reach (EchoEar-2ST, Waveshare LCD-7, SmartRing-Plus).
[SDK README](sdk/README.md) covers app authors; [MUSE.md](sdk/MUSE.md) is the
self-contained guide to send Muse.

### Recommended memory setting for apps firmware

For apps-enabled builds on boards with external-BSS support, recommend
`CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM=y`, especially when internal RAM is
short. In a local firmware defaults fragment (after the board/apps overlays):

```text
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM=y
```

This moves the avatar/audio objects' zero-initialized statics (about 17 KB
with the current renderer) into PSRAM; initialized data and actual driver
DMA buffers stay internal. PSRAM is slower and cache-dependent, so verify
rendering/audio on the board. It also slightly reduces free PSRAM available
to apps; the StopWatch still had its full 2 MB app budget in our test.

The StopWatch live test needed this setting: without it, about 14 KB of DMA
heap remained, below Noise's 16 KB send reserve, and control registration
stalled. With it, about 32 KB remained and registration/discovery succeeded.
Keep the send reserve intact rather than reducing it to make a test pass.

This is a build recommendation, not a default: neither Kconfig nor
`devices/sdkconfig.muse-apps` enables it automatically. Keep any opt-in
fragment scoped to apps builds so apps-off images remain unchanged.

### Component structure

The component is optional. Without the option it builds nothing and its
manifest pulls in no interpreter. Link calls it only through `muse_apps.h`,
under `CONFIG_MUSE_WASM_APPS` in `main/app.c`, `main/noise_control.cpp` and
`main/muse_glue.c`.

The component's files:

- `muse_apps.c`: app task, lifecycle, events, operations (`EV_DONE`), timers,
  frames, memory budget, manifest, commands and console lines.
- `muse_apps_vm.c`: background VM requests over Link's session, including
  `app.run` downloads, reads and writes.
- API modules: `muse_apps_sys.c` (`sys_*`, `muse_*`), `muse_apps_ui.c`
  (`ui_*`: objects, properties, styles, animation, images),
  `muse_apps_canvas.c` (`cv_*`), `muse_apps_chat.c` (`chat_*`),
  `muse_apps_files.c` (`file_read`, `state_*`) and `muse_apps_dev.c` (`dev_*`).
- `muse_apps_store.c`: separate task that downloads, installs, removes and
  reads installed apps and does every flash write; `muse_apps_catalog.c`
  holds their plain-C catalog, tested on the host.
- `muse_apps_flash.c`: plain-C storage for the gadget's own files.
- `muse_apps_capabilities.c`: shared structured discovery for `device.guide`
  and API parts for `app.api`; device descriptions also serve `dev_describe`.
- `muse_apps_launcher.c`: a screen-sized LVGL list, with glyphs and colours
  in `muse_apps_icons.c`.
- `muse_apps_priv.h`: operations, budgeted memory, LVGL's lock and module
  start/stop hooks.
- `muse_apps_wamr_mem.c`: built into WAMR's library in place of its ESP-IDF
  memory-map layer, so linear memory is PSRAM in the app's budget.

CMake builds each module that exists, linking the component whole so imports
replace their weak `ERR_UNSUPPORTED` stubs in `muse_apps_stubs.c`. CMake checks
module files at configure time: after adding one, run
`idf.py -B <dir> reconfigure`.

WAMR and the component's own cJSON dependency are keyed on `MUSE_APPS_WAMR`,
defined on every target as `MUSE_BOARD_ID` is. A build without apps must keep
the loaded-section sizes it had before: compare against main. Everything
else apps need is gated by `CONFIG_CAPABILITIES_REGISTRY` or
`CONFIG_MUSE_APP_VIEW`, selected by the apps option. Keep new hooks outside
this component behind those guards too.

## API source and generated files

API 1.1 is defined by [sdk/api/api.json](sdk/api/api.json). Its
[change rules](sdk/api/README.md) require stable ids and constant values;
additions are minor versions, removals or incompatible changes are major.

`components/muse_apps/sdk/api/gen_api.py` validates the source and generates:

- `sdk/gadget.h`: the header apps include.
- `sdk/api/REFERENCE.md`: the reference.
- `sdk/api/api.capabilities.json`: what the component embeds and `app.api` serves.
- `muse_apps_api.h`: import table `MUSE_APPS_FUNCTIONS`, WAMR signatures,
  permissions, weak stubs, widgets, properties and well-known devices.

Change `api.json`, not generated files. From `esp32/`, run:

```sh
python3 components/muse_apps/sdk/api/gen_api.py
python3 components/muse_apps/sdk/muse_brief.py
```

`api.json` is authored, not extracted from the firmware. Change the native
implementation and tests with it. MUSE.md's prose is also authored: update
it when behavior changes; the script only replaces its existing BEGIN/END
code blocks, so it does not recreate a missing document.

The second command refreshes the copies of `gadget.h`, `build.py` and
`sdk/examples/hello.c` inside MUSE.md. The embedded example uses the standalone
`hello.c`/`gadget.h` layout delivered to Muse. Its `--check` fails on stale
copies or if the guide
exceeds the 192 KB accepted as one typed message (currently about 98 KB).

## App format, lifecycle and memory

An app imports only from `gadget`, exports `on_start`, and optionally
`on_event`, `on_frame` and `on_stop`. The `gadget.manifest` custom section
(`GADGET_MANIFEST()` in `sdk/gadget.h`) gives `api` ("1.0" or "1.1"), `name`
(the folder name), `title`, `requires` and `uses`. Permissions are
`chat.read`, `chat.send`, `files.home` and `files.gadget`. Device names in
`requires`/`uses` control which devices the app can find; unmet requirements
refuse the app. Without a manifest an app has no permissions or devices.

Apps live on the VM in `~/workspace/gadgets/apps/<name>/`; state goes in
`~/workspace/gadgets/devices/<gadget>/state/<app>/`, which only the gadget
writes. SDK examples live in `sdk/examples/`: hello, stopwatch, chat,
dashboard and canvas effects.
`sdk/build.py` uses wasi-sdk or Zig's clang (`pip install ziglang`), checking
imports and signatures against `api.capabilities.json`, exports and the manifest.

Each app's PSRAM budget is set at start: free PSRAM minus a 1 MB reserve for
the gadget, capped at 2 MB, refusing below 1 MB (`budget_now()`). It covers
WAMR code, linear memory and stack, module allocations (`muse_apps_alloc`)
and charges for LVGL objects (`muse_apps_charge`). `device.guide` and
`app.status` report `budget_kb`; apps read `sys_info(INFO_MEM_BUDGET)`.
All allocations go when the app stops. Queues, task stacks, the import
table and log stay in PSRAM: internal RAM is needed by Wi-Fi, Noise and
panel DMA, and `display.draw_file` needs about 30 KB free.

Callbacks are instruction-metered: 4 million instructions, or 16 million
for `_initialize` and `on_start` together. A runaway callback or trap ends
the app, shows `APP STOPPED` on the face (or a launcher note) and keeps the
reason in `app.status`. An app with a start function, `__post_instantiate`
or `__wasm_call_ctors` (which WAMR would run before the limit applies), or
callbacks not as `gadget.h` declares them, is refused. Objects' pixels are
clamped to 4096 and shadows, radii and dashes to 64 px, and an object takes
32 of the app's styles (shared ones plus `ui_style` selectors). Other counts
and limits are in `api.json`.

Apps come from the person's own Muse, which writes them and asks before it
makes one: they're trusted like Muse's commands, and WAMR's interpreter keeps
each to its own linear memory. Natives check the app pointers and lengths
they're given; there's no confirmation on the gadget and no narrower API.

## Screen, drawing and input

The shared UI's `components/muse/muse_app_view.c`, under
`CONFIG_MUSE_APP_VIEW`, owns a full-screen LVGL root above the face. Apps
put objects under it and receive touch, gestures and role-based buttons.
Power, a voice call or the video view stops the app. An app opened from the
launcher returns there.

A 2 s primary/talk hold closes the app or launcher (`exit_hold` in
`device.guide`). The app sees press and long press, then `on_stop`, never the
release. `muse_talk_key(1)` returns that button to Muse's recording; such an
app needs an on-screen Close control or `app.stop`. The apps code must not
contain board-specific behavior: consume screen/button descriptions and
the capability registry, not board names or pin maps.

The launcher is a vertical list of icon/name rows and Close, inside the safe
square on round screens. Touch scrolls, taps open, and long presses ask to
remove. Secondary steps down; primary opens on release. A primary long
press asks to remove if secondary exists to cancel; holding on to 2 s
closes instead. Keyboards use arrows, Enter and Esc.

The apps overlay enables LVGL PNG/JPEG decoders (`CONFIG_LV_USE_LODEPNG`,
`CONFIG_LV_USE_TJPGD`; no progressive JPEG). Decoded images and canvases
share a pixel limit equal to the screen's pixel count. `ui_image_load`
fetches on its own PSRAM-stack thread. Fonts and decoders add about 320 KB
of flash compared with no-apps firmware.

Canvas buffers are RGB565 and budgeted. `cv_rect_style` (API 1.1) paints a
shared style's fill, two-colour gradient, border and shadow. Shadow/border
widths and shadow spread are bounded to 64 px. Drawing is clipped to the
canvas buffer; leave space for shadows. Changing a style affects subsequent
calls, not already painted pixels.

## Capability registry

`components/capabilities_registry` describes local sensors, motors and
other hardware. Drivers register `capability_t` descriptors with names,
readings/units/rates, actions/parameters, events and nonblocking callbacks.
Well-known names from `api.json` must match their exact fields, actions and
events. Other devices describe themselves, with no new WASM imports.

`dev_*` reaches the registry; `device.guide` exposes it to Muse. Common
capabilities register in `components/muse/board_capabilities.c` (battery
on boards with `read_power`). Board files register other hardware under
`CONFIG_CAPABILITIES_REGISTRY`; chip drivers belong in their board's CMake
block. Apps know only the registry. `tests/test_apps_dev.py` checks
board-defined descriptors against well-known definitions.

## VM requests and storage

`chat_*`, `file_read` and `state_*` queue work to `muse_apps_vm.c`'s
background task (PSRAM stack). It runs requests in order, using at most two
of Link's three slots: a chat subscription plus another request. Requests
carry `x-request-id` and `x-app-id`; refusals log status and the start of the
body. State files are written whole in one request. Chat holds eight
messages (about 70 KB of the budget), with one unread `CHAT_TEXT` at a time.

Installed apps use the 2 MB `apps` LittleFS partition, up to 24 apps. Each
`<sha256>.wasm` is written before `catalog.json` names it; the catalog is
replaced by rename. Names/icons/colours default from the manifest, with
optional install overrides and a SHA-256 check. Installed VM folders are
assumed to be `workspace/gadgets/apps/<id>/`. Unpairing removes all apps.

The 4 MB `files` partition follows `apps`. `files.put` streams VM files up
to 3 MB into flash, hashes them and optionally checks the requested SHA-256.
`files.list` and `files.remove` manage them. Apps read paths starting with
`/` through `file_read` and `ui_image_load`, with `files.gadget` permission,
offline too. The store task writes; a separate reader task serves reads so
they don't wait behind downloads. Stacks use PSRAM where XIP keeps caches
on during flash operations, otherwise internal RAM. Unpairing wipes files.

Both partitions are in every 16 MB Muse table, even without apps: OTA keeps
the installed table. A board with an older table keeps apps in memory until
restart and has no file storage; 8 MB tables have neither partition.

## Commands and bench tools

- `app.run {path}`: download up to 256 KB with `GET /fs/raw/` Range requests,
  validate and run with WAMR's interpreter (no AOT/WASI). Paths are relative
  to VM home: an app folder containing `app.wasm` or a `.wasm` file.
  `app.run {id}` runs an installed app. Replies on start or failure.
- `app.stop`: stop after `on_stop`. Another run, power, the exit hold, calls
  and video also stop an app.
- `app.install`, `app.list`, `app.remove`: manage installed apps.
- `app.status`: running app, memory/frames and last exit reason/trap.
  `app.logs`: app `sys_log` output plus runtime notes.
- `device.guide`: its Markdown plus `capabilities` (`board`, `devices`,
  `wasm`) in apps-enabled builds. Apps-off keeps its original response.
  `wasm.enabled` reports compiled support, not runtime readiness; a failed
  runtime initialization still makes app commands return `unavailable`.
- `app.api {part}`: only the machine-readable API, in 6 KB parts. `part`
  is required; start at 0 to learn the `parts` count. It returns `part`,
  `parts`, `text`, not board/device/budget information. Runtime discovery
  belongs exclusively to `device.guide`.
- `files.put`, `files.list`, `files.remove`: manage gadget files.

From `esp32/`, `python3 tools/muse/app.py APP` sends and runs a local app
via USB. `--vm PATH` runs a VM app; `--install PATH`/`--install-file APP`
install, with `--id`, `--name`, `--icon`, `--color` overrides. `--run ID`,
`--remove ID`, `--list`, `--launcher` and `--key` drive the remaining app
controls. `--files`, `--file-put PATH`, `--file-upload FILE` (USB up to
256 KB), `--file-remove NAME` drive files. `--put PATH APP` writes to the
VM with gadget credentials. `--stop`, `--status`, `--logs`, `--api`
(`--out api.capabilities.json` for `build.py --capabilities`) and `--event TYPE,A,B` support
bench checks. `include/muse_apps.h` lists console lines, including
`app.press=talk|second,down|up` for simulated button edges.

## Focused verification

From `esp32/`, using a Python with `ziglang` or setting `WASI_SDK`:

```sh
python3 -m unittest tests.test_wasm_apps tests.test_app_api \
  tests.test_apps_ui tests.test_apps_dev tests.test_apps_vm \
  tests.test_apps_capabilities tests.test_app_store tests.test_app_files
```

These check generated API/header consistency, examples, rejected modules,
module wiring, memory placement and accounting, capability descriptors,
chat parsing/storage, catalog/files (with sanitizers) and guide freshness.
Check skipped tests: app compilation is skipped without a WASM compiler.

Build the affected apps firmware and a no-apps image; keep the latter's
loaded-section sizes identical to the baseline. Clear `managed_components/`
and `dependencies.lock` between board configurations. If flashed, verify
boot reaches `Muse Gadget starting` with no panic or reboot loop. Keep
token-bearing configs, images and logs in protected storage outside the
checkout, as the parent instructions require.
