# Web flasher

A page on GitHub Pages that flashes Muse Gadget firmware from Chrome or Edge
over Web Serial, using [ESP Web Tools](https://esphome.github.io/esp-web-tools/).
The ESP32 CI (`.github/workflows/esp32.yml`) packages the boards listed in
`boards.json` from its own builds, assembles the site on every run, and deploys
it on pushes to `main`.

## The SDK token

The site's builds have no SDK token, and a gadget needs one to pair. After
installing, the page asks for the maker's token and sends it to the gadget
over serial as `>sdk_token=mgst_…` (`sdk-token.js`). The gadget stores it in
NVS, answers `@sdk_token ok` and restarts with it; NVS survives updates from
the page, so it's needed once per gadget. The firmware side is
`esp32/main/sdk_token_console.h`: Muse boards take the command on their own
console, other boards on the console UART and the chip's USB Serial/JTAG
port. It's serial-only, so it can't be set over BLE.

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
  still closing one, Connect & install and Send token are off (their port
  pickers need the click itself, so the card can't close first), and while the
  token goes out the card can't connect.
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
`boards.json` files over the unpacked site and reload.

Run the offline regression tests (the installer hooks, the Device settings
protocol and the languages) with Node.js 22.7 or later, which loads the
page's modules as ES modules (no board or package installation needed):

```sh
node --test web-flasher/tests/*.test.mjs
```
