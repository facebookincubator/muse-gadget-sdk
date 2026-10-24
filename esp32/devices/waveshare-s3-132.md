<!-- SPDX-License-Identifier: Apache-2.0 -->

# Waveshare ESP32-S3-Touch-AMOLED-1.32 (experimental)

This profile targets the measured ESP32-S3-PICO-1 revision 0.2 unit with
**8 MB flash in QIO mode** and **8 MB octal PSRAM at 80 MHz**. It drives the
466×466 round AMOLED, CST820 touch controller, ES8311 codec, and two physical
buttons through a dedicated
[`board_waveshare_s3_132.c`](../components/muse/boards/board_waveshare_s3_132.c)
backend. The tested setup used a USB data host, with no external speaker or
battery connected.

The validation below was completed on 2026-10-24 using Muse baseline
[`e6fe5174092702ffddae6f90759a38d55a4ebae1`](https://github.com/facebookincubator/muse-gadget-sdk/commit/e6fe5174092702ffddae6f90759a38d55a4ebae1)
plus the local 1.32 board changes and **ESP-IDF v6.0.1**. Later Muse revisions,
including `c9354b7`, have not been built or tested with this port in that
validation. The experimental label remains appropriate while the full Muse,
acoustic, and battery checks are outstanding.

## Hardware map

GPIO numbers below match the board backend and the fixed vendor sources listed
under [Sources](#sources). I2C addresses are 7-bit.

| Function | Wiring / configuration |
|---|---|
| Display | 1.32-inch round AMOLED, 466×466, RGB565, QSPI at 40 MHz |
| Display bus | SPI2; CS GPIO10, CLK GPIO11, D0/D1/D2/D3 GPIO12/13/14/15 |
| Display reset / TE | GPIO8 / GPIO9 |
| Display driver | `espressif/esp_lcd_sh8601` `2.0.1~1`; horizontal RAM offset 6; MADCTL `0xC0` |
| Shared I2C | SDA GPIO47, SCL GPIO48 |
| Touch | CST820 at `0x15`; reset GPIO7, interrupt GPIO6 configured as an input; LVGL polls one touch point |
| Audio codec | ES8311 at `0x18`, I2S0; left input slot is the microphone, right slot is the DAC reference |
| Audio I2S | MCLK GPIO38, BCLK GPIO39, WS GPIO41, DIN GPIO40, DOUT GPIO42 |
| Speaker amplifier | GPIO46, active high; configured PA gain 6 dB |
| PWR / BOOT | GPIO17 / GPIO0, both active low |
| Battery rail hold | GPIO18; held high during initialization |
| Battery voltage input | GPIO4, ADC1 channel 3, calibrated reading with a 1:1 divider |
| Console | Native USB Serial/JTAG |

The product description uses the CO5300 panel name. This backend follows the
vendor ESP-IDF example's SH8601-compatible QSPI command sequence. Display
commands, including brightness and sleep, share the existing LCD send task.
Touch coordinates are transformed to the display orientation, and invalid
frames release the pointer.

While a USB data host is connected, battery percentage and voltage are reported
as unknown: USB can power the measured VBAT node even without a cell. The
backend has no verified battery-presence or charging-status input. A standalone
USB charger cannot be identified by USB Serial/JTAG, so battery readings and
charging behavior in that condition remain unverified. GPIO18 rail shutdown
and battery wake have not been tested with a battery.

## Build

Activate ESP-IDF v6.0.1 and run from `esp32/`:

```sh
tools/muse/board.sh build s3-132
MUSE_BENCH=1 tools/muse/board.sh build s3-132
```

The `s3-132` alias selects target `esp32s3` and loads these files in order:

1. `sdkconfig.defaults`
2. `devices/sdkconfig.muse`
3. [`devices/sdkconfig.muse-waveshare-s3-132`](sdkconfig.muse-waveshare-s3-132)

The normal build directory is `build-muse-waveshare-s3-132`. Bench adds
`devices/sdkconfig.muse-bench` last, enables screenshots, and uses
`build-muse-waveshare-s3-132-bench`. Both profiles use the existing
[`partitions_muse_8mb.csv`](../partitions_muse_8mb.csv) layout, with two
4,063,232-byte application slots. The tested final normal image was
2,166,784 bytes, leaving 46.67% of one slot free.

Build profiles sequentially because they share managed components and the
dependency lock. Follow the [SDK setup instructions](../README.md#2-build)
for configuration. A native USB descriptor alone does not identify this board;
check the target before flashing when more than one board is attached.

## Controls

- In the tested USB setup, a BOOT short press turned the display off and a PWR
  short press woke it. Press and release were confirmed by the operator and
  firmware logs. This test does not establish battery power-off or low-power
  behavior.
- PWR is assigned to push-to-talk and BOOT to the auxiliary control. Long-press
  recording and physical confirmation during phone pairing remain untested.
- Brightness is under **Settings → Sound → Brightness** at the bottom of the
  Sound page. The exact 10%, 50%, and 100% settings were read back and observed
  on the screen; the final setting was 100%.
- Touch response and release were checked at the upper, lower, left, right,
  and center controls, then checked again after display sleep/wake.

## Validation

These results apply to one unit and the baseline above. Host checks and
software audio checks are distinct from peripheral or acoustic acceptance.

| Check | Profile / scope | Result |
|---|---|---|
| Firmware builds | Normal and bench, ESP-IDF v6.0.1 | Passed; both images were flashed and booted |
| Other full-UI board regression | StopWatch build in a separate build environment | Passed; no StopWatch hardware test |
| Full host suite | Earlier adaptation revision | 640 tests passed, 0 skipped |
| Final focused board tests | Final board changes | 11 tests passed: touch frames, serialized panel commands, battery error handling, build/configuration and USB selection |
| Final normal boot | Three 30-second observations | Passed without panic, watchdog reset, or repeated boot |
| Normal stability | 600-second USB observation, unpaired and offline | 120 heartbeats; no crash or reboot; end heap did not decrease |
| Bench display/status | Face and settings screenshots | Checked; button hints clear of status text, battery shown as unknown under USB |
| Normal display and brightness | Real screen, 10/50/100%, five seconds per level, setting readback | Passed; operator confirmed the visible changes |
| Normal touch | Edge/center controls, release, and interaction after wake | Passed by operator observation |
| Normal sleep/wake | Serial display sleep/wake, then physical BOOT/PWR short presses under USB | Passed; physical actions also confirmed in firmware logs |
| Public BLE transport | Nine connect/read/disconnect cycles | Passed; public LinkTX read and return to advertising |
| Temporary Link v5 handshake | Community mode, `none` authentication, epoch 0, `confirm_press` | Passed through authenticated `confirm_required`, then immediately disconnected |
| Handshake cleanup | Separate 120-second serial observation | No crash or reboot; device remained unpaired and without network configuration |
| Audio software path | Normal; MP3 decoding and eight codec gain settings | Passed; DAC reference in the right slot is not an acoustic loopback |
| Wi-Fi scan | Normal | Access points discovered; network connection was not tested |

The full host run preceded the final board refinements. The final 11 focused
tests and firmware rebuilds cover those changes; the 640-test result is not
presented as a fresh full-suite run on a later revision.

The temporary handshake exercised P-256 ECDH, HKDF, AES-GCM, transcript hash,
and session ID verification. It sent only a client hello and encrypted
client-finished record. It did not perform OS pairing, physical confirmation,
network configuration, credential requests, or persistent provisioning, and
does not validate manufacturer identity or complete Muse app pairing. The
independent cleanup observation recorded 1,060 bytes of minimum free NimBLE
host stack. This is a historical minimum for that run, not a margin measurement
for full provisioning or connected sessions.

The remaining acceptance checks are:

- Speaker sound, volume and mute, controlled microphone speech quality, and
  long-press recording.
- Battery voltage and percentage, supply switching, low battery, PWR shutdown
  and wake, and charging behavior.
- Phone pairing with the user's SDK token, physical pairing confirmation,
  network setup, speech recognition/replies, reconnection, and stack margin
  during complete provisioning and connected sessions.
- OTA and abnormal recovery. USB flashing does not validate OTA.
- Phone video, Muse image delivery, and the home-network tunnel. The video
  profile has not been built or run for this board.

## Sources

Hardware references are fixed to Waveshare commit
[`b3d6546e39db6287bc643eb0b0ad796e0ff61315`](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.32/tree/b3d6546e39db6287bc643eb0b0ad796e0ff61315):

- [LVGL v9 example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.32/tree/b3d6546e39db6287bc643eb0b0ad796e0ff61315/Example/ESP-IDF/07_LVGL_V9_Test): display pin map, SH8601 driver selection, touch reset and coordinates.
- [Audio board configuration](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.32/blob/b3d6546e39db6287bc643eb0b0ad796e0ff61315/Example/ESP-IDF/05_Audio_Test/components/codec_board/board_cfg.txt): ES8311 and I2S pins, amplifier pin and gain.
- [ADC example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.32/tree/b3d6546e39db6287bc643eb0b0ad796e0ff61315/Example/ESP-IDF/01_ADC_Test) and [battery/power example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.32/tree/b3d6546e39db6287bc643eb0b0ad796e0ff61315/Example/ESP-IDF/04_BATT_PWR_Test): voltage divider and rail/button behavior.

The panel initialization sequence in
[`waveshare_s3_132_display_init.h`](../components/muse/boards/waveshare_s3_132_display_init.h)
is adapted from the separately published official
[`waveshare/esp32_s3_touch_amoled_1_32` BSP v1.0.1](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_32/versions/1.0.1),
at Waveshare-ESP32-components commit
[`c77caf968fa6b11f3b6a416c853c578d012a8cea`](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/c77caf968fa6b11f3b6a416c853c578d012a8cea/bsp/esp32_s3_touch_amoled_1_32/esp32_s3_touch_amoled_1_32.c#L50-L62).
That component includes an
[Apache-2.0 license](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/c77caf968fa6b11f3b6a416c853c578d012a8cea/bsp/esp32_s3_touch_amoled_1_32/LICENSE),
retained [beside the local header](../components/muse/boards/waveshare_s3_132_display_init.LICENSE).
The registry archive and fixed Git commit contain identical source and license
files; the archive SHA-256 is
`7ba23d272c5b9abcc59e6aac612c6c5652cfdf06bedc7c181215028337fa266a`.

The eleven upstream commands retain their bytes, lengths, order and delays.
Local changes use the tested SH8601 driver's table type, replace the two
zero-length placeholder payloads with `NULL`, and append `MADCTL=0xC0` for the
tested display orientation while keeping the driver's register cache in sync.
These changes are documented in the header. The separate hardware-reference
repository above has no root license notice; it is not the license source for
this initialization sequence.
