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

# Web flasher

A page on GitHub Pages that flashes Muse Gadget firmware from Chrome or Edge
over Web Serial, using [ESP Web Tools](https://esphome.github.io/esp-web-tools/).
The ESP32 CI (`.github/workflows/esp32.yml`) packages the boards listed in
`boards.json` from its own builds, assembles the site on every run, and deploys
it on pushes to `main`.

The M5Stack AtomS3 + Atomic Voice Base profile is experimental. It uses the
AtomS3's 8 MB flash and has no PSRAM; replies are shown as text on its display.
Choose **M5Stack AtomS3 + Atomic Voice Base (experimental)** in the board list
to flash it from Chrome or Edge.

## The SDK token

The site's builds have no SDK token, and a gadget needs one to pair. After
installing, the page asks for the maker's token and sends it to the gadget
over serial as `>sdk_token=mgst_…` (`sdk-token.js`). The gadget stores it in
NVS, answers `@sdk_token ok` and restarts with it; NVS survives updates from
the page, so it's needed once per gadget. The firmware side is
`esp32/main/sdk_token_console.h`: Muse boards take the command on their own
console, other boards on the console UART and the chip's USB Serial/JTAG
port. It's serial-only, so it can't be set over BLE.

The page keeps the token only until the tab closes, unless the maker ticks
**Remember this token on this computer**
(`token-memory.js`): off by default, with a disclaimer that the token is
saved in this browser's local storage, unencrypted. While it's ticked the
token is saved (key `muse-web-flasher:sdk-token`) once it has reached a
gadget, and a reload fills it in; unticking, **Forget saved token** and
**Clear** delete it. Older pages saved the token without asking
(`muse-sdk-token`): that copy is deleted at load, never taken as consent.
The token never reaches the session log or Save log, only its length and
SHA-256 prefix.

## Updates say what happened

- **Done only after reading the version back.** Once ESP Web Tools has
  written the firmware, the page takes over its "Installation complete!",
  closes the installer and reads the gadget's version over the same port
  (`version-read.js`, 115200, paced on boards with `pacing`): it asks with
  the console's `>version`, answered `@version X`
  (`esp32/main/sdk_token_console.c`, every board), and also takes the boot
  log's `link.app: Version: X`. It says Done only if that matches the
  manifest's `version`, which `make_manifest.py` reads from the app image
  itself; otherwise it says plainly that the update didn't take, or that it
  couldn't confirm it. Then it sends the token as before.
- **A stop says what state the gadget is in.** The page reads the version
  the gadget runs when the installer connects. Closing the installer before
  installing says "Nothing was installed; your gadget still runs X"; a
  failure before anything was written says "The update stopped before it
  finished. Nothing was installed; ..."; after an erase or a partial write
  it says the gadget may not start until it's installed again.
- **No touching during the transfer:** "Don't unplug or touch the gadget
  until this says Done." under the installer's progress and on the page,
  and leaving the page asks first.
- **Wrist-worn products** (`worn` in `boards.json`) also get "Take the
  gadget off your wrist..." beside the board and during the transfer.
- **Save log** downloads the session's log as a .txt: ESP Web Tools' and
  esptool-js's console output, the install's states, every console line the
  version reads saw, and the token step, with every secret masked as
  `esp32/tools/redact.py` masks it (length and SHA-256 prefix).

The pure logic (version compare, masking, stop messages, the log) is in
`flasher-core.js`, which `esp32/tests/test_web_flasher.py` runs in Node,
against `tools/redact.py` for the masking. What these steps say is in
`i18n.js` in English and Japanese like the rest of the page (`install.*`,
`stop.*`, `transfer.*`, `version.*`, `log.*`); the lines the page adds to the
installer's dialog follow the page's language, while the dialog's own text
stays English. The session log's own lines are English; status lines go in
as shown.

## Catching the wrong board

ESP Web Tools refuses a board whose chip doesn't match the build's. On top of
that, `flasher-hooks.js` limits the port picker to the board's `usb` devices
and, before erasing or writing, refuses a board whose flash can't hold the
build's partitions (`min_flash_bytes`, which `make_manifest.py` reads from the
partition table). It doesn't check PSRAM: esptool only sees PSRAM inside the
chip's package, and some boards have it beside the chip.

## Device settings

The Device settings card reads a gadget's settings over its USB console and
changes five of them: screen brightness, screen sleep, speaker volume, speaker
on/off and microphone gain (`device-settings.js`). It uses the console
commands boards with the full UI already have
(`esp32/components/muse/muse_input.c` and `muse_ble.c`): `>status` answers
`@status {json}`, and `>key=value` logs `cmd key=value -> ok`. Each change
waits for that line, then reads the status back and checks the value. Changes
go one at a time, and a failed one shows what the gadget has instead.

- It sends only `>status` and those five keys, as integers or booleans in the
  firmware's ranges; never Wi-Fi, pairing or token commands. The SDK token goes
  only through the token step above.
- The status shows whether the gadget has a Muse credential, never the
  credential, plus its Wi-Fi network name and IP address. The page draws
  what the gadget sends as text and keeps none of it.
- It picks the port and paces writes like the token step, from the selected
  board. Changing the board closes its port. While it holds a port, or is
  still closing one, Connect & install, Send token and Check version are off
  (their port pickers need the click itself, so the card can't close first),
  and while the token goes out or a version read holds a port the card can't
  connect.
- The `ok` line is logged at INFO, every board's default. A build that logs
  less makes each save time out rather than report success.

## Languages

The page is in English, with a Japanese switch in the header. The visitor's
choice is kept in this browser (`localStorage`); until they choose, a browser
whose first language is Japanese gets Japanese. Every piece of text is in
`i18n.js`, by key, in both languages: index.html marks elements with
`data-i18n="key"` (or `data-i18n-label` for an `aria-label`), and the scripts
call `t(key, params)`. Switching redraws all of it, status lines included.
Text is only ever set as text, never as HTML.

- Add new text to both tables. `tests/i18n.test.mjs` fails on a key missing
  from either, on placeholders (`{name}`) or code that differ, and on visible
  text in index.html that isn't in the table.
- Keep names, domains, URLs, console commands and token formats as they are.
- ESP Web Tools' install dialog has no language setting (10.4.0), so it stays
  in English; the page says so in Japanese.
- The Japanese was machine-translated and reviewed, reusing the contributor's
  own Japanese for the Device settings terms. Corrections from native
  speakers are welcome.

## Adding a board

Add an entry to `boards.json`. `board` must match a `board` in the build
matrix in `esp32.yml`; the site job fails if it doesn't.

A note shown under the picker when the board is selected (which port to pick,
backups to take first, and so on) goes in `i18n.js` as `note.<board>`, in both
languages, with `**bold**` and `` `code` ``.

| Field    | Required | Meaning |
|----------|----------|---------|
| `board`  | yes      | Board id, as in the build matrix. |
| `name`   | yes      | Name shown in the board picker. |
| `usb`    | no       | USB devices the board's console can show up as, as `"vid:pid"` strings (see `esp32/tools/muse/ports.py`). The port picker shows only these; leave it out if you're not sure, so the right port isn't hidden. |
| `pacing` | no       | Paced serial writes for USB bridges that drop bytes, as `{"chunk": 32, "factor": 2}`: bytes per write, and how many times as long as a chunk takes on the wire to wait after it. Leave it out for boards that flash fine without. |
| `worn`   | no       | `true` for a wrist-worn product: the page asks to take it off the wrist before and during the transfer, since a wake can stall the update. |

## Testing locally

Download the `github-pages` artifact from an ESP32 CI run, unpack
`artifact.tar` and serve it on localhost; Chrome allows Web Serial there
without https:

```sh
gh run download RUN_ID -n github-pages -D /tmp/flasher
mkdir /tmp/flasher/site && tar -xf /tmp/flasher/artifact.tar -C /tmp/flasher/site
python3 -m http.server 8000 --bind 127.0.0.1 --directory /tmp/flasher/site
```

To try changes to the page itself, copy `index.html` and the `.js` and
`boards.json` files over the unpacked site and reload. A manifest made before
the page read versions back says `X.Y.Z+<7 hex>`; the page matches it with the
image's `X.Y.Z+g<8 hex>` by commit prefix.

Run the offline regression tests (the installer hooks, the Device settings
protocol and the languages) with Node.js 22.7 or later, which loads the
page's modules as ES modules (no board or package installation needed):

```sh
node --test web-flasher/tests/*.test.mjs
```

The updater rules (`flasher-core.js`, `version-read.js`,
`token-memory.js`, and the page itself in a headless Chrome for Testing) are
in `esp32/tests/test_web_flasher.py`; from `esp32/`:
`python3 -m unittest -v tests.test_web_flasher`.
