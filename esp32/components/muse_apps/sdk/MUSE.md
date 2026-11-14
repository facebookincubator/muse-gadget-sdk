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

# Building apps for my Muse gadget

This guide is for you, Muse. My gadget can run small apps that you write: you
write an app in C, compile it to WebAssembly on your VM, and tell the gadget
to run it, or install it so I can open it myself from the gadget's launcher.
The gadget downloads it over its own link to your VM and runs it on its screen
in place of its face, with LVGL widgets, its buttons and touch, its sensors,
and the chat. Below: a one-time setup, the loop for making an app,
what an app can do, and three files to save.

My gadget only runs apps once I've turned on Run apps in its Settings (or its
menu): until then it doesn't offer you the app commands. If you don't see
`app.run`, ask me to turn it on.

This is the gadget apps API, version 1.1. `gadget.h`, at the end of this
guide, declares every function, event and constant, with what each does; read
it before writing an app. Where this guide and the gadget disagree, the
gadget is right: `device.guide` says what it has.

## One-time setup

1. Save the three files at the end of this guide in `~/gadget-sdk/` on your
   VM: `gadget.h` (the API), `build.py` (the build) and `hello.c` (a small
   app to start from). Copy them exactly.
2. Get a compiler. Your VM needs no system compiler: Zig's clang comes as a
   Python package.

   ```sh
   python3 -m venv ~/gadget-sdk/venv
   ~/gadget-sdk/venv/bin/pip install ziglang
   ```

3. Call `device.guide`, read its Markdown and keep `payload.capabilities`:
   `board` describes the screen/buttons, `devices` the registered hardware,
   and `wasm` the supported app API and current memory budget. Without
   `capabilities.wasm`, this build does not advertise WASM support; don't run
   an app. Refresh `device.guide` before relying on its budget. Fetch the
   API with the command named in `wasm.api_command` (`app.api`):
   `{"part": 0}`, `{"part": 1}` and so on up to
   `wasm.api_parts - 1`. Join each part's `text` in order with nothing between
   them and save it as `~/gadget-sdk/api.capabilities.json`. `build.py` checks
   apps against the gadget's own list of functions rather than `gadget.h`'s.
4. Build `hello.c` and run it, to check the whole path:

   ```sh
   mkdir -p ~/workspace/gadgets/apps/hello
   cp ~/gadget-sdk/hello.c ~/workspace/gadgets/apps/hello/
   ~/gadget-sdk/venv/bin/python ~/gadget-sdk/build.py ~/workspace/gadgets/apps/hello
   ```

   It prints the size of `app.wasm`, about 1.3 KB, and its SHA-256. Then call
   `app.run` with `{"path": "workspace/gadgets/apps/hello"}`. The screen
   should say "Hello from Muse" inside a spinner, with a count of seconds; a
   tap or a button changes the colour.

## Where things live

On your VM, under `~/workspace/gadgets/`:

- `apps/<name>/`: one folder per app, named as its manifest names it (a-z,
  0-9 and `-`). It holds `app.wasm`, which the gadget runs, its C source
  beside it, and any PNG or JPEG files the app shows (`ui_image_load` and
  `file_read` take paths relative to this folder).
- `devices/<gadget>/`: the gadget's own folder (`capabilities.wasm.device_dir` in `device.guide`).
  The gadget writes there, not you: an app's saved state is in
  `devices/<gadget>/state/<app>/`. Read it if it helps; don't edit it.

On the gadget itself, its own files: 4 MB of flash that `files.put` copies
files into from your VM (below), for apps to read without the network.

## Making an app

1. **Learn the board.** `device.guide` tells you the screen, the buttons, touch
   and the devices, each with its description. Write for what it says.
2. **Write** the app in C against `gadget.h`, in
   `~/workspace/gadgets/apps/<name>/<name>.c`, with a manifest naming the
   permissions and devices it uses (below). Start from `hello.c`.
3. **Build** it: `~/gadget-sdk/venv/bin/python ~/gadget-sdk/build.py ~/workspace/gadgets/apps/<name>`.
   It compiles every `.c` file in the folder into `app.wasm` there, then
   checks it as the gadget will, and deletes it if the gadget would refuse
   it: an import outside the API or with the wrong signature, no `on_start`,
   a manifest with a wrong `api` or `name`, more than 256 KB. Fix every
   warning too: a missing manifest or permission means calls that fail.
4. **Run** it: `app.run` with `{"path": "workspace/gadgets/apps/<name>"}`
   (relative to your home). It answers once the app is running, after its
   `on_start`: `{"ok": true, "payload": {"app": "<name>", "bytes": …,
   "fetch_ms": …, "load_ms": …, "memory_kb": …}}`. Or it says why not, in
   `error.code` and `error.message`:

   | Code | Means |
   |---|---|
   | `fetch_failed` | It couldn't read the file: no `app.wasm` in that folder, over 256 KB, or the gadget is offline |
   | `refused` | The manifest: not JSON, the wrong `api` or `name`, or it `requires` something this gadget lacks |
   | `invalid_app` | Not a module it can run, an import it doesn't offer, or no `on_start` |
   | `app_trapped` | `on_start` crashed or ran out of instructions; the message says how |
   | `out_of_memory` | Not enough memory free on the gadget right now |
   | `no_display`, `busy` | The screen isn't up yet, or the gadget is busy: try again |

5. **Check** it: `app.status` says whether it's still running and, once it
   has stopped, why (`last_exit`). `app.logs` has its `sys_log` lines and
   the gadget's notes about it (start, stop, traps).
6. **Fix** and go back to 3. Running it again replaces the old one.
7. **Install** it once it works: `app.install` with `{"path":
   "workspace/gadgets/apps/<name>"}` keeps it on the gadget, in its launcher
   (Apps, in its Settings), where I open it myself. Its id, its name in the
   launcher, its icon and its colour come from the manifest (`name`,
   `title`, `icon`, `color`; Design, below); pass `id`, `name`, `icon` or
   `color` to change them. It answers once it's installed: `{"ok": true,
   "payload": {"id": "<name>", "name": …, "icon": …, "color": …, "sha256":
   …, "bytes": …, "changed": true}}`. Installing doesn't run it: `app.run`
   with `{"id": "<name>"}` does. To update an installed app, build it and
   install it again: the same id with a new file. Keep the id the folder's
   name: an installed app reads its pictures and files from
   `workspace/gadgets/apps/<id>/`.

`app.stop` closes the app and brings the face back, as does a voice call
starting or the video view opening. The power key, or holding the gadget's
`exit_hold` button (in `device.guide`) for 2 s, closes it too, back to the
launcher if I opened it there.

Installing can also fail:

| Code | Means |
|---|---|
| `hash_mismatch` | The file isn't the one whose `sha256` you gave: build it again |
| `bad_id` | The id isn't 1 to 31 of a-z, 0-9, `-` and `_` |
| `bad_icon`, `bad_color` | Not one of the glyphs or colours below (Design) |
| `no_space`, `too_many_apps` | The gadget is full (24 apps, about 2 MB): ask me which to remove |
| `not_installed` | `app.run` or `app.remove` with an id that isn't installed: check `app.list` |
| `missing_param` | No `id`, and the app has no manifest naming it |

The apps stay installed until you remove one with `app.remove` or I remove
it on the gadget (hold it in the launcher), so check `app.list` before you
assume an app is there. Unpairing the gadget removes every app.

### The gadget's commands

| Command | Params | Answers |
|---|---|---|
| `app.run` | `path` (on the VM) or `id` (installed) | Once it runs: `app` (and `id`), `bytes`, `load_ms`, `memory_kb` |
| `app.install` | `path`; optional `id`, `name`, `icon`, `color`, `sha256` | Once installed: `id`, `name`, `icon`, `color`, `sha256`, `bytes`, `changed` |
| `app.list` | none | `apps` (each `id`, `name`, `sha256`, `bytes`, `icon`, `color`), `running` (an installed app's id, or null), `kept_after_restart`, `free_bytes` |
| `app.remove` | `id` | `removed`: whether it was installed |
| `files.put` | `path`; optional `name`, `sha256` | Once copied: `name`, `bytes`, `sha256`, `replaced`, `free_bytes` |
| `files.list` | none | `files` (each `name`, `bytes`), `files_bytes`, `free_bytes`, `total_bytes` |
| `files.remove` | `name` | `removed`, `free_bytes` |
| `app.stop` | none | `stopped`: whether an app was running |
| `app.status` | none | `state` (`running`, `launcher` or `idle`); while running `app` (and `id`), `source`, `running_s`, `frames`, `events`, `memory_kb`, `peak_kb`; `budget_kb`; `last_exit` with `app`, `reason`, `message`, `ago_s`, `peak_kb` |
| `app.logs` | none | `lines`: the newest log lines, oldest first, each led by seconds since the app started |
| `device.guide` | none | `format`, Markdown `guide`, and optional `capabilities`: `board`, registered `devices`, and `wasm` on apps-enabled builds |
| `app.api` | `part` (required integer) | Only that API part's `part`, `parts`, `text`; start at 0, then fetch through `parts - 1`. Join `text` in order. No runtime capabilities |

`last_exit.reason` is `user` (the power key, the exit hold or `app.stop`), `replaced`
(another `app.run`), `call`, `video`, `exit` (the app called `sys_exit`),
`update`, or `trap`, with WebAssembly's exception in `message`: out of
bounds memory, unreachable, integer divide by zero, or running out of
instructions.
After a trap the face shows `APP STOPPED` (or the launcher says it stopped,
if I opened it there), and nothing tells you unless you ask: check
`app.status` a few seconds after running an app, and after the person has
used it.

## How an app runs

- It's a WebAssembly module of up to 256 KB that imports only functions from
  module `gadget` (the ones in `gadget.h`) and exports `on_start()`. If it
  wants them it also exports `on_event(type, a, b)`, `on_frame(now_ms)` and
  `on_stop(reason)`. `GADGET_IMPORT` and `GADGET_EXPORT` in `gadget.h` do the
  naming.
- The gadget calls `on_start` once: build the screen there. Then `on_event`
  for each event (`EV_` constants: buttons, touch, timers, finished
  operations, the chat, Muse's mode, devices, the screen and the link), and
  `on_stop` before it closes the app. All on one task, one at a time. Changes
  to the screen made in one call show together when it returns.
- `on_frame` is for an app that animates something itself, up to 30 times a
  second while the screen is on (`sys_frame_rate` sets how often, 0 pauses
  it). Leave it out otherwise: widgets redraw themselves, `ui_animate` runs
  on the gadget, and `sys_timer` gives you ticks.
- Nothing blocks. A call that needs your VM or takes time returns an
  operation id at once, and `on_event(EV_DONE, id, result)` comes when it's
  done: `result` is 0 or more (bytes read, say) or an `ERR_` constant. A
  buffer you hand such a call must stay untouched until then: make it static.
- A call that fails returns a negative `ERR_` constant. Check the ones that
  can fail, and `sys_log` what went wrong: you'll read it in `app.logs`.
- The screen sleeps after the person's idle time; `on_frame` stops and
  `EV_SCREEN` says so, but events still come. An app meant to be watched (a
  clock, a timer) keeps it on with `sys_keep_awake(1)` while it matters.
- The app's memory goes when it stops. To keep something between runs, save
  it with `state_save` (in `on_stop` is fine: saves made there finish before
  the memory goes) and load it with `state_load` in `on_start`.

## The manifest and permissions

Every app starts with one line naming itself:

```c
GADGET_MANIFEST("{\"api\":\"1.0\",\"name\":\"plant\",\"title\":\"Plant\",\"icon\":\"leaf\",\"color\":\"green\",\"requires\":[\"soil_moisture\"],\"uses\":[\"chat.send\",\"battery\"]}");
```

- `api`: `"1.1"` (or `"1.0"` for apps that use no 1.1 calls). `name`: the app's folder name. `title`: for people, in the
  launcher. `icon` and `color`: its look in the launcher (Design, below).
- `requires`: features and devices the app can't work without; the gadget
  refuses to run it without them. Features: `touch`, `round`, `rect`,
  `buttons2` (a second button), `nav` (navigation keys or a knob).
- `uses`: permissions, and devices it uses when they're there. Permissions:
  `chat.read` (follow the chat), `chat.send` (send you messages),
  `files.home` (read files outside the app's folder), `files.gadget` (read
  the gadget's own files).
- An app can find only the devices its manifest names, and a call that needs
  a permission it didn't ask for answers `ERR_DENIED`. With no manifest it
  runs with neither.

## Devices

The gadget lists its own devices in `device.guide`, each with a description:

```json
{"name": "soil_moisture", "desc": "Capacitive soil moisture probe on the Grove port",
 "fields": [{"name": "moisture", "unit": "%", "min": 0, "max": 100}],
 "rate": 1, "actions": [], "events": []}
```

- Find one by name: `dev_find("soil_moisture")`, after naming it in the
  manifest. The well-known names in `gadget.h` (`DEV_TEMPERATURE`,
  `DEV_BATTERY`, `DEV_ACCEL`, `DEV_VIBRATION`...) have fixed fields, so
  `DEV_BATTERY_PERCENT` is where the percentage is. For any other device, take
  the field's place from its description, or ask: `dev_field(dev, "moisture")`.
- **Readings** are up to 8 floats in the order of `fields`, in a
  `gadget_reading_t` with the time it was taken. `dev_read` gives the latest
  at any time. To follow a device, `dev_open(dev, rate_hz)` (up to its
  `rate`), then on `EV_DEV_DATA` take what's queued with `dev_take`; close it
  with `dev_close` when the app stops needing it, as a running sensor costs
  battery.
- **Actions** take float parameters in the order the description gives, and
  text if it says `"text": true`: `dev_call(dev, dev_action(dev, "buzz"),
  params, sizeof params, "")`.
- **Events** a device reports by itself come as `EV_DEV_EVENT` after
  `dev_open`, `b` being the event's place in `events` (`dev_event` finds it
  by name).

A device the gadget doesn't have can't be found: check `dev_find`'s result
and leave that part of the app out rather than failing.

## The screen

The screen is LVGL's, through handles: create widgets once in `on_start`,
change their properties as things change, and ask for their events. The
gadget draws them and redraws only what changed.

```c
static int32_t count_label, plus;
static int32_t count;

GADGET_EXPORT(on_start) void on_start(void) {
    ui_set(UI_ROOT, P_BG_COLOR, 0x121214);
    count_label = ui_create(W_LABEL, UI_ROOT);
    ui_set(count_label, P_TEXT_FONT, FONT_48);
    ui_set(count_label, P_TEXT_COLOR, 0x5FA87D);
    ui_set_text(count_label, P_TEXT, "0");
    ui_align(count_label, ALIGN_CENTER, 0, -30);

    plus = ui_create(W_BUTTON, UI_ROOT);              /* a button with a label in it */
    ui_set(plus, P_RADIUS, RADIUS_CIRCLE);
    ui_style(plus, PART_MAIN | STATE_PRESSED, P_BG_COLOR, 0x5E7FA6);
    ui_align(plus, ALIGN_CENTER, 0, 50);
    ui_on(plus, UI_CLICKED);                          /* its clicks come as EV_UI */
    int32_t l = ui_create(W_LABEL, plus);
    ui_set_text(l, P_TEXT, SYM_PLUS " Add");
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b) {
    if (type == EV_UI && a == plus && b == UI_CLICKED) {
        char buf[12], *p = buf + sizeof buf - 1;
        int32_t n = ++count;
        *p = 0;
        do *--p = (char)('0' + n % 10); while ((n /= 10) && p > buf);
        ui_set_text(count_label, P_TEXT, p);
        ui_set(count_label, P_TRANSLATE_Y, -8);       /* a little hop */
        ui_animate(count_label, P_TRANSLATE_Y, 0, 300, EASE_OVERSHOOT);
    }
}
```

- **Widgets** (`W_`): containers, labels, buttons, images, lines, arcs, bars,
  sliders, switches, checkboxes, rollers, dropdowns, text areas with a
  keyboard, scales, spinners, charts, tables, and a canvas for pixels.
  `ui_create(type, parent)` with `UI_ROOT` or another object as the parent.
- **Properties** (`P_`) go through `ui_set` and `ui_get` (`ui_set_text` for
  text): size and place, layout, scrolling, and style. Style set this way is
  the main part's in the default state; `ui_style(obj, PART_ | STATE_, prop,
  value)` styles a part or a state (an arc's indicator, a pressed button). A
  shared style (`ui_style_create`, `ui_style_set`, `ui_style_add`) gives many
  objects one look.
- **Layout:** place with `ui_align`, or let a container lay out its children:
  `P_LAYOUT` `LAYOUT_FLEX` with `P_FLEX_FLOW` (rows or columns, `P_FLEX_GROW`
  to share space) or `ui_grid`. `PCT(p)` is a share of the parent, and
  `SIZE_CONTENT` fits the content. To centre a row's or column's children
  across it, set both `P_FLEX_CROSS_PLACE` and `P_FLEX_TRACK_PLACE` to
  `PLACE_CENTER` (cross placement alone centres them only within the widest
  child). A container whose content is taller scrolls; `ui_scroll_to_view`
  brings a child into view.
- **Events:** `ui_on(obj, UI_CLICKED | ...)` sends that object's events as
  `EV_UI` (`a` the object, `b` the event). A touch on the background, or on
  something not clickable, comes as `EV_TOUCH_DOWN`, `MOVE` and `UP` with the
  point; swipes as `EV_GESTURE`. Buttons come as `EV_BUTTON` by role
  (`BUTTON_PRIMARY` is the talk button), with `BTN_DOWN`, `BTN_LONG` and
  `BTN_UP`. The power key closes the app and never reaches it, and holding
  `BUTTON_PRIMARY` (the talk button, named `exit_hold` in `device.guide`) for 2 s
  closes it too: the app gets `BTN_DOWN` and `BTN_LONG`, then `on_stop`.
  Don't make an app depend on a hold of that button longer than that. If you
  give the talk button back to Muse with `muse_talk_key(1)`, the hold records
  for Muse instead: provide an on-screen Close control or use `app.stop`.
  On boards without touch, `ui_navigation(1)` lets the buttons or knob move focus
  between the app's buttons and inputs.
- **Animation:** `ui_animate(obj, prop, to, ms, EASE_...)` runs a property
  to a value on the gadget, with `ANIM_REPEAT` and `ANIM_REVERSE` for loops
  (a pulse, a blink): position, size, opacity, translation, an arc's value.
- **Text:** fonts are Montserrat at 14, 16, 20, 28 and 48 px (`FONT_14`...);
  `FONT_48` has only digits, `:`, `.`, `-`, `%` and the degree sign. LVGL's
  symbols (`SYM_`) work in the other sizes. Keep text to ASCII: the fonts
  may lack anything else (accented letters, curly quotes, emoji).
  `ui_text_width` and `ui_text_height` measure text before you place it;
  `ui_fit_text` fits a label to a width (Design, below).
- **Pixels:** a `W_CANVAS` and the `cv_` calls draw shapes, lines, arcs and
  text in it, for what widgets can't do (a game, a plot of your own). Its
  buffer counts against the memory budget. Since API 1.1,
  `cv_rect_style(canvas, x, y, w, h, style)` paints a shared style's fill,
  two-colour gradient, border and shadow. Create the style with
  `ui_style_create` and set `P_BG_COLOR`, `P_BG_GRAD_COLOR`, `P_BG_GRAD_DIR`
  (`GRAD_HOR` or `GRAD_VER`) and the `P_SHADOW_*` properties with
  `ui_style_set`. `P_RADIUS` rounds its corners; `P_OPA` and `cv_opa` multiply
  each paint opacity. Border and shadow widths and shadow spread are capped
  at 64 px. Leave space for the shadow inside the canvas: pixels outside it
  are clipped. Changing the style doesn't repaint old pixels; draw again.
- Pictures: `ui_image_load(img, "plant.png")` loads a PNG or JPEG from the
  app's folder; `EV_UI` with `UI_LOADED` or `UI_LOAD_FAILED` says when.

## The chat, files and state

- **The chat:** with `chat.read`, `chat_open()` follows the chat: the gadget
  fetches the latest message and then reports changes as `EV_CHAT`
  (`CHAT_MESSAGE` for a new one, `CHAT_TEXT` as your reply grows,
  `CHAT_DONE`, `CHAT_BUSY` while you work, `CHAT_CONNECTED`). It keeps the
  last 8 messages: `chat_latest(0)` is the newest one's id, `chat_info` its
  role (`ROLE_USER`, `ROLE_MUSE`, `ROLE_APP`), length and whether it's done,
  and `chat_text` its text, up to 4 KB of it. The gadget parses the chat's
  formats; the app sees messages.
- **Messages to you:** with `chat.send`, `chat_send(text)` posts to the chat
  as "[<app> on the gadget] text", so you can tell it from the person's own
  words; your reply comes back through `chat_open`. An app that messages you
  should show the person what it sent.
- **Talking to you:** `muse_talk_key(1)` leaves the talk button to your
  push-to-talk, as on the face; `muse_mode()` and `EV_MUSE` say whether
  you're listening, thinking or speaking, and `muse_caption()` what you
  heard.
- **Files:** `file_read(path, offset, buf, size)` reads up to 64 KB of a file
  in the app's folder, or from `"~/"` in your home with `files.home`. So an
  app can show what you write for it: put the data in its folder (a JSON
  file, say) and have it read it.
- **The gadget's own files:** a path from `"/"` is a file on the gadget's
  flash, for `file_read` and `ui_image_load` alike, with the `files.gadget`
  permission: `ui_image_load(img, "/photos/cat.jpg")`. They load without the
  network and quicker than from your VM, so put pictures an app shows every
  time there: `files.put` with `{"path": "workspace/gadgets/apps/frame/cat.jpg",
  "name": "photos/cat.jpg"}` copies one from your VM (up to 3 MB; a name is
  folders and a file of A-Z, a-z, 0-9, `.`, `-` and `_`), and answers with
  its SHA-256 (give `sha256` and it checks it first). `files.list` lists
  them with the room left, `files.remove {name}` removes one. They stay until
  removed, and unpairing the gadget removes them all.
- **State:** `state_save(name, data, len)` and `state_load` keep small files
  (64 KB for all of an app's) on your VM in the gadget's folder, for the next
  run.

## Limits

The same on every gadget that runs apps:

- **Memory:** up to 2 MB for everything an app takes: its own memory and
  stack, and what the gadget holds for it (objects, styles, canvases, images,
  chat text, device queues). A gadget short of PSRAM gives less, never under
  1 MB: `device.guide` says how much (`capabilities.wasm.budget_kb`), and so does
  `sys_info(INFO_MEM_BUDGET)`. `sys_info(INFO_MEM_USED)` says how much is in
  use, and `app.status` the peak. Past it, calls answer `ERR_NO_MEMORY`.
- **Time:** each call into the app gets 4 million WebAssembly instructions
  (`on_start` 16 million), well under a second. Past that the app traps and stops.
  Keep a call to a few milliseconds: no busy loops, no waiting.
- **Counts:** 256 objects, 32 shared styles, 8 timers, 4 operations in
  flight, 4 open devices, text of up to 4 KB per string, 200 bytes per log
  line, images and canvases no larger than the screen in all.
- **No WASI, no stdio.** The gadget refuses a module that imports anything
  but `gadget`. `printf`, `snprintf`, `sprintf`, `puts`, `time`, `clock` and
  file functions either pull WASI in or add kilobytes of code: don't use
  them. Format numbers yourself (a small integer-to-text loop, as `hello.c`
  has) and take time from `sys_millis()` or `sys_time()`. Pure libc is fine:
  `memcpy`, `memset`, `strlen`, `strcmp`, `sinf`, `sqrtf`, `malloc`.
- Build with `build.py`. If you compile another way, the target is
  `wasm32-wasi` as a reactor (`-mexec-model=reactor`) with
  `-mcpu=mvp -mbulk-memory -msign-ext -mmutable-globals -mnontrapping-fptoint`:
  the gadget's interpreter takes nothing newer.
- Never send a compiled `.wasm` as base64 or text in a message: build on your
  VM and run it from there.

## Testing on your VM

Once an app runs, the gadget only tells you about a crash when you ask
(`app.status`), so test the logic first. Compile the same C for your VM with
a file of stubs for the `gadget.h` functions the app calls and a `main()`
that calls `on_start` and feeds `on_event` the events you expect: `gadget.h`
drops its WebAssembly attributes when it isn't building for wasm, and
`GADGET_MANIFEST` becomes a harmless declaration.

```sh
~/gadget-sdk/venv/bin/python -m ziglang cc -I ~/gadget-sdk stubs.c app.c -o test && ./test
```

## Tips

- The screen's size and shape are in `device.guide` (and `sys_info`): lay the app
  out from them rather than from fixed numbers. On a round screen keep what
  matters inside the circle: `INFO_SAFE_INSET` is the margin to the largest
  square that shows. A flex column with that much padding is an easy start.
- The real time is `sys_time()`, UTC milliseconds, once the gadget has set
  its clock (0 before). You know the person's time zone; put its offset in
  the app.
- Dark backgrounds suit the gadgets' screens; bright full-screen fills use
  more battery.
- Give the talk button back to Muse with `muse_talk_key(1)` when the app is
  about the chat; use it as a button otherwise.
- A button press on a dark screen only wakes it: the app doesn't see it.

## Design

Every app should look calm, legible and made for the gadget's own screen,
whatever its size and shape. Unless I ask for another look, use this one.

**The screen.** Lay everything out from the screen `device.guide` describes
(`board.width`, `height`, `round`, `safe_inset`; in the app,
`sys_info(INFO_SCREEN_WIDTH)`, `INFO_SCREEN_HEIGHT`, `INFO_ROUND` and
`INFO_SAFE_INSET`), never from fixed numbers. With `w` and `h` the size and
`s` the shorter side: centre things on (`w / 2`, `h / 2`), size them as parts
of `s`, and keep a margin of about `s / 30` at the edges. A round screen has
no corners: keep everything inside the circle, which is narrow near the top
and bottom. `ui_span(y, margin)` says how wide row `y` is (the circle's chord
on a round screen, the width on any other), centred on x = `w / 2`.

Lay a screen out in bands:

- a short title or status near the top, around y = `h / 8`, in `FONT_20` or
  `FONT_16` (`FONT_14` when `s` is under 200);
- the main thing in the middle, around y = `h / 2`;
- at most one hint or button row near the bottom, around y = `h * 7 / 8`.

Dense content goes in the safe area, a container `INFO_SAFE_INSET` in from
each edge: on a round screen the square inside the circle, on a rectangular
one nearly the whole screen. Apart from the Style's faint ring, don't draw a
border or ornament around the edge of the screen.

**Text.** Never show text you haven't fitted. For a label whose text you
don't control (a name, a caption, your words, a number that can grow), call
`ui_fit_text(label, ui_span(y, margin), FONT_20)` after setting it: it takes
a smaller font if it must, then ends it with "...". For more than one line,
give the label a width (`P_WIDTH`) so it wraps, size its box by
`ui_text_height`, and let a taller one scroll. Text in a shape (a button, a
pill) gets padding all round, about `s / 40` and at least 6 px. Centre text
(`ALIGN_CENTER`, `P_TEXT_ALIGN`). Use one large value (`FONT_48`, or
`FONT_28` when `s` is under 240), a label (`FONT_20` or `FONT_16`) and at
most one small hint (`FONT_14`); don't add more text than that.

**Colour.** The Style below, unless I ask for another: its palette, one or
two accents at a time. Pick the app's main accent from it and give the app
the launcher colour nearest to it (sage: green or mint, coral: red, slate:
blue, lavender-grey: indigo or gray, gold: yellow, violet: purple), so it
looks like its icon.

**Shapes.**

- Prefer circles, arcs and pills (`P_RADIUS` `RADIUS_CIRCLE`) to boxes with
  corners. Containers and buttons come with LVGL's theme: set their
  `P_BG_COLOR` (the panel colour, or the accent for the one main action) and
  take off borders and shadows you didn't ask for.
- Show progress as a ring: a `W_ARC` with its main part the faint ring
  colour and its indicator (`PART_INDICATOR`) the accent, about `s / 30`
  wide (`P_ARC_WIDTH`), inside the margin.
- On a touch screen, make anything a finger taps at least 48 px across (or a
  quarter of `s` on a small screen), with space between.

**Motion.** Ease changes over 150 to 300 ms with `ui_animate` instead of
jumping, and change only what changed: a still screen costs nothing.

**Games.** The playfield is the screen's own shape: draw it on a `W_CANVAS`
the size of the screen (or move small objects), and make its edge the wall.
On a rectangular screen a ball of radius `r` bounces off x = `r` and
`w - r`, y = `r` and `h - r`. On a round one the wall is the circle,
`R = s / 2 - margin` from the centre: keep the ball within `R - r` of it, and
when it gets there reflect its velocity about the normal and put it back
inside:

```c
float cx = w / 2.0f, cy = h / 2.0f, lim = R - r;
float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy);
if (d > lim) {
    float nx = dx / d, ny = dy / d, dot = vx * nx + vy * ny;
    if (dot > 0) { vx -= 2 * dot * nx; vy -= 2 * dot * ny; }
    x = cx + nx * lim; y = cy + ny * lim;
}
```

In Breakout:

- Lay the bricks in rows, each centred and as wide as `ui_span` allows at
  its row, so on a round screen the rows follow the circle's curve.
- The paddle slides along a row near the bottom, its travel that row's
  span; on a round screen it can ride the rim as an arc that the user turns.
- A ball that passes the paddle's row is lost.
- Score and lives go small at the top, fitted to the span.

Apply the same rule to every game: walls, targets and travel all follow the
screen's shape.

**Icon.** Give each app an `icon` and a `color` in its manifest. Pick the
glyph that says what the app does, and a colour no other app on the gadget
has, if you can (check `app.list`; Colour, above). The glyphs: timer, clock, calendar,
check, list, note, chat, mic, music, heart, pulse, moon, sun, cloud, drop,
leaf, bolt, star, spark, smile, game, dice, target, bricks, book, chart,
compass, globe, cup, bell, home, pin, camera, flag, wind. The colours: red,
orange, yellow, green, mint, teal, blue, indigo, purple, pink, brown, gray.

### Style
If I don't ask for another style, use this one: muted, desaturated traces on charcoal. Never neon, never fully saturated.

Palette, with what each colour is for:

Background 0x121214, charcoal, for the whole screen
Panel 0x17171C, slightly lighter charcoal, for the main disc or card behind content
Faint dial ring 0x23232B, for the ring itself
Sage green 0x5FA87D, for main text, counts, and positive state
Coral red 0xC05A50, for alerts, lives, game over, and one ring segment
Slate blue 0x5E7FA6, for secondary accents, water, and rules
Lavender-grey 0x8E8CA8, for labels, hints, and secondary text
Pale 0xD8D5CC, for headings, paddles, and light glyphs
Muted violet 0x7E639F, spare accent, used for a lit tick ring instead of green
Muted gold 0xC7A252, spare accent, used for sun icons and small highlights

Ring treatment, the signature bit:

A faint 0x23232B ring, as large as the shorter side allows inside the margin (on a round screen, near its edge), with just two small coloured segments on it: coral top-right, about 25 to 65 degrees, and sage bottom-left, about 205 to 245 degrees. Everything else on the ring stays faint.

Layout and text:

Centre everything on the screen's centre. On a round screen keep it inside the circle: the corners are not on the screen.
Frame text with thin rules, for example a coral rule above and a slate rule below, rather than boxes or bright fills.
Labels in lavender-grey, main values in sage or pale. Accent colours are used sparingly, one or two at a time, not all at once.
Dark backgrounds also save battery on OLED screens; avoid bright full-screen fills, they cost battery and break the look.
Really try hard not to overlap the ring.

Do not:

No hot pink, neon green, or fully saturated fills. An earlier version went hot pink, then neon green, and both were rejected for exactly that reason.
No rainbow brick rows or multicolour noise. In Breakout the rows stay to the muted set: slate, sage, coral, lavender-grey, gold.

As C defines, for an app:
#define STYLE_BG   0x121214
#define STYLE_PANEL 0x17171C
#define STYLE_RING 0x23232B
#define STYLE_SAGE 0x5FA87D
#define STYLE_CORAL 0xC05A50
#define STYLE_SLATE 0x5E7FA6
#define STYLE_LAV  0x8E8CA8
#define STYLE_PALE 0xD8D5CC
#define STYLE_VIOLET 0x7E639F
#define STYLE_GOLD 0xC7A252

## gadget.h

<!-- BEGIN gadget.h -->
```c
/*
 * gadget.h: the Muse gadget apps API, version 1.1 (draft).
 *
 * Generated from components/muse_apps/sdk/api/api.json by components/muse_apps/sdk/api/gen_api.py. Don't edit it:
 * change api.json and run gen_api.py.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GADGET_H
#define GADGET_H

#include <stdint.h>

#define GADGET_API_MAJOR 1
#define GADGET_API_MINOR 1

#ifdef __wasm__
#define GADGET_IMPORT(name) __attribute__((import_module("gadget"), import_name(#name)))
#define GADGET_EXPORT(name) __attribute__((export_name(#name)))
/* The app's manifest, as one JSON string literal, written to the custom section
 * gadget.manifest: GADGET_MANIFEST("{\"api\":\"1.0\", ...}"); */
#define GADGET_MANIFEST(json) \
    __asm__(".section .custom_section.gadget.manifest,\"\",@\n.ascii " #json "\n.text\n")
#else
#define GADGET_IMPORT(name)
#define GADGET_EXPORT(name)
#define GADGET_MANIFEST(json) extern int gadget_manifest_unused_
#endif

/*
 * Conventions. Every function is imported from the module "gadget". An app
 * that imports anything else isn't run. Integers are 32-bit and signed.
 * Colours are 0xRRGGBB. Opacity runs from 0 (transparent) to 255 (opaque).
 * Angles are whole degrees, clockwise, with 0 at 3 o'clock, as in LVGL.
 * Percentages run from 0 to 100. Strings are NUL-terminated UTF-8 in the app's
 * memory. A call copies what it needs before it returns. A handle or id is
 * positive; 0 is never a valid one. A function that can fail returns a
 * negative ERR_ constant. Nothing blocks. A call that needs the VM, or takes
 * time, returns an operation id at once and finishes later with EV_DONE (a:
 * the id, b: 0 or more on success, or an ERR_). A buffer given to such a call
 * must stay reserved until then. Changes to the screen made in one callback
 * appear together on the next frame after it returns. Getters see every change
 * made so far. The gadget describes itself: board facts come from sys_info,
 * and its sensors and other hardware from dev_ calls, each with a description
 * the firmware supplies. device.guide gives Muse the same before it writes an
 * app.
 */

/* ---- Values ---- */

/* The app's screen: the parent of top-level objects. It can't be deleted. */
#define UI_ROOT 1
/* A width or height that fits the object's content. */
#define SIZE_CONTENT 0x2FFFFFFF
/* p percent (0 to 1000) of the parent's content area. */
#define PCT(p) (0x20000000 + (p))
/* A radius that makes the object round, or its ends fully rounded. */
#define RADIUS_CIRCLE 0x7FFF
#define OPA_TRANSP 0  /* Fully transparent. */
#define OPA_COVER 255  /* Fully opaque. */
/* What ui_get returns for a wrong handle or property; the log says which. */
#define UI_INVALID (-0x7FFFFFFF - 1)

/* ---- Events: on_event(type, a, b) ---- */

#define EV_UI 1  /* a: handle, b: UI_ bit. Something happened to an object the app asked about with ui_on, or an image load finished. */
#define EV_BUTTON 2  /* a: BUTTON_, b: BTN_. A button. */
#define EV_KNOB 3  /* a: steps. The knob turned: positive clockwise. */
#define EV_TOUCH_DOWN 4  /* a: x, b: y. A touch began on the root, or on an object that isn't clickable. */
#define EV_TOUCH_MOVE 5  /* a: x, b: y. The touch moved. */
#define EV_TOUCH_UP 6  /* a: x, b: y. The touch ended. */
#define EV_GESTURE 7  /* a: GESTURE_. A swipe. */
#define EV_TIMER 8  /* a: timer id. A timer fired. */
#define EV_DONE 9  /* a: op id, b: result. An operation finished: b is 0 or more on success (bytes read, say), or an ERR_. */
#define EV_CHAT 10  /* a: CHAT_, b: message id or value. The chat changed, after chat_open. */
#define EV_MUSE 11  /* a: MUSE_. Muse's mode changed. */
#define EV_DEV_DATA 12  /* a: device, b: readings waiting. Readings to take with dev_take, after dev_open. At most one per frame per device. */
#define EV_DEV_EVENT 13  /* a: device, b: event index. A device's event (dev_event gives the index), after dev_open. */
#define EV_SCREEN 14  /* a: 1 on, 0 off. The screen went off (on_frame stops) or came back. */
#define EV_CONNECTION 15  /* a: 1 up, 0 down. The link to the VM came up or went down. */

/* ---- Constants ---- */

/*
 * Why a call failed. Every one is negative.
 */
#define ERR_ARG (-1)  /* An argument is out of range, a string is too long, or a buffer lies outside the app's memory. */
#define ERR_HANDLE (-2)  /* The handle or id isn't one this app holds, or is the wrong kind for the call. */
#define ERR_NO_MEMORY (-3)  /* The app's memory budget is used up. */
#define ERR_LIMIT (-4)  /* A per-app limit was reached (objects, styles, timers, operations, open sensors). */
#define ERR_BUSY (-5)  /* The thing is in use; try again later. */
#define ERR_OFFLINE (-6)  /* The gadget isn't connected to its VM. */
#define ERR_DENIED (-7)  /* The app's manifest doesn't ask for this permission, or the VM refused. */
#define ERR_UNSUPPORTED (-8)  /* This gadget doesn't have it: a sensor, an output, or a property on this widget. */
#define ERR_TOO_LARGE (-9)  /* The data is larger than the call takes. */
#define ERR_NOT_FOUND (-10)  /* The file, state or message doesn't exist. */
#define ERR_TIMEOUT (-11)  /* The VM didn't answer in time. */
#define ERR_FAILED (-12)  /* Anything else; the app's log says why. */

/*
 * What sys_info reports.
 */
#define INFO_API_VERSION 1  /* The API version the gadget serves, major * 100 + minor. */
#define INFO_SCREEN_WIDTH 2  /* The screen's width in pixels. */
#define INFO_SCREEN_HEIGHT 3  /* The screen's height in pixels. */
#define INFO_ROUND 4  /* 1 on a round screen. */
#define INFO_SAFE_INSET 5  /* Pixels to keep clear of the edges so nothing is cut off: on a round screen, the inset of the largest centred square. */
#define INFO_TOUCH 6  /* 1 if the screen takes touch. */
#define INFO_BUTTONS 7  /* The buttons this gadget reports, as bits: 1 << BUTTON_x. */
#define INFO_KNOB 8  /* 1 if the gadget has a knob or wheel (EV_KNOB). */
#define INFO_MEM_BUDGET 10  /* The app's memory budget in bytes: limits.budget_bytes, or less on a gadget short of PSRAM. */
#define INFO_MEM_USED 11  /* Bytes of the budget in use. */
#define INFO_CONNECTED 12  /* 1 while the gadget is connected to its VM. */

/*
 * Why an app is being closed (on_stop).
 */
#define STOP_USER 1  /* The person closed it (the power key, or the exit button held 2 s) or Muse ran app.stop. */
#define STOP_REPLACED 2  /* Muse started another app. */
#define STOP_CALL 3  /* A voice call started. */
#define STOP_VIDEO 4  /* The video view opened. */
#define STOP_EXIT 5  /* The app called sys_exit. */
#define STOP_UPDATE 6  /* The gadget is restarting, for an update or a setting. */

/*
 * Buttons, by role. Which exist depends on the board (INFO_BUTTONS). The power
 * key always closes the app and is never reported. Holding BUTTON_PRIMARY (the
 * talk button) for 2 s closes it too: device.guide names it as exit_hold; the
 * app sees its BTN_DOWN and BTN_LONG, then on_stop, and never its BTN_UP. If
 * the app gives the talk button back to Muse with muse_talk_key(1), that hold
 * is reserved for talking instead; provide an on-screen Close control or use
 * app.stop.
 */
#define BUTTON_PRIMARY 1  /* The talk button. Muse's push-to-talk while muse_talk_key(1). */
#define BUTTON_SECONDARY 2  /* The second button, where the board has one. */
#define BUTTON_UP 3  /* A navigation key. */
#define BUTTON_DOWN 4  /* A navigation key. */
#define BUTTON_LEFT 5  /* A navigation key. */
#define BUTTON_RIGHT 6  /* A navigation key. */
#define BUTTON_ENTER 7  /* Select, or a knob's press. */
#define BUTTON_BACK 8  /* Escape or back. */

/*
 * What a button did (EV_BUTTON's b).
 */
#define BTN_DOWN 1  /* Pressed. */
#define BTN_UP 2  /* Released. */
#define BTN_LONG 3  /* Still held 600 ms after DOWN; UP still follows. */

/*
 * Swipe directions (EV_GESTURE).
 */
#define GESTURE_LEFT 1
#define GESTURE_RIGHT 2
#define GESTURE_UP 3
#define GESTURE_DOWN 4

/*
 * What Muse is doing (muse_mode, EV_MUSE).
 */
#define MUSE_IDLE 0
#define MUSE_LISTENING 1  /* Recording the person. */
#define MUSE_THINKING 2  /* Waiting for the reply. */
#define MUSE_SPEAKING 3  /* Playing the reply. */
#define MUSE_ERROR 4  /* Something failed; the caption says what. */

/*
 * What changed in the chat (EV_CHAT's a).
 */
#define CHAT_MESSAGE 1  /* A new message; b is its id. */
#define CHAT_TEXT 2  /* A message's text grew while Muse writes it; b is its id. */
#define CHAT_DONE 3  /* A message is complete; b is its id. */
#define CHAT_BUSY 4  /* b is 1 while Muse is working on a reply, 0 once it's idle. */
#define CHAT_CONNECTED 5  /* b is 1 once the chat is followed, 0 when the link is lost (it reconnects by itself). */

/*
 * What chat_info reports about a message.
 */
#define CHAT_INFO_ROLE 1  /* A ROLE_ constant. */
#define CHAT_INFO_LENGTH 2  /* Bytes of text kept. */
#define CHAT_INFO_DONE 3  /* 1 once the message is complete. */
#define CHAT_INFO_TRUNCATED 4  /* 1 if the text was cut to chat_text_bytes. */

/*
 * Who wrote a message.
 */
#define ROLE_USER 1  /* The person. */
#define ROLE_MUSE 2  /* Muse. */
#define ROLE_APP 3  /* An app on a gadget, with chat_send. Its "[name on the gadget]" prefix is removed. */

/*
 * Object events, as bits for ui_on; EV_UI's b holds one.
 */
#define UI_CLICKED 1  /* Pressed and released without sliding off. */
#define UI_PRESSED 2
#define UI_RELEASED 4
#define UI_LONG_PRESSED 8  /* Held 400 ms. */
#define UI_VALUE_CHANGED 16  /* The person changed its value, selection, checked state or text. */
#define UI_FOCUSED 32
#define UI_DEFOCUSED 64
#define UI_READY 128  /* A keyboard's OK, or Enter in a one-line text area. */
#define UI_CANCEL 256  /* A keyboard's close key, or BACK while navigating. */
#define UI_SCROLL_END 512  /* Scrolling stopped. */
#define UI_ANIM_DONE 1024  /* An animation on the object finished. */
#define UI_LOADED 2048  /* ui_image_load finished. Sent without ui_on. */
#define UI_LOAD_FAILED 4096  /* ui_image_load failed; the log says why. Sent without ui_on. */

/*
 * Parts of a widget, for ui_style and ui_style_add. Combine with a STATE_.
 */
#define PART_MAIN 0  /* The background. */
#define PART_SCROLLBAR 256
#define PART_INDICATOR 512  /* A bar's, slider's or arc's filled part, a checkbox's box, a scale's major ticks. */
#define PART_KNOB 768  /* A slider's, arc's or switch's handle. */
#define PART_SELECTED 1024  /* The selected option or text. */
#define PART_ITEMS 1280  /* A table's cells, a chart's series, a scale's minor ticks, a keyboard's keys. */
#define PART_CURSOR 1536  /* A text area's cursor, a chart's cursor. */

/*
 * States, for ui_style and ui_style_add.
 */
#define STATE_DEFAULT 0
#define STATE_CHECKED 1
#define STATE_FOCUSED 2
#define STATE_PRESSED 4
#define STATE_DISABLED 8
#define STATE_EDITED 16  /* A slider or roller being changed with keys. */

/*
 * Where to put an object, as in LVGL.
 */
#define ALIGN_DEFAULT 0
#define ALIGN_TOP_LEFT 1
#define ALIGN_TOP_MID 2
#define ALIGN_TOP_RIGHT 3
#define ALIGN_BOTTOM_LEFT 4
#define ALIGN_BOTTOM_MID 5
#define ALIGN_BOTTOM_RIGHT 6
#define ALIGN_LEFT_MID 7
#define ALIGN_RIGHT_MID 8
#define ALIGN_CENTER 9
#define ALIGN_OUT_TOP_LEFT 10  /* Outside the base, for ui_align_to. */
#define ALIGN_OUT_TOP_MID 11
#define ALIGN_OUT_TOP_RIGHT 12
#define ALIGN_OUT_BOTTOM_LEFT 13
#define ALIGN_OUT_BOTTOM_MID 14
#define ALIGN_OUT_BOTTOM_RIGHT 15
#define ALIGN_OUT_LEFT_TOP 16
#define ALIGN_OUT_LEFT_MID 17
#define ALIGN_OUT_LEFT_BOTTOM 18
#define ALIGN_OUT_RIGHT_TOP 19
#define ALIGN_OUT_RIGHT_MID 20
#define ALIGN_OUT_RIGHT_BOTTOM 21

/*
 * Text align.
 */
#define TEXT_ALIGN_AUTO 0
#define TEXT_ALIGN_LEFT 1
#define TEXT_ALIGN_CENTER 2
#define TEXT_ALIGN_RIGHT 3

/*
 * The fonts every gadget has: Montserrat, with LVGL's symbols (SYM_).
 */
#define FONT_DEFAULT 0  /* The theme's font, 14 px. */
#define FONT_14 1
#define FONT_16 2
#define FONT_20 3
#define FONT_28 4
#define FONT_48 5  /* Digits, ':', '.', '-', '%' and the degree sign only. */

/*
 * Layout.
 */
#define LAYOUT_NONE 0  /* Children are placed by x, y and align. */
#define LAYOUT_FLEX 1  /* Children flow in rows or columns (P_FLEX_*). */
#define LAYOUT_GRID 2  /* Set with ui_grid. */

/*
 * Flex flow.
 */
#define FLEX_ROW 0
#define FLEX_COLUMN 1
#define FLEX_ROW_WRAP 2
#define FLEX_COLUMN_WRAP 3
#define FLEX_ROW_REVERSE 4
#define FLEX_COLUMN_REVERSE 5

/*
 * How flex places items and tracks.
 */
#define PLACE_START 0
#define PLACE_END 1
#define PLACE_CENTER 2
#define PLACE_SPACE_EVENLY 3
#define PLACE_SPACE_AROUND 4
#define PLACE_SPACE_BETWEEN 5

/*
 * Dir.
 */
#define DIR_NONE 0
#define DIR_LEFT 1
#define DIR_RIGHT 2
#define DIR_TOP 4
#define DIR_BOTTOM 8
#define DIR_HOR 3
#define DIR_VER 12
#define DIR_ALL 15

/*
 * Scrollbar mode.
 */
#define SCROLLBAR_OFF 0
#define SCROLLBAR_ON 1
#define SCROLLBAR_ACTIVE 2  /* While scrolling. */
#define SCROLLBAR_AUTO 3  /* When the content is larger. */

/*
 * Snap.
 */
#define SNAP_NONE 0
#define SNAP_START 1
#define SNAP_END 2
#define SNAP_CENTER 3

/*
 * What a label does with text that doesn't fit its size.
 */
#define LONG_WRAP 0  /* Wraps, growing the height. */
#define LONG_DOTS 1  /* Ends with ... */
#define LONG_SCROLL 2  /* Scrolls back and forth. */
#define LONG_SCROLL_CIRCULAR 3  /* Scrolls round. */
#define LONG_CLIP 4  /* Cuts it off. */

/*
 * Grad dir.
 */
#define GRAD_NONE 0
#define GRAD_VER 1
#define GRAD_HOR 2

/*
 * Text decor.
 */
#define DECOR_NONE 0
#define DECOR_UNDERLINE 1
#define DECOR_STRIKETHROUGH 2

/*
 * Border side.
 */
#define BORDER_NONE 0
#define BORDER_BOTTOM 1
#define BORDER_TOP 2
#define BORDER_LEFT 4
#define BORDER_RIGHT 8
#define BORDER_FULL 15

/*
 * Arc mode.
 */
#define ARC_NORMAL 0
#define ARC_SYMMETRICAL 1  /* Fills from the middle. */
#define ARC_REVERSE 2

/*
 * Bar mode.
 */
#define BAR_NORMAL 0
#define BAR_SYMMETRICAL 1  /* Fills from zero. */
#define BAR_RANGE 2  /* From P_START_VALUE to P_VALUE. */

/*
 * Orientation.
 */
#define ORIENT_AUTO 0  /* From the object's shape. */
#define ORIENT_HORIZONTAL 1
#define ORIENT_VERTICAL 2

/*
 * Keyboard mode.
 */
#define KB_TEXT_LOWER 0
#define KB_TEXT_UPPER 1
#define KB_SPECIAL 2
#define KB_NUMBER 3

/*
 * Scale mode.
 */
#define SCALE_HORIZONTAL_TOP 0
#define SCALE_HORIZONTAL_BOTTOM 1
#define SCALE_VERTICAL_LEFT 2
#define SCALE_VERTICAL_RIGHT 3
#define SCALE_ROUND_INNER 4  /* A dial, labels inside. */
#define SCALE_ROUND_OUTER 5  /* A dial, labels outside. */

/*
 * Chart type.
 */
#define CHART_NONE 0
#define CHART_LINE 1
#define CHART_BAR 2

/*
 * Chart update.
 */
#define UPDATE_SHIFT 0  /* ui_chart_push shifts the points left. */
#define UPDATE_CIRCULAR 1  /* ui_chart_push overwrites the oldest point. */

/*
 * How an image fills an image object larger or smaller than itself.
 */
#define IMAGE_CENTER 0
#define IMAGE_TOP_LEFT 1
#define IMAGE_STRETCH 2
#define IMAGE_TILE 3
#define IMAGE_CONTAIN 4
#define IMAGE_COVER 5

/*
 * Animation curves; add ANIM_ flags.
 */
#define EASE_LINEAR 0
#define EASE_IN 1
#define EASE_OUT 2
#define EASE_IN_OUT 3
#define EASE_OVERSHOOT 4
#define EASE_BOUNCE 5
#define EASE_STEP 6  /* Jumps at the end. */

/*
 * Anim flag.
 */
#define ANIM_REPEAT 256  /* Runs forever. */
#define ANIM_REVERSE 512  /* Runs back to the start after each run. */

/* Widget types, for ui_create. */
#define W_OBJ 1  /* A plain container: a box with a background, border and children. */
#define W_LABEL 2  /* Text. */
#define W_BUTTON 3  /* A container that presses; put a label in it. */
#define W_IMAGE 4  /* A PNG (8-bit channels) or JPEG (baseline) (ui_image_load, ui_image_set). */
#define W_LINE 5  /* A polyline through points (ui_line_points). */
#define W_ARC 6  /* A ring with a value, which touch can drag. */
#define W_BAR 7  /* A progress bar. */
#define W_SLIDER 8  /* A bar with a knob to drag. */
#define W_SWITCH 9  /* An on/off toggle; P_CHECKED is its state. */
#define W_CHECKBOX 10  /* A box with a label; P_CHECKED is its state. */
#define W_ROLLER 11  /* A drum of options to spin. */
#define W_DROPDOWN 12  /* A button that opens a list of options. */
#define W_TEXTAREA 13  /* Editable text, for a keyboard. */
#define W_KEYBOARD 14  /* An on-screen keyboard that types into a text area. */
#define W_SCALE 15  /* Ticks and labels in a line or a dial; ui_scale_needle points a line at a value. */
#define W_SPINNER 16  /* A spinning arc for 'working'. */
#define W_CHART 17  /* Line or bar series (ui_chart_series, ui_chart_set, ui_chart_push). */
#define W_TABLE 18  /* Rows and columns of text (ui_table_cell). */
#define W_CANVAS 19  /* Pixels to draw on with the cv_ calls. Its buffer is RGB565 and counts against the budget; P_WIDTH and P_HEIGHT size it, up to the screen. */

/* Properties, for ui_set, ui_get, ui_set_text, ui_style and ui_animate. */
#define P_X 1  /* coord, any object. Left edge, from the parent's content area, or the offset from P_ALIGN. */
#define P_Y 2  /* coord, any object. Top edge, or the offset from P_ALIGN. */
#define P_WIDTH 3  /* coord, any object */
#define P_HEIGHT 4  /* coord, any object */
#define P_MIN_WIDTH 5  /* coord, any object */
#define P_MAX_WIDTH 6  /* coord, any object */
#define P_MIN_HEIGHT 7  /* coord, any object */
#define P_MAX_HEIGHT 8  /* coord, any object */
#define P_ALIGN 9  /* align, any object. Where in the parent; P_X and P_Y offset it. */
#define P_HIDDEN 10  /* bool, any object. Not drawn, and takes no input or space in a layout. */
#define P_CLICKABLE 11  /* bool, any object. Takes touch. On for buttons and inputs, off for labels and images. */
#define P_SCROLLABLE 12  /* bool, any object. Scrolls when its content is larger. On for containers. */
#define P_CHECKABLE 13  /* bool, any object. A click toggles P_CHECKED. */
#define P_EVENT_BUBBLE 14  /* bool, any object. Its events also go to its parent (one ui_on on a list instead of one per row). */
#define P_FLOATING 15  /* bool, any object. Left out of the parent's layout and scrolling. */
#define P_CHECKED 16  /* bool, any object. The checked state: a switch's or checkbox's value. */
#define P_DISABLED 17  /* bool, any object. Greyed out; takes no input. */
#define P_SCROLL_DIR 18  /* dir, any object */
#define P_SCROLLBAR_MODE 19  /* scrollbar_mode, any object */
#define P_SCROLL_SNAP_X 20  /* snap, any object. Which part of a child to stop on after a horizontal scroll. */
#define P_SCROLL_SNAP_Y 21  /* snap, any object */
#define P_SCROLL_X 22  /* int, any object, read-only. How far it's scrolled; scroll with ui_scroll_to. */
#define P_SCROLL_Y 23  /* int, any object, read-only */
#define P_INDEX 24  /* int, any object. Its place among its siblings, which sets the drawing order: 0 is drawn first, -1 moves it last. */
#define P_CHILD_COUNT 25  /* int, any object, read-only */
#define P_LAYOUT 26  /* layout, any object */
#define P_FLEX_FLOW 27  /* flex_flow, any object */
#define P_FLEX_MAIN_PLACE 28  /* place, any object. Along the flow. */
#define P_FLEX_CROSS_PLACE 29  /* place, any object. Across the flow, within a track (see flex_track_place). */
#define P_FLEX_TRACK_PLACE 30  /* place, any object. Where the tracks sit across the flow. A row or column that doesn't wrap is one track, as wide (or tall) as its largest item, so centring its items across the flow takes PLACE_CENTER here and in flex_cross_place. */
#define P_FLEX_GROW 31  /* int, any object. A child's share of the free space in its parent's flex track; 0 keeps its own size. */
#define P_BG_COLOR 100  /* rgb, style */
#define P_BG_OPA 101  /* opa, style */
#define P_BG_GRAD_COLOR 102  /* rgb, style. The gradient's end colour. */
#define P_BG_GRAD_DIR 103  /* grad_dir, style */
#define P_BORDER_COLOR 104  /* rgb, style */
#define P_BORDER_WIDTH 105  /* int, style */
#define P_BORDER_OPA 106  /* opa, style */
#define P_BORDER_SIDE 107  /* border_side, style */
#define P_OUTLINE_COLOR 108  /* rgb, style */
#define P_OUTLINE_WIDTH 109  /* int, style */
#define P_OUTLINE_PAD 110  /* int, style. Gap between the object and its outline. */
#define P_SHADOW_COLOR 111  /* rgb, style */
#define P_SHADOW_WIDTH 112  /* int, style. Blur width, 0 to 64. Shadows cost drawing time; use them on a few objects. */
#define P_SHADOW_OPA 113  /* opa, style */
#define P_SHADOW_OFFSET_X 114  /* int, style. -64 to 64. */
#define P_SHADOW_OFFSET_Y 115  /* int, style. -64 to 64. */
#define P_SHADOW_SPREAD 116  /* int, style. 0 to 64. */
#define P_RADIUS 117  /* int, style. Corner radius, 0 to 64; RADIUS_CIRCLE for round. */
#define P_PAD_ALL 118  /* int, style, write-only. Sets the four paddings. */
#define P_PAD_LEFT 119  /* int, style */
#define P_PAD_RIGHT 120  /* int, style */
#define P_PAD_TOP 121  /* int, style */
#define P_PAD_BOTTOM 122  /* int, style */
#define P_PAD_ROW 123  /* int, style. Gap between rows of a layout. */
#define P_PAD_COLUMN 124  /* int, style. Gap between columns of a layout. */
#define P_TEXT_COLOR 125  /* rgb, style */
#define P_TEXT_OPA 126  /* opa, style */
#define P_TEXT_FONT 127  /* font, style */
#define P_TEXT_ALIGN 128  /* text_align, style */
#define P_TEXT_LETTER_SPACE 129  /* int, style */
#define P_TEXT_LINE_SPACE 130  /* int, style. Not negative. */
#define P_TEXT_DECOR 131  /* text_decor, style */
#define P_LINE_COLOR 132  /* rgb, style */
#define P_LINE_WIDTH 133  /* int, style */
#define P_LINE_OPA 134  /* opa, style */
#define P_LINE_ROUNDED 135  /* bool, style */
#define P_LINE_DASH_WIDTH 136  /* int, style. 0 to 64. */
#define P_LINE_DASH_GAP 137  /* int, style. 0 to 64. */
#define P_ARC_COLOR 138  /* rgb, style */
#define P_ARC_WIDTH 139  /* int, style */
#define P_ARC_OPA 140  /* opa, style */
#define P_ARC_ROUNDED 141  /* bool, style */
#define P_IMAGE_OPA 142  /* opa, style */
#define P_IMAGE_RECOLOR 143  /* rgb, style. Tints an image (or a symbol drawn as one). */
#define P_IMAGE_RECOLOR_OPA 144  /* opa, style */
#define P_OPA 145  /* opa, style. The whole object and its children. */
#define P_TRANSFORM_ROTATION 146  /* angle, style. Rotates the drawn object about its pivot. Costs drawing time and memory. */
#define P_TRANSFORM_SCALE 147  /* percent, style. Scales the drawn object about its pivot. */
#define P_TRANSFORM_PIVOT_X 148  /* coord, style */
#define P_TRANSFORM_PIVOT_Y 149  /* coord, style */
#define P_TRANSLATE_X 150  /* coord, style. Moves the drawn object without moving it in the layout. */
#define P_TRANSLATE_Y 151  /* coord, style */
#define P_LENGTH 152  /* int, style. A scale's tick length (PART_INDICATOR major, PART_ITEMS minor). */
#define P_CLIP_CORNER 153  /* bool, style. Clips children to the rounded corners. */
#define P_ANIM_DURATION 154  /* int, style. Milliseconds for a widget's own animations: a label's scrolling, a roller's spin. */
#define P_TEXT 200  /* text, label, checkbox, textarea, dropdown. The text. On a dropdown, fixed text to show in place of the selected option. */
#define P_LONG_MODE 201  /* long_mode, label */
#define P_VALUE 202  /* int, arc, bar, slider */
#define P_MIN_VALUE 203  /* int, arc, bar, slider, scale, chart. The range's low end; a chart's Y axis. */
#define P_MAX_VALUE 204  /* int, arc, bar, slider, scale, chart */
#define P_START_VALUE 205  /* int, bar, slider. The low end in BAR_RANGE mode. */
#define P_BAR_MODE 206  /* bar_mode, bar, slider */
#define P_ORIENTATION 207  /* orientation, bar, slider, switch */
#define P_ARC_MODE 208  /* arc_mode, arc */
#define P_START_ANGLE 209  /* angle, arc. The indicator's start; setting P_VALUE moves it too. */
#define P_END_ANGLE 210  /* angle, arc */
#define P_BG_START_ANGLE 211  /* angle, arc. The ring's start. */
#define P_BG_END_ANGLE 212  /* angle, arc */
#define P_ROTATION 213  /* angle, arc, scale, image. Turns the whole widget. */
#define P_OPTIONS 214  /* text, roller, dropdown. The options, one per line. */
#define P_SELECTED 215  /* int, roller, dropdown. The selected option, from 0. */
#define P_VISIBLE_ROWS 216  /* int, roller */
#define P_INFINITE 217  /* bool, roller. Spins round instead of stopping at the ends. ERR_ARG if letter spacing leaves its options no height. */
#define P_LIST_DIR 218  /* dir, dropdown. Which way the list opens. */
#define P_PLACEHOLDER 219  /* text, textarea. Text shown while it's empty. */
#define P_ONE_LINE 220  /* bool, textarea */
#define P_PASSWORD 221  /* bool, textarea. Shows bullets. */
#define P_MAX_LENGTH 222  /* int, textarea */
#define P_ACCEPTED_CHARS 223  /* text, textarea. The only characters it takes. */
#define P_CURSOR_POS 224  /* int, textarea */
#define P_TEXTAREA 225  /* handle, keyboard. The text area it types into. */
#define P_KEYBOARD_MODE 226  /* keyboard_mode, keyboard */
#define P_SCALE_MODE 227  /* scale_mode, scale */
#define P_TOTAL_TICKS 228  /* int, scale */
#define P_MAJOR_TICK_EVERY 229  /* int, scale. Every nth tick is major and labelled. */
#define P_LABEL_SHOW 230  /* bool, scale */
#define P_ANGLE_RANGE 231  /* angle, scale. A dial's sweep. */
#define P_SPIN_TIME 232  /* int, spinner. Milliseconds per turn. */
#define P_ARC_SWEEP 233  /* angle, spinner. The spinning arc's length. */
#define P_CHART_TYPE 234  /* chart_type, chart */
#define P_POINT_COUNT 235  /* int, chart. Points per series. */
#define P_UPDATE_MODE 236  /* chart_update, chart */
#define P_HOR_DIV_LINES 237  /* int, chart. Horizontal grid lines. */
#define P_VER_DIV_LINES 238  /* int, chart. Vertical grid lines. */
#define P_ROW_COUNT 239  /* int, table */
#define P_COLUMN_COUNT 240  /* int, table */
#define P_IMAGE_SCALE 241  /* percent, image. Zoom. */
#define P_OFFSET_X 242  /* int, image. Shifts the picture inside the object, wrapping round. */
#define P_OFFSET_Y 243  /* int, image */
#define P_INNER_ALIGN 244  /* image_align, image */
#define P_Y_INVERT 245  /* bool, line. Y points up from the bottom. */

/*
 * Well-known devices, for dev_find, and where their fields, actions and events
 * are. A gadget may have none of these, and devices with any other name: it
 * lists its own.
 */
#define DEV_TEMPERATURE "temperature"  /* Air temperature. */
#define DEV_TEMPERATURE_C 0  /* field, °C */
#define DEV_HUMIDITY "humidity"  /* Relative humidity. */
#define DEV_HUMIDITY_RH 0  /* field, % */
#define DEV_PRESSURE "pressure"  /* Air pressure. */
#define DEV_PRESSURE_HPA 0  /* field, hPa */
#define DEV_CO2 "co2"  /* CO2. */
#define DEV_CO2_PPM 0  /* field, ppm */
#define DEV_VOC "voc"  /* Volatile organic compounds, as Sensirion's VOC index (1 to 500). */
#define DEV_VOC_INDEX 0  /* field */
#define DEV_LIGHT "light"  /* Ambient light. */
#define DEV_LIGHT_LUX 0  /* field, lx */
#define DEV_ACCEL "accel"  /* Acceleration in the screen's frame: x right, y up, z out of the screen. Events from the IMU's own feature engine where it has one. */
#define DEV_ACCEL_X 0  /* field, g */
#define DEV_ACCEL_Y 1  /* field, g */
#define DEV_ACCEL_Z 2  /* field, g */
#define DEV_ACCEL_TAP 0  /* event */
#define DEV_ACCEL_DOUBLE_TAP 1  /* event */
#define DEV_ACCEL_SHAKE 2  /* event */
#define DEV_ACCEL_FACE_UP 3  /* event */
#define DEV_ACCEL_FACE_DOWN 4  /* event */
#define DEV_GYRO "gyro"  /* Rotation rate, in accel's axes. */
#define DEV_GYRO_X 0  /* field, deg/s */
#define DEV_GYRO_Y 1  /* field, deg/s */
#define DEV_GYRO_Z 2  /* field, deg/s */
#define DEV_STEPS "steps"  /* Steps counted since dev_open, from the IMU's step counter. */
#define DEV_STEPS_COUNT 0  /* field */
#define DEV_BATTERY "battery"  /* charging and usb are 0 or 1; volts is 0 where the board can't measure it. */
#define DEV_BATTERY_PERCENT 0  /* field, % */
#define DEV_BATTERY_VOLTS 1  /* field, V */
#define DEV_BATTERY_CHARGING 2  /* field */
#define DEV_BATTERY_USB 3  /* field */
#define DEV_VIBRATION "vibration"  /* A vibration motor. */
#define DEV_VIBRATION_BUZZ 0  /* action: Buzzes; a new buzz replaces one still running. Params: ms (ms, 1 to 1000). */
#define DEV_SPEAKER "speaker"  /* Beeps on the gadget's speaker; ERR_BUSY while Muse listens or speaks. */
#define DEV_SPEAKER_TONE 0  /* action: Plays a tone. Params: hz (Hz, 100 to 8000), ms (ms, 1 to 2000), volume (%, 0 to 100). */

/* LVGL's symbols, as UTF-8 strings, for labels in any FONT_ but FONT_48. */
#define SYM_AUDIO "\xEF\x80\x81"
#define SYM_VIDEO "\xEF\x80\x88"
#define SYM_LIST "\xEF\x80\x8B"
#define SYM_OK "\xEF\x80\x8C"
#define SYM_CLOSE "\xEF\x80\x8D"
#define SYM_POWER "\xEF\x80\x91"
#define SYM_SETTINGS "\xEF\x80\x93"
#define SYM_HOME "\xEF\x80\x95"
#define SYM_DOWNLOAD "\xEF\x80\x99"
#define SYM_REFRESH "\xEF\x80\xA1"
#define SYM_MUTE "\xEF\x80\xA6"
#define SYM_VOLUME_MID "\xEF\x80\xA7"
#define SYM_VOLUME_MAX "\xEF\x80\xA8"
#define SYM_IMAGE "\xEF\x80\xBE"
#define SYM_PREV "\xEF\x81\x88"
#define SYM_PLAY "\xEF\x81\x8B"
#define SYM_PAUSE "\xEF\x81\x8C"
#define SYM_STOP "\xEF\x81\x8D"
#define SYM_NEXT "\xEF\x81\x91"
#define SYM_LEFT "\xEF\x81\x93"
#define SYM_RIGHT "\xEF\x81\x94"
#define SYM_PLUS "\xEF\x81\xA7"
#define SYM_MINUS "\xEF\x81\xA8"
#define SYM_WARNING "\xEF\x81\xB1"
#define SYM_SHUFFLE "\xEF\x81\xB4"
#define SYM_UP "\xEF\x81\xB7"
#define SYM_DOWN "\xEF\x81\xB8"
#define SYM_LOOP "\xEF\x81\xB9"
#define SYM_CALL "\xEF\x82\x95"
#define SYM_ENVELOPE "\xEF\x83\xA0"
#define SYM_CHARGE "\xEF\x83\xA7"
#define SYM_BELL "\xEF\x83\xB3"
#define SYM_KEYBOARD "\xEF\x84\x9C"
#define SYM_GPS "\xEF\x84\xA4"
#define SYM_WIFI "\xEF\x87\xAB"
#define SYM_BATTERY_FULL "\xEF\x89\x80"
#define SYM_BATTERY_3 "\xEF\x89\x81"
#define SYM_BATTERY_2 "\xEF\x89\x82"
#define SYM_BATTERY_1 "\xEF\x89\x83"
#define SYM_BATTERY_EMPTY "\xEF\x89\x84"
#define SYM_BLUETOOTH "\xEF\x8A\x93"
#define SYM_TRASH "\xEF\x8B\xAD"
#define SYM_EDIT "\xEF\x8C\x84"
#define SYM_BACKSPACE "\xEF\x95\x9A"

/*
 * One reading of a device.
 */
typedef struct {
    int32_t t_ms;  /* sys_millis() when it was taken. */
    int32_t count;  /* How many values it holds. */
    float v[8];  /* The values, in the order and units of the device's fields. */
} gadget_reading_t;

/* ---- Functions ---- */

/* == System == */
/*
 * Time, logging, board facts, timers and the app's own lifecycle.
 */

/*
 * A clock for timing. Wraps after 24 days. Returns milliseconds since the
 * gadget started.
 */
GADGET_IMPORT(sys_millis) int32_t sys_millis(void);

/*
 * The time of day. 0 until the gadget has set its clock from the VM, soon
 * after it connects. Returns UTC milliseconds since 1970, or 0.
 */
GADGET_IMPORT(sys_time) int64_t sys_time(void);

/*
 * Adds a line to the app's log, which Muse reads with app.logs. text: The
 * line.
 */
GADGET_IMPORT(sys_log) void sys_log(const char *text);

/*
 * Board facts and the app's memory use. key: What to report. Returns the
 * value, or ERR_ARG.
 */
GADGET_IMPORT(sys_info) int32_t sys_info(int32_t key);

/*
 * From the hardware random number generator. Returns 32 random bits.
 */
GADGET_IMPORT(sys_random) int32_t sys_random(void);

/*
 * Starts or restarts a timer: EV_TIMER after ms milliseconds, while the app
 * runs, screen on or off. id: 1 to 8. ms: 10 or more. repeat: Fire every ms,
 * not once.
 */
GADGET_IMPORT(sys_timer) int32_t sys_timer(int32_t id, int32_t ms, int32_t repeat);

/*
 * Stops a timer.
 */
GADGET_IMPORT(sys_timer_stop) int32_t sys_timer_stop(int32_t id);

/*
 * How often on_frame is called while the screen is on. 30 to start with, if
 * the app exports on_frame. fps: 0 to 30.
 */
GADGET_IMPORT(sys_frame_rate) int32_t sys_frame_rate(int32_t fps);

/*
 * Keeps the screen on while on is 1. Use it only while showing something the
 * person is watching.
 */
GADGET_IMPORT(sys_keep_awake) void sys_keep_awake(int32_t on);

/*
 * Ends the app once the current callback returns, after on_stop(STOP_EXIT).
 */
GADGET_IMPORT(sys_exit) void sys_exit(void);

/*
 * Sets the screen's brightness while the app runs. percent: 1 to 100, or -1
 * for the person's own setting.
 */
GADGET_IMPORT(sys_brightness) int32_t sys_brightness(int32_t percent);

/* == UI objects == */
/*
 * LVGL widgets, kept by the firmware. Create them once, change their
 * properties, and ask for their events with ui_on. Properties are P_ constants
 * (Properties, below); style properties set this way apply to the main part in
 * the default state.
 */

/*
 * Creates a widget with the theme's look. type: A W_ constant. parent:
 * UI_ROOT, or another object other than a roller. Returns the new object.
 */
GADGET_IMPORT(ui_create) int32_t ui_create(int32_t type, int32_t parent);

/*
 * Deletes an object and its children. Their handles become invalid.
 */
GADGET_IMPORT(ui_delete) int32_t ui_delete(int32_t obj);

/*
 * Deletes an object's children.
 */
GADGET_IMPORT(ui_clean) int32_t ui_clean(int32_t obj);

/*
 * Sets a property that isn't text. prop: A P_ constant.
 */
GADGET_IMPORT(ui_set) int32_t ui_set(int32_t obj, int32_t prop, int32_t value);

/*
 * Reads a property that isn't text. Returns the value, or UI_INVALID.
 */
GADGET_IMPORT(ui_get) int32_t ui_get(int32_t obj, int32_t prop);

/*
 * Sets a text property. prop: A text property: P_TEXT, P_OPTIONS...
 */
GADGET_IMPORT(ui_set_text) int32_t ui_set_text(int32_t obj, int32_t prop, const char *text);

/*
 * Reads a text property, such as what was typed into a text area. buf: Gets
 * the text, cut to fit and always NUL-terminated. Returns the text's full
 * length in bytes.
 */
GADGET_IMPORT(ui_get_text) int32_t ui_get_text(int32_t obj, int32_t prop, void *buf, int32_t buf_size);

/*
 * Chooses which events of this object come to on_event as EV_UI. Replaces the
 * earlier choice. events: UI_ bits, 0 for none.
 */
GADGET_IMPORT(ui_on) int32_t ui_on(int32_t obj, int32_t events);

/*
 * Places an object in its parent, offset by dx, dy.
 */
GADGET_IMPORT(ui_align) int32_t ui_align(int32_t obj, int32_t align, int32_t dx, int32_t dy);

/*
 * Places an object relative to another, once (it doesn't follow base later).
 * base: Another object. align: ALIGN_OUT_ puts it outside base.
 */
GADGET_IMPORT(ui_align_to) int32_t ui_align_to(int32_t obj, int32_t base, int32_t align, int32_t dx, int32_t dy);

/*
 * Moves an object to another parent. parent: Not a roller, nor the object or
 * one inside it.
 */
GADGET_IMPORT(ui_set_parent) int32_t ui_set_parent(int32_t obj, int32_t parent);

/*
 * Lays out the object's children in a grid. Sizes are space-separated: pixels,
 * "auto" (fits the content) or "Nfr" (N shares of what's left), up to 8 each:
 * ui_grid(obj, "1fr 1fr 1fr", "auto 1fr"). Place children with ui_grid_cell.
 * columns: Column sizes. rows: Row sizes.
 */
GADGET_IMPORT(ui_grid) int32_t ui_grid(int32_t obj, const char *columns, const char *rows);

/*
 * Puts a child in a grid cell; the child stretches to fill it. obj: A child of
 * a grid.
 */
GADGET_IMPORT(ui_grid_cell) int32_t ui_grid_cell(int32_t obj, int32_t col, int32_t col_span, int32_t row, int32_t row_span);

/*
 * Scrolls an object's content.
 */
GADGET_IMPORT(ui_scroll_to) int32_t ui_scroll_to(int32_t obj, int32_t x, int32_t y, int32_t animate);

/*
 * Scrolls its parents until the object is visible: the newest message in a
 * chat, say.
 */
GADGET_IMPORT(ui_scroll_to_view) int32_t ui_scroll_to_view(int32_t obj, int32_t animate);

/*
 * On boards without touch: buttons and the knob move focus between the app's
 * buttons, sliders and other inputs (UP and DOWN, or the knob), ENTER presses
 * or edits, BACK sends UI_CANCEL. Buttons it uses don't come as EV_BUTTON
 * while it's on.
 */
GADGET_IMPORT(ui_navigation) int32_t ui_navigation(int32_t on);

/*
 * How wide a line of text is. Returns pixels.
 */
GADGET_IMPORT(ui_text_width) int32_t ui_text_width(int32_t font, const char *text);

/*
 * How tall text is, wrapped at width. width: Wrap width. Returns pixels.
 */
GADGET_IMPORT(ui_text_height) int32_t ui_text_height(int32_t font, int32_t width, const char *text);

/*
 * How much of row y shows, less margin at each end: on a round screen the
 * circle's chord, else the screen's width. Size text and rows near the top and
 * bottom of a round screen by it. y: A row of the screen. margin: Pixels to
 * keep clear at each end. Returns pixels, centred on the screen; 0 off the
 * screen.
 */
GADGET_IMPORT(ui_span) int32_t ui_span(int32_t y, int32_t margin);

/*
 * Fits a label's text on one line width wide: in font, or the next smaller
 * font it fits in, down to FONT_14, then cut short with "...". Sets the
 * label's width, font and long mode. Call it after ui_set_text, and again when
 * the text changes, for any text whose length you don't control: a name, a
 * message, a number that can grow. obj: A label. width: The most it may take.
 * font: The font it would like. Returns the FONT_ it got.
 */
GADGET_IMPORT(ui_fit_text) int32_t ui_fit_text(int32_t obj, int32_t width, int32_t font);

/*
 * Loads a PNG or JPEG into an image object, from the VM or from the gadget's
 * own files (which work offline). EV_UI with UI_LOADED or UI_LOAD_FAILED says
 * when it's done. The decoded image counts against the budget. obj: An image
 * object. path: In the app's folder on the VM, or from "/" on the gadget
 * (files.gadget).
 */
GADGET_IMPORT(ui_image_load) int32_t ui_image_load(int32_t obj, const char *path);

/*
 * Shows a PNG or JPEG from the app's memory, decoded at once. obj: An image
 * object. data: A PNG or JPEG file.
 */
GADGET_IMPORT(ui_image_set) int32_t ui_image_set(int32_t obj, const void *data, int32_t data_len);

/*
 * Sets a line's points. obj: A line object. xy: int32_t x, y pairs.
 */
GADGET_IMPORT(ui_line_points) int32_t ui_line_points(int32_t obj, const void *xy, int32_t xy_len);

/*
 * Adds a series to a chart. Returns the series, 1 to 4.
 */
GADGET_IMPORT(ui_chart_series) int32_t ui_chart_series(int32_t chart, int32_t color);

/*
 * Sets a series' points. values: int32_t values, up to P_POINT_COUNT.
 */
GADGET_IMPORT(ui_chart_set) int32_t ui_chart_set(int32_t chart, int32_t series, const void *values, int32_t values_len);

/*
 * Adds a point to a series, as P_UPDATE_MODE says.
 */
GADGET_IMPORT(ui_chart_push) int32_t ui_chart_push(int32_t chart, int32_t series, int32_t value);

/*
 * Sets a cell's text, adding rows and columns as needed.
 */
GADGET_IMPORT(ui_table_cell) int32_t ui_table_cell(int32_t table, int32_t row, int32_t col, const char *text);

GADGET_IMPORT(ui_table_column_width) int32_t ui_table_column_width(int32_t table, int32_t col, int32_t width);

/*
 * Points a line at a value on a round scale. line: A line object, child of the
 * scale. length: Pixels from the centre.
 */
GADGET_IMPORT(ui_scale_needle) int32_t ui_scale_needle(int32_t scale, int32_t line, int32_t length, int32_t value);

/* == Styles == */
/*
 * Style properties for other parts and states, and shared styles.
 */

/*
 * Sets a style property for one part and state: ui_style(slider,
 * PART_INDICATOR, P_BG_COLOR, 0x00C8FF). Each selector counts once toward
 * limits.styles_per_object. selector: A PART_ plus STATE_ bits. prop: A style
 * property.
 */
GADGET_IMPORT(ui_style) int32_t ui_style(int32_t obj, int32_t selector, int32_t prop, int32_t value);

/*
 * Creates a shared style, to give many objects the same look with one set of
 * calls. Returns A style.
 */
GADGET_IMPORT(ui_style_create) int32_t ui_style_create(void);

/*
 * Sets a property in a shared style. Objects using it update. prop: A style
 * property.
 */
GADGET_IMPORT(ui_style_set) int32_t ui_style_set(int32_t style, int32_t prop, int32_t value);

/*
 * Gives an object a shared style. With the selectors ui_style has set on it,
 * 32 per object (limits.styles_per_object). Properties set on the object
 * itself still win. selector: A PART_ plus STATE_ bits.
 */
GADGET_IMPORT(ui_style_add) int32_t ui_style_add(int32_t obj, int32_t style, int32_t selector);

/* == Animation == */
/*
 * Animating a property to a value.
 */

/*
 * Animates a property from its value now to to. UI_ANIM_DONE follows, if asked
 * for with ui_on. A new animation of the same property replaces the old one.
 * prop: A property that isn't text. easing: An EASE_ plus ANIM_ flags.
 */
GADGET_IMPORT(ui_animate) int32_t ui_animate(int32_t obj, int32_t prop, int32_t to, int32_t ms, int32_t easing);

/*
 * Stops animations where they are. prop: 0 for all.
 */
GADGET_IMPORT(ui_animate_stop) int32_t ui_animate_stop(int32_t obj, int32_t prop);

/* == Canvas == */
/*
 * Immediate-mode drawing on a canvas object (W_CANVAS). Drawing shows on the
 * next frame.
 */

/*
 * Fills the whole canvas.
 */
GADGET_IMPORT(cv_fill) int32_t cv_fill(int32_t canvas, int32_t color);

/*
 * Opacity for the drawing calls that follow on this canvas. Starts at
 * OPA_COVER.
 */
GADGET_IMPORT(cv_opa) int32_t cv_opa(int32_t canvas, int32_t opa);

/*
 * A filled rectangle; with radius RADIUS_CIRCLE, a circle or pill.
 */
GADGET_IMPORT(cv_rect) int32_t cv_rect(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t color);

/*
 * Draws a rectangle with a shared style: P_RADIUS; P_BG_COLOR, P_BG_OPA,
 * P_BG_GRAD_COLOR and P_BG_GRAD_DIR (GRAD_HOR or GRAD_VER, two colour stops);
 * P_BORDER_COLOR, P_BORDER_WIDTH, P_BORDER_OPA and P_BORDER_SIDE;
 * P_SHADOW_COLOR, P_SHADOW_WIDTH, P_SHADOW_OPA, P_SHADOW_OFFSET_X,
 * P_SHADOW_OFFSET_Y and P_SHADOW_SPREAD. P_OPA and cv_opa multiply the fill,
 * border and shadow opacities. Missing properties give a white fill, square
 * corners and no border or shadow. Other properties are ignored. Border and
 * shadow widths are clamped to 0..64 px, shadow spread to 0..64, and offsets
 * to -4096..4096. Drawing, including shadows, is clipped to the canvas buffer:
 * leave room around the rectangle. It paints once; later style changes affect
 * only later calls, not already drawn pixels. No buffer yet means no drawing.
 * Available since API 1.1. w: Positive pixels, no wider than the canvas. h:
 * Positive pixels, no taller than the canvas. style: A shared style from
 * ui_style_create, painted as it is now.
 */
GADGET_IMPORT(cv_rect_style) int32_t cv_rect_style(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t style);

/*
 * A rectangle's outline, width pixels thick, inside it.
 */
GADGET_IMPORT(cv_border) int32_t cv_border(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width, int32_t color);

/*
 * A line with round ends.
 */
GADGET_IMPORT(cv_line) int32_t cv_line(int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, int32_t color);

/*
 * An arc, clockwise from start to end. r: Outer radius.
 */
GADGET_IMPORT(cv_arc) int32_t cv_arc(int32_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t width, int32_t start, int32_t end, int32_t color);

/*
 * A filled triangle.
 */
GADGET_IMPORT(cv_triangle) int32_t cv_triangle(int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t color);

/*
 * Text, wrapped at w. y: The top of the first line. w: Wrap width. Returns the
 * height drawn.
 */
GADGET_IMPORT(cv_text) int32_t cv_text(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t font, int32_t color, int32_t align, const char *text);

/*
 * Copies pixels in, clipped to the canvas. data: w * h RGB565 pixels, row by
 * row, little-endian.
 */
GADGET_IMPORT(cv_pixels) int32_t cv_pixels(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, const void *data, int32_t data_len);

/* == Muse == */
/*
 * What Muse is doing, and the talk key.
 */

/*
 * What Muse is doing now; EV_MUSE says when it changes.
 */
GADGET_IMPORT(muse_mode) int32_t muse_mode(void);

/*
 * The caption the face would show: what Muse heard, or an error. Returns the
 * caption's full length.
 */
GADGET_IMPORT(muse_caption) int32_t muse_caption(void *buf, int32_t buf_size);

/*
 * With 1, the talk button is Muse's push-to-talk again, as outside apps, and
 * its presses don't come as EV_BUTTON. Off when the app starts.
 */
GADGET_IMPORT(muse_talk_key) void muse_talk_key(int32_t on);

/* == Chat == */
/*
 * Following the chat and sending to it, through the gadget's link to the VM.
 * The gadget parses the VM's formats; the app sees messages.
 */

/*
 * Starts following the chat: fetches the latest message, then reports new ones
 * as EV_CHAT. The gadget reconnects by itself. Needs the chat.read permission.
 */
GADGET_IMPORT(chat_open) int32_t chat_open(void);

/*
 * Stops following the chat and frees its messages. Needs the chat.read
 * permission.
 */
GADGET_IMPORT(chat_close) void chat_close(void);

/*
 * The app keeps the last chat_messages messages. n: 0 for the newest, 1 for
 * the one before... Returns A message id, or 0 if there's no such message.
 * Needs the chat.read permission.
 */
GADGET_IMPORT(chat_latest) int32_t chat_latest(int32_t n);

/*
 * About a message. id: A message id. Needs the chat.read permission.
 */
GADGET_IMPORT(chat_info) int32_t chat_info(int32_t id, int32_t key);

/*
 * A message's text, without attachments or markup the gadget can't show.
 * offset: Byte offset into the text. buf: Gets the text from offset, cut to
 * fit and NUL-terminated. Returns bytes copied, not counting the NUL. Needs
 * the chat.read permission.
 */
GADGET_IMPORT(chat_text) int32_t chat_text(int32_t id, int32_t offset, void *buf, int32_t buf_size);

/*
 * Sends a message to Muse, as "[<app> on the gadget] text". EV_DONE when the
 * VM has it; the reply comes through chat_open. Needs the chat.send
 * permission.
 */
GADGET_IMPORT(chat_send) int32_t chat_send(const char *text);

/* == Files == */
/*
 * Reading files on the VM (the app's own folder, or the home folder with the
 * files.home permission) and the gadget's own files on its flash, which Muse
 * puts there with files.put (paths from "/", with the files.gadget
 * permission).
 */

/*
 * Reads part of a file on the VM, or on the gadget itself (a path from "/").
 * EV_DONE's b is the bytes read, 0 at the end of the file, or ERR_NOT_FOUND.
 * path: Relative to the app's folder, from "~/" for the home folder
 * (files.home), or from "/" for the gadget's own files (files.gadget). buf:
 * Gets the bytes; keep it reserved until EV_DONE.
 */
GADGET_IMPORT(file_read) int32_t file_read(const char *path, int32_t offset, void *buf, int32_t buf_size);

/* == State == */
/*
 * Small files the app keeps between runs, on the VM under the gadget's folder
 * (devices/<gadget>/state/<app>/).
 */

/*
 * Saves a state file on the VM, replacing the last one whole. EV_DONE when
 * it's written. name: a-z, 0-9, '-', '_' and '.'. data: Copied during the
 * call.
 */
GADGET_IMPORT(state_save) int32_t state_save(const char *name, const void *data, int32_t data_len);

/*
 * Loads a state file. EV_DONE's b is its size, or ERR_NOT_FOUND. buf: Keep it
 * reserved until EV_DONE.
 */
GADGET_IMPORT(state_load) int32_t state_load(const char *name, void *buf, int32_t buf_size);

/* == Devices == */
/*
 * Sensors, motors, relays and anything else the firmware has a driver for. The
 * firmware describes each device: a name, its readings (fields with units),
 * its actions (with their parameters) and its events. Apps find devices by
 * name and read, stream or call them through these few functions, so new
 * hardware needs no new imports and no change to this file. Well-known names
 * (Devices, below) have fixed fields and units so apps work across boards; any
 * other name carries its own description.
 */

/*
 * With dev_name, lists them all. Returns how many devices this gadget has.
 */
GADGET_IMPORT(dev_count) int32_t dev_count(void);

/*
 * A device's name, by position. index: 0 to dev_count() - 1. buf: Gets the
 * name, NUL-terminated. Returns the name's length.
 */
GADGET_IMPORT(dev_name) int32_t dev_name(int32_t index, void *buf, int32_t buf_size);

/*
 * Finds a device by name. ERR_DENIED if the app's manifest names it in neither
 * requires nor uses. name: A well-known name (DEV_ constants) or one the
 * gadget lists. Returns the device.
 */
GADGET_IMPORT(dev_find) int32_t dev_find(const char *name);

/*
 * The device's description, as device.guide gives it (Devices, below). Apps
 * written for one device rarely need it: Muse already read it. buf: Gets the
 * description as JSON, cut to fit and NUL-terminated. Returns the
 * description's full length.
 */
GADGET_IMPORT(dev_describe) int32_t dev_describe(int32_t device, void *buf, int32_t buf_size);

/*
 * Where a field is in a reading. Returns the field's index in a reading's
 * values.
 */
GADGET_IMPORT(dev_field) int32_t dev_field(int32_t device, const char *name);

/*
 * The latest reading, without opening the device. ERR_NOT_FOUND until there is
 * one: a sensor that streams starts on the first dev_read (taking a moment),
 * and runs until the app stops or calls dev_close. Its t_ms says how fresh it
 * is. To get every reading, use dev_open and dev_take instead. reading: One
 * gadget_reading_t. Returns how many values it holds.
 */
GADGET_IMPORT(dev_read) int32_t dev_read(int32_t device, void *reading, int32_t reading_size);

/*
 * Starts streaming readings into a queue, and the device's events. EV_DEV_DATA
 * says when readings wait, EV_DEV_EVENT brings events. Close it once the app
 * stops needing it: a running sensor costs battery. rate_hz: Up to the
 * device's rate; 0 for events only.
 */
GADGET_IMPORT(dev_open) int32_t dev_open(int32_t device, int32_t rate_hz);

/*
 * Takes waiting readings off the queue. readings: gadget_reading_t array.
 * Returns readings copied, oldest first.
 */
GADGET_IMPORT(dev_take) int32_t dev_take(int32_t device, void *readings, int32_t readings_size);

/*
 * Stops streaming and events.
 */
GADGET_IMPORT(dev_close) int32_t dev_close(int32_t device);

/*
 * Finds an action by name. Returns the action's index.
 */
GADGET_IMPORT(dev_action) int32_t dev_action(int32_t device, const char *name);

/*
 * Runs an action. It doesn't block: a driver whose action takes time starts it
 * and returns. action: Its index. params: float parameters, in the order the
 * description gives. text: For actions that take text; "" otherwise. Returns
 * the action's result, 0 or more.
 */
GADGET_IMPORT(dev_call) int32_t dev_call(int32_t device, int32_t action, const void *params, int32_t params_len, const char *text);

/*
 * Finds an event by name. Returns the event's index, as EV_DEV_EVENT gives it.
 */
GADGET_IMPORT(dev_event) int32_t dev_event(int32_t device, const char *name);

/* == What the app exports == */

/*
 * Required. Called once the app is loaded: build the screen here.
 */
/* GADGET_EXPORT(on_start) void on_start(void); */

/*
 * Called for each event (EV_ constants; the table under Events says what a and
 * b hold).
 */
/* GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b); */

/*
 * Called up to sys_frame_rate times a second while the screen is on, with
 * sys_millis(). Leave it out unless the app animates something itself:
 * widgets, animations and timers don't need it.
 */
/* GADGET_EXPORT(on_frame) void on_frame(int32_t now_ms); */

/*
 * Called before the app is closed, unless it trapped. Save state here:
 * state_save calls made in on_stop finish (for up to 2 s) before the app's
 * memory is freed.
 */
/* GADGET_EXPORT(on_stop) void on_stop(int32_t reason); */

#endif /* GADGET_H */
```
<!-- END gadget.h -->

## build.py

<!-- BEGIN build.py -->
```python
#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Build a Muse gadget app (WebAssembly, apps API 1.x) from C, and check it as the gadget will.

    python3 build.py APP_FOLDER                       its .c files -> APP_FOLDER/app.wasm
    python3 build.py app.c [more.c ...] [-o app.wasm]

Uses wasi-sdk if it finds one ($WASI_SDK, ~/wasi-sdk, /opt/wasi-sdk), else Zig's
clang from the ziglang package of the Python that runs this script:

    python3 -m venv ~/gadget-sdk/venv && ~/gadget-sdk/venv/bin/pip install ziglang
    ~/gadget-sdk/venv/bin/python build.py ~/workspace/gadgets/apps/hello

gadget.h is next to this script. The app can use libc's pure functions (memcpy,
strlen, sinf, malloc) but nothing that reaches WASI, which gadgets don't offer:
stdio (printf, even snprintf) and the clock (time) do. It's compiled for what the
gadget's interpreter takes: the wasm MVP plus bulk memory, sign extension, mutable
globals and non-trapping float-to-int, as a WASI reactor (its _initialize, if
any, runs before on_start).

Then it checks the module as the gadget will, and deletes it if the gadget would
refuse it: an import from any module but "gadget", or one the API doesn't have or
with another signature; no on_start export; a manifest (GADGET_MANIFEST) that
isn't JSON or has a wrong api or name; more than 256 KB. It warns when there's no
manifest (the app then gets no permissions and no devices) and when the app calls
something whose permission the manifest doesn't ask for.

The API's functions come from api.capabilities.json: --capabilities FILE, else api.capabilities.json
beside this script or in api/ under it, else gadget.h's declarations. app.api
serves the gadget's own, in parts: joined, they're api.capabilities.json.
"""
import argparse
import glob
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FEATURES = ["-mcpu=mvp", "-mbulk-memory", "-msign-ext", "-mmutable-globals", "-mnontrapping-fptoint"]
APP_NAME = re.compile(r"^[a-z0-9][a-z0-9-]{0,30}$")
LIST_MAX, ENTRY_MAX = 16, 31   # a manifest's requires or uses: entries, characters each
VALTYPES = {0x7F: "i", 0x7E: "I", 0x7D: "f", 0x7C: "F"}


# ---- The module ----

def leb(b, i):
    result = shift = 0
    while True:
        byte = b[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, i


def name_at(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode("utf-8", "replace"), i + n


def limits_at(b, i):
    flags, i = leb(b, i)
    _, i = leb(b, i)
    if flags & 1:
        _, i = leb(b, i)
    return i


def read_module(data):
    """Imports as (module, name, kind, params, results), exported functions, custom sections."""
    if data[:8] != b"\0asm\x01\0\0\0":
        raise ValueError("not a WebAssembly module")
    types, imports, exports, customs = [], [], set(), {}
    i = 8
    while i < len(data):
        sid = data[i]
        size, j = leb(data, i + 1)
        end = j + size
        if sid == 0:
            name, k = name_at(data, j)
            customs[name] = data[k:end]
        elif sid == 1:
            count, k = leb(data, j)
            for _ in range(count):
                k += 1                                   # 0x60: a function type
                n, k = leb(data, k)
                params = "".join(VALTYPES.get(data[k + x], "?") for x in range(n))
                k += n
                n, k = leb(data, k)
                results = "".join(VALTYPES.get(data[k + x], "?") for x in range(n))
                types.append((params, results))
                k += n
        elif sid == 2:
            count, k = leb(data, j)
            for _ in range(count):
                mod, k = name_at(data, k)
                name, k = name_at(data, k)
                kind = data[k]
                k += 1
                params = results = ""
                if kind == 0:                            # function: its type
                    t, k = leb(data, k)
                    params, results = types[t]
                elif kind == 1:                          # table: element type, limits
                    k = limits_at(data, k + 1)
                elif kind == 2:                          # memory: limits
                    k = limits_at(data, k)
                else:                                    # global: type, mutability
                    k += 2
                imports.append((mod, name, kind, params, results))
        elif sid == 7:
            count, k = leb(data, j)
            for _ in range(count):
                name, k = name_at(data, k)
                kind = data[k]
                _, k = leb(data, k + 1)
                if kind == 0:
                    exports.add(name)
        i = end
    return imports, exports, customs


# ---- The API ----

def wamr_to_wasm(sig):
    """A WAMR signature, "(ii$*~)i", as wasm types: ("iiiii", "i")."""
    params, result = sig[1:].split(")")
    return params.replace("*~", "ii").replace("$", "i").replace("*", "i"), result


class Api:
    def __init__(self, capabilities_file=None):
        self.version, self.functions, self.perms, self.module_bytes = None, {}, {}, 256 * 1024
        self.source = None
        header = os.path.join(HERE, "gadget.h")
        text = open(header, encoding="utf-8").read() if os.path.exists(header) else ""
        # Which calls need which permission: gadget.h's comments say.
        for comment, name in re.findall(r"/\*((?:(?!\*/).)*)\*/\s*GADGET_IMPORT\((\w+)\)", text, re.S):
            m = re.search(r"Needs the (\S+) permission", " ".join(comment.replace("*", " ").split()))
            if m:
                self.perms[name] = m.group(1)
        found = [capabilities_file] if capabilities_file else [os.path.join(HERE, "api.capabilities.json"),
                                               os.path.join(HERE, "api", "api.capabilities.json")]
        for path in found:
            if path and os.path.exists(path):
                capabilities = json.load(open(path, encoding="utf-8"))
                self.version = capabilities["api"]
                self.functions = {n: wamr_to_wasm(s) for n, s in capabilities["functions"].items()}
                self.module_bytes = capabilities.get("limits", {}).get("module_bytes", self.module_bytes)
                rel = os.path.relpath(path)
                self.source = os.path.abspath(path) if rel.startswith("..") else rel
                return
            if capabilities_file:
                sys.exit("No %s" % capabilities_file)
        if not text:
            sys.exit("No gadget.h next to build.py, and no api.capabilities.json: save gadget.h there.")
        major = re.search(r"#define GADGET_API_MAJOR (\d+)", text)
        minor = re.search(r"#define GADGET_API_MINOR (\d+)", text)
        if not (major and minor):
            sys.exit("gadget.h next to build.py isn't an apps API 1.x header")
        self.version = "%s.%s" % (major.group(1), minor.group(1))
        for ret, name, params in re.findall(r"GADGET_IMPORT\(\w+\)\s+(\w+)\s+(\w+)\(([^)]*)\);", text):
            args = [] if params.strip() == "void" else params.split(",")
            wasm = "".join("I" if a.split()[0] in ("int64_t", "uint64_t") else
                           "f" if a.split()[0] == "float" else "i" for a in args)
            self.functions[name] = (wasm, "" if ret == "void" else "I" if ret == "int64_t" else "i")
        self.source = "gadget.h"


# ---- Building ----

def compiler():
    """The command that compiles for wasm32-wasi, and its name."""
    for d in (os.environ.get("WASI_SDK"), os.path.expanduser("~/wasi-sdk"), "/opt/wasi-sdk"):
        if d and os.path.exists(os.path.join(d, "bin", "clang")):
            return [os.path.join(d, "bin", "clang"), "--target=wasm32-wasip1",
                    "--sysroot=" + os.path.join(d, "share", "wasi-sysroot")], "wasi-sdk"
    if importlib.util.find_spec("ziglang"):
        return [sys.executable, "-m", "ziglang", "cc", "-target", "wasm32-wasi"], "zig"
    sys.exit("No compiler: pip install ziglang (into the Python that runs this script), or unpack wasi-sdk\n"
             "(https://github.com/WebAssembly/wasi-sdk/releases) and set WASI_SDK to it.")


def check(path, api):
    """What the gadget would refuse (errors) and what's likely a mistake (warnings)."""
    data = open(path, "rb").read()
    errors, warnings = [], []
    try:
        imports, exports, customs = read_module(data)
    except (ValueError, IndexError) as e:
        return ["%s isn't a module the gadget can read: %s" % (path, e)], []
    if len(data) > api.module_bytes:
        errors.append("%d bytes: the gadget takes up to %d" % (len(data), api.module_bytes))
    foreign = ["%s.%s" % (m, n) for m, n, _, _, _ in imports if m != "gadget"]
    if foreign:
        errors.append("imports %s, which gadgets don't offer: printf, snprintf, time and other libc calls that\n"
                      "  need WASI pull these in. Use only gadget.h and pure libc functions." % ", ".join(foreign))
    called = []
    for mod, name, kind, params, results in imports:
        if mod != "gadget":
            continue
        if kind != 0:
            errors.append("imports gadget.%s, which isn't a function" % name)
        elif name not in api.functions:
            errors.append("calls %s, which API %s doesn't have (%s)" % (name, api.version, api.source))
        elif (params, results) != api.functions[name]:
            want = api.functions[name]
            errors.append("calls %s as (%s)%s, but the API's is (%s)%s: declare it as gadget.h does"
                          % (name, params, results, want[0], want[1]))
        else:
            called.append(name)
    if "on_start" not in exports:
        errors.append("exports no on_start: GADGET_EXPORT(on_start) void on_start(void) { ... }")

    raw = customs.get("gadget.manifest")
    if raw is None:
        warnings.append('no manifest, so the app gets no permissions and no devices: add\n'
                        '  GADGET_MANIFEST("{\\"api\\":\\"%s\\",\\"name\\":\\"my-app\\"}");' % api.version)
        return errors, warnings
    try:
        manifest = json.loads(raw.rstrip(b"\0").decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        return errors + ["the manifest isn't JSON (%s): %r" % (e, raw[:80])], warnings
    if not isinstance(manifest, dict):
        return errors + ["the manifest isn't a JSON object"], warnings
    m = re.match(r"^(\d+)\.(\d+)$", str(manifest.get("api", "")))
    major, minor = (int(x) for x in api.version.split("."))
    if not isinstance(manifest.get("api"), str) or not m:
        errors.append('the manifest\'s api must be a version string: "%s"' % api.version)
    elif int(m.group(1)) != major or int(m.group(2)) > minor:
        errors.append("the manifest asks for API %s; this is %s" % (manifest["api"], api.version))
    name = manifest.get("name")
    if not isinstance(name, str) or not APP_NAME.match(name):
        errors.append("the manifest's name must be the app's folder name: a-z, 0-9 and '-', up to 31 characters")
    if "title" in manifest and not isinstance(manifest["title"], str):
        errors.append("the manifest's title must be a string")
    lists = {}
    for key in ("requires", "uses"):
        items = manifest.get(key, [])
        if not (isinstance(items, list) and len(items) <= LIST_MAX and
                all(isinstance(x, str) and 0 < len(x) <= ENTRY_MAX for x in items)):
            errors.append("the manifest's %s must be a list of up to %d names of up to %d characters"
                          % (key, LIST_MAX, ENTRY_MAX))
            items = []
        lists[key] = items
    for fn in called:
        perm = api.perms.get(fn)
        if perm and perm not in lists["uses"]:
            warnings.append('calls %s, which needs %s: add "%s" to the manifest\'s uses, or the gadget answers '
                            "ERR_DENIED" % (fn, perm, perm))
    folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
    if os.path.basename(path) == "app.wasm" and isinstance(name, str) and folder != name:
        warnings.append("the manifest's name is %s, but the app's folder is %s: make them the same" % (name, folder))
    return errors, warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+", help="an app's folder, or its .c files")
    ap.add_argument("-o", "--output", help="default: app.wasm in the app's folder, else NAME.wasm here")
    ap.add_argument("--stack", type=int, default=8192, help="the app's own stack in its memory, bytes")
    ap.add_argument("--capabilities", help="the API to check against: api.capabilities.json, from app.api or the gadget SDK")
    args = ap.parse_args()
    sources, out = args.sources, args.output
    if len(sources) == 1 and os.path.isdir(sources[0]):
        folder = sources[0]
        sources = sorted(glob.glob(os.path.join(folder, "*.c")))
        if not sources:
            sys.exit("No .c files in %s" % folder)
        out = out or os.path.join(folder, "app.wasm")
    out = out or os.path.splitext(os.path.basename(sources[0]))[0] + ".wasm"
    api = Api(args.capabilities)
    cc, name = compiler()
    cmd = [*cc, "-mexec-model=reactor", "-O2", *FEATURES, "-I", HERE, "-Wall", "-Wextra",
           "-Wl,--no-entry", "-Wl,--gc-sections", "-Wl,--strip-debug", "-Wl,-z,stack-size=%d" % args.stack,
           *sources, "-o", out]
    if subprocess.run(cmd).returncode:
        return 1
    errors, warnings = check(out, api)
    for w in warnings:
        print("%s: warning: %s" % (out, w), file=sys.stderr)
    if errors:
        os.remove(out)
        for e in errors:
            print("%s: %s" % (out, e), file=sys.stderr)
        print("%s: removed: the gadget would refuse it" % out, file=sys.stderr)
        return 1
    with open(out, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()  # app.install's sha256, if you want the gadget to check it
    print("%s: %d bytes (%s; API %s from %s), sha256 %s" % (out, os.path.getsize(out), name, api.version, api.source,
                                                           digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```
<!-- END build.py -->

## hello.c

<!-- BEGIN hello.c -->
```c
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The smallest useful gadget app: a greeting, a spinner and the seconds it has
 * run. A tap or a button press changes the colour. Widgets do the drawing, a
 * timer does the counting: no on_frame. Build: python3 build.py hello.c
 */
#include "gadget.h"

GADGET_MANIFEST("{\"api\":\"1.0\",\"name\":\"hello\",\"title\":\"Hello\",\"icon\":\"smile\",\"color\":\"green\"}");

static const int32_t colors[] = {0x5FA87D, 0x5E7FA6, 0xC7A252, 0xC05A50};
static int32_t spinner, count, color, seconds;

/* The count as text, without snprintf (libc's stdio doesn't belong in apps). */
static void show_seconds(void)
{
    char buf[12], *p = buf + sizeof buf - 1;
    int32_t s = seconds;
    *p = '\0';
    do *--p = (char)('0' + s % 10); while ((s /= 10) && p > buf);
    ui_set_text(count, P_TEXT, p);
}

static void show_color(void)
{
    ui_style(spinner, PART_INDICATOR, P_ARC_COLOR, colors[color]);
    ui_set(count, P_TEXT_COLOR, colors[color]);
}

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
    int32_t size = (w < h ? w : h) * 3 / 4;
    ui_set(UI_ROOT, P_BG_COLOR, 0x121214);

    spinner = ui_create(W_SPINNER, UI_ROOT);
    ui_set(spinner, P_WIDTH, size);
    ui_set(spinner, P_HEIGHT, size);
    ui_set(spinner, P_ARC_COLOR, 0x23232B);     /* the track: style properties set this way are the main part's */
    ui_align(spinner, ALIGN_CENTER, 0, 0);

    int32_t title = ui_create(W_LABEL, UI_ROOT);
    ui_set_text(title, P_TEXT, "Hello from Muse");
    ui_set(title, P_TEXT_FONT, FONT_20);
    ui_set(title, P_TEXT_COLOR, 0xD8D5CC);
    ui_align(title, ALIGN_CENTER, 0, -36);

    count = ui_create(W_LABEL, UI_ROOT);
    ui_set(count, P_TEXT_FONT, FONT_48);
    ui_align(count, ALIGN_CENTER, 0, 20);

    show_color();
    show_seconds();
    sys_timer(1, 1000, 1);
    sys_log("hello: started");
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    (void)a;
    if (type == EV_TIMER) {
        seconds++;
        show_seconds();
    } else if (type == EV_TOUCH_DOWN || (type == EV_BUTTON && b == BTN_DOWN)) {
        color = (color + 1) % 4;
        show_color();
    }
}
```
<!-- END hello.c -->
