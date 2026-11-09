# Waveshare ESP32-S3-Touch-LCD-1.85B

The 1.85B uses an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM, a round
360×360 ST77916 QSPI LCD, CST816S touch, ES8311 speaker and ES7210 microphones.
It has a BQ27220 battery gauge. This is a different board from the 1.85C:
LCD and touch resets are direct GPIOs, and battery readings come from I2C,
not the 1.85C's ADC divider.

## Build and flash

Use ESP-IDF v6.0.3, then follow the SDK token and backup instructions in
[`../AGENTS.md`](../AGENTS.md). The USB console is the chip's native
USB Serial/JTAG (`303a:1001`); that USB ID alone does not identify the model.

```sh
tools/muse/board.sh build 185b
tools/muse/ports.py --list
tools/muse/board.sh flash 185b DEVICE_USB_SERIAL
```

The overlay is `sdkconfig.muse-waveshare-s3-185b`, loaded after
`sdkconfig.defaults` and `sdkconfig.muse`. The standard 16 MB Muse partition
table provides two 4 MB app slots. The board is also included in the CI build
matrix and web flasher catalog.

## Hardware reference

The pin map and peripheral setup come from [Waveshare's BSP](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/tree/7232af4c501c64d34215b73e1d8319376dfd9c19/examples/esp-idf/05_bq27220/components/esp32_s3_touch_lcd_1_85B).
The panel's two initialization sequences were copied from the earlier
ESP-IDF-V5.5.3 BSP, under its Apache-2.0 license. Register `0x04` selects the
sequence, as in Waveshare's implementation.

| Peripheral | Pins / address |
|---|---|
| LCD QSPI | CS 21, clock 40, data 46/45/42/41, reset 3, backlight 5 |
| Shared I2C | SDA 11, SCL 10 |
| Touch | Reset 1, interrupt 4 |
| Audio I2S | MCLK 2, BCLK 48, WS 38, speaker data 47, microphone data 39 |
| Amplifier | Enable 9 |
| BOOT | GPIO0, active low; push-to-talk and setup confirmation |
| Battery gauge | BQ27220 at I2C address `0x55`, read at 100 kHz |

Power off darkens the display, stops the codecs and amplifier, and enters
deep sleep until BOOT. It does not claim to disconnect the battery or achieve
the vendor firmware's standby current. The physical PWR button is not a Muse
input. The SD slot, RTC and IMU are not used by this port.

The port has no display pause or light sleep: with the screen off on
battery, LVGL and the CPU keep running, so standby lasts less than on the
1.85C.

## Battery reporting

The board reads the BQ27220's battery presence, initialization state,
state of charge, voltage and signed current. The register definitions are in
[TI's BQ27220 technical reference](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf),
sections 2.6–2.8, 2.21 and 2.27.

Percent is the gauge's estimate; its accuracy depends on the battery profile
already in the gauge. Muse does not unseal, calibrate, reset or program the
gauge, and does not assume every unit has the same capacity. A missing battery,
uninitialized gauge, invalid sample or failed I2C read produces an unavailable
reading rather than a fabricated zero or a stale percentage. Charging requires
positive current and a clear discharge flag; an idle/full battery is not
automatically marked charging.

The USB flag uses native USB host detection. It follows connection to a
computer, but does not detect a charge-only adapter's VBUS. Charging current
is still read independently. Consequently the battery runtime meter may count
time on a charge-only adapter as battery time.

## Validation

On two 1.85B units, the initial board port boots with 16 MB flash and 8 MB
PSRAM, selects panel revision 2 (`00 02 7f 7f`), registers display/touch and
initializes both audio codecs. The shared Muse runtime reaches ready-to-pair
without a panic or reboot loop. Recording/playback, pairing, OTA and
power-off/wake have not yet been exercised for this contribution.

Battery-reader host tests cover valid endpoints, signed current, relaxation,
missing batteries, uninitialized/config-update states, invalid data, every
register-read failure and recovery. On two units the battery-enabled firmware
reports 19% / 4,187 mV and 9% / 3,805 mV respectively, with positive charging
current while attached to a USB host. Serial status confirms both percentages.
Calibration accuracy has not yet been verified.

The final initialization error checks and display-window alignment callback
were subsequently flashed to one of those units. App and OTA-selector flash
digests matched; a 30-second boot capture showed normal display/touch/audio
initialization and no panic or reboot loop. The gauge reported 13% / 3,937 mV,
charging, and serial status confirmed 13%. The device owner confirmed normal
display rendering and touch navigation into Settings. With USB unplugged for
20 seconds, the unit stayed on and the charging symbol disappeared; reconnecting
USB restored the symbol. Board-callback tests additionally cover pixel alignment
at every screen position and gauge allocation failure/recovery.

ESP-IDF v6.0.3 builds pass for this profile and the 1.75C regression profile,
with 48% of the 4 MB app slot free. The host suite passes (1,045 tests; two
optional dependency checks skipped), as do all 41 web-flasher tests.
