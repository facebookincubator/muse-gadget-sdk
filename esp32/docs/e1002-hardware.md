# reTerminal E1001/E1002 hardware interfaces

Build from `esp32/` with `tools/board.sh reterminal-e1001 build` or
`tools/board.sh reterminal-e1002 build`. Both profiles use the shared
`reterminal_e100x_*` modules; KEY0 retains Muse pairing and long-press setup
reset. Sources for the wiring are Seeed's E1001/E1002 V1.2 schematics and
`reTerminal-E10xx-Firmware` board profiles. Adapted battery curves and the
UC8179 four-gray waveform retain Seeed's MIT notices in the source files.

| Hardware | Connection | Link interface |
| --- | --- | --- |
| KEY0/1/2 | GPIO3/4/5, active low | `buttons.read`, `events.read/wait` |
| SHT4x, PCF8563 | I2C SDA19/SCL20, 0x44/0x51 | `sensors.read`, `rtc.read/set` |
| Battery | ADC GPIO1, divider enable GPIO21 | `battery.read`, `device.health` |
| Buzzer | GPIO45 | `buzzer.beep` |
| PDM mic | EN38, CLK42, DATA41 | `microphone.record` |
| SD | SPI2 GPIO7/8/9, CS14, DET15, EN16 | `sd.*` |

Data replies use `{"ok":true,"payload":{...}}`. Hardware file, RTC, buzzer
and microphone operations share a serialized worker and may return `busy`.
Both boards keep `display.draw_url` with `url` and optional `row`, and
`display.draw_file` with VM `path` and optional `row`. The latter reads over
the active Noise session. `power.status` reports online power-management
state; neither board exposes a Deep Sleep command.

- `buttons.read` returns each key's current state and press count since boot.
- `battery.read` returns cached `battery_mv` and estimated `battery_pct`.
  Failed or stale readings are null; these boards do not report charging.
- `rtc.read` returns calendar fields and `valid`. `rtc.set` requires
  `year`, `month`, `day`, `hour`, `minute`, `second` (2000–2099).
- `buzzer.beep` takes `frequency_hz` (100–5000) and `duration_ms` (20–1000).
- `microphone.record` takes `duration_ms` (100–500) and returns 16 kHz,
  mono, signed little-endian PCM16 as `audio_base64`.
- `sd.status` reports card detect, FAT mount and capacity. `sd.list`
  accepts an optional relative `path` and returns at most 32 names.
  `sd.read_text/write_text` use relative paths and at most 2048 text bytes.
  `sd.read` returns at most 1024 bytes as `data_base64` with `next_offset`
  and `eof`; `sd.write` accepts 1–1024 decoded bytes and an optional
  `offset`. Failed mounts do not format the card.

## Events

`events.read` returns retained `events` and `next_sequence`; pass
`{"after":0}` to read from this boot. To listen, repeatedly call
`events.wait` with the last `next_sequence`, for example
`{"after":123,"timeout_ms":25000}`. These are command results, not unsolicited
Link messages. One wait may be pending; a second returns `busy`.

`button.changed` records `key`, `gpio`, `pressed` and `presses`;
`sd.changed` records GPIO15 and `inserted`. Records include `sequence`
and `uptime_ms`. The latest 32 are retained in RAM; at most 16 are returned
per call. `has_more`, `history_lost` and `cursor_reset` let a caller drain
or reset its cursor. History is lost on reboot. A pending wait is bound to
its original Noise session.

## E1001 four-gray display

E1001 keeps `display.draw_url` for black-and-white images. Its
`display.draw_gray4_url` takes `url` and optional `row` for baseline JPEG
or raw RGB565 and returns `gray_levels: 4`. The panel shows black, dark gray,
light gray and white with one full UC8179 gray refresh. The gray waveform
comes from Seeed_GFX2 and is in `main/uc8179_gray4_lut.h` under MIT.
