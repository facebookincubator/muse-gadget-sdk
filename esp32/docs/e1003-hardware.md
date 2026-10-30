# reTerminal E1003 hardware interfaces

From `esp32/`, run `tools/board.sh reterminal-e1003 build` with ESP-IDF
v6.0.1. The profile uses normal Muse BLE pairing (confirm with KEY0), Wi-Fi,
Noise and Link registration. Holding KEY0 resets setup. It shares
`reterminal_e100x_*` command, event, RTC, SD and microphone modules with
E1001/E1002; E1003 uses its own IT8951 panel, GT911 touch and SY6974B PMIC.

Wiring sources: Seeed's E1003 V1.0 schematic
`202004522_reTerminal_E1003_V1.0_SCH_251231.pdf`, the
`reTerminal-E10xx-Firmware` E1003 board profile, and Seeed_GFX2
`Driver_IT8951`/`Driver_ED103TC2`. Adapted panel commands and the battery
curve retain Seeed's MIT notices in the source.

| Hardware | E1003 connection | Link interface |
| --- | --- | --- |
| ED103TC2 / IT8951 | SPI2 GPIO7/8/9, CS10, RESET12, READY13, drive EN11, TCON EN21 | `display.*` |
| SD | Shared SPI2, CS14, DET15, EN39 | `sd.*` |
| KEY0/1/2 | GPIO3/4/5 | `buttons.read`, `events.read/wait` |
| GT911 | I2C SDA19/SCL20, 0x5D, INT2, RESET48 | `touch.read`, `touch.changed` events |
| SHT4x, PCF8563, SY6974B | I2C SDA19/SCL20, 0x44/0x51/0x6B | `sensors.read`, `rtc.read/set`, `pmic.read` |
| Battery, buzzer, LED | ADC1 and EN40, buzzer45, active-low LED16 | `battery.read`, `buzzer.beep`, `led.read/set` |
| PDM mic | EN38, DATA41, CLK42 | `microphone.record` |

The shared E1001/E1002 command limits and event cursor rules are described
in [the hardware interfaces](e1002-hardware.md). E1003 adds the following:

- `touch.read` returns GT911 availability and one contact in 1872×1404
  panel coordinates. `touch.changed` reports press/release with `x`, `y`,
  `sequence` and `uptime_ms`; it does not stream drag or multi-touch samples.
- `pmic.read` reports input and charge state and raw SY6974B status/fault
  registers. It reads REG09 twice to distinguish latched and current faults.
  `device.health` reports current input/charging state without clearing the
  latched fault. Charging settings are not changed.
- `led.read/set` controls the active-low GPIO16 user LED.
- `battery_pct` uses E1003's voltage curve; it is an estimate, not a fuel
  gauge. E1003 uses GPIO40 to enable battery sensing, unlike E1001/E1002.

## Display

`display.draw_url` and `display.draw_file` replace the full monochrome
image. `draw_file` reads a VM file over the current Noise session using
`path` and optional `row`. The panel resolution is 1872×1404.
`display.draw_gray16_url` takes `url` and optional `row` to replace the
full image with 16 gray levels. Both URL paths accept baseline JPEG or raw
RGB565. A new full image starts on a white canvas.

`display.draw_partial_url` takes `url`, `x`, `y`, `width`, `height`.
The monochrome image must exactly match the requested rectangle and fit the
panel; `x` and `width` must be multiples of 16. A successful full
monochrome draw is required first, including after a gray16 draw. The tenth
successful partial request performs a full refresh. The panel driver shares
SPI2 with SD and keeps E1003's TCON/SD power sequencing board-specific.
